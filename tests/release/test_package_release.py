"""Deterministic release packaging with synthetic original firmware; no network."""
import argparse
import contextlib
import copy
import io
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
from urllib.parse import unquote, urlsplit
import zipfile

ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT / "tools"), str(ROOT / "tests/migration")]
from test_cli import manifest_fixture
import package_installer as bundle_tool
import package_release as p


class Tests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.root = self.base / "public source é"
        self.root.mkdir()
        self.input = self.base / "original builds"
        self.input.mkdir()
        _, self.plan, _ = manifest_fixture(self.input)
        image = bytearray((self.input / "open-keylight.bin").read_bytes())
        image[48:80] = b"0.2.0-dev".ljust(32, b"\0")
        offset, checksum = 24, 0xEF
        for _ in range(image[1]):
            _, length = struct.unpack_from("<II", image, offset)
            offset += 8
            for value in image[offset:offset + length]: checksum ^= value
            offset += length
        image[-33] = checksum
        image[-32:] = bytes.fromhex(p.digest(image[:-32]))
        (self.input / "open-keylight.bin").write_bytes(image)
        (self.root / "VERSION").write_text("0.2.0-dev\n", encoding="ascii")
        self.bundle = self.base / "firmware"
        bundle_tool.build_bundle(argparse.Namespace(
            output=self.bundle, source_commit="a" * 40, version_file=self.root / "VERSION",
            identity=self.input / "identity.oklnxp", off1=self.input / "OFF1.oklnxp",
            low1=self.input / "LOW1.oklnxp", lighting=self.input / "lighting.oklnxp",
            esp=self.input / "open-keylight.bin", assets=self.input / "asset-manifest.json"))
        self.installer = self.base / "installer dist"
        self.installer.mkdir()
        cli = b"import {readFile} from 'node:fs/promises'; console.log('synthetic original test');\n"
        (self.installer / "cli.js").write_bytes(cli)
        (self.installer / "package.json").write_text(json.dumps({
            "version": "0.2.0-dev", "type": "module", "engines": {"node": ">=22"}}))
        (self.installer / "THIRD_PARTY_NOTICES.txt").write_text("Synthetic notices\n")
        self.build = {"format": 1, "version": "0.2.0-dev", "node": ">=22", "bytes": len(cli), "sha256": p.digest(cli),
                      "bundled_packages": 1, "external_runtime_packages": 0}
        self.write_json(self.installer / "build.json", self.build)
        for name in (*["tools/" + name for name in p.TOOLS],
                     *["distribution/windows/" + name for name in p.LAUNCHERS],
                     *p.GUIDE_FILES, "docs/releases/0.2.0-dev.md", *p.GUIDE_IMAGES):
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("Public test content for " + name + "\n", encoding="utf-8")
        self.write_json(self.root / "distribution/windows/runtimes.json", {"format": 1, "platform": "win-x64"})
        self.output = self.base / "release.zip"
        self.network = patch("socket.socket", side_effect=AssertionError("No network permitted"))
        self.network.start()
        self.addCleanup(self.network.stop)

    @staticmethod
    def write_json(path, value): path.write_text(json.dumps(value), encoding="utf-8")

    def build_release(self, **options):
        return p.build_release(options.get("bundle", self.bundle), self.installer,
                               options.get("output", self.output), self.root)

    def rejects(self, kind=ValueError):
        with self.assertRaises(kind): self.build_release()
        self.assertFalse(self.output.exists())
        self.assertEqual(list(self.base.glob(".release.zip.*")), [])

    def test_fixed_layout_checksums_and_metadata(self):
        result = self.build_release()
        with zipfile.ZipFile(self.output) as archive:
            expected = {"firmware/bundle.json", "VERSION", *p.GUIDE_FILES, "docs/releases/0.2.0-dev.md",
                        "release.json", "SHA256SUMS", "START_HERE.txt", *p.LAUNCHERS, *p.GUIDE_IMAGES,
                        *["tools/" + name for name in p.TOOLS],
                        *["installer/" + name for name in p.INSTALLER],
                        *["firmware/" + stage + ".oklnxp" for stage in p.STAGES],
                        "firmware/open-keylight.bin", "firmware/asset-manifest.json"}
            self.assertEqual(set(archive.namelist()), expected)
            self.assertEqual(archive.namelist(), sorted(expected))
            for entry in archive.infolist():
                self.assertEqual(entry.date_time, (1980, 1, 1, 0, 0, 0))
                self.assertEqual(entry.compress_type, zipfile.ZIP_STORED)
                self.assertEqual(entry.external_attr >> 16, 0o100644)
                self.assertEqual((entry.extra, entry.comment), (b"", b""))
            sums = dict(line.split("  ", 1)[::-1] for line in archive.read("SHA256SUMS").decode().splitlines())
            self.assertEqual(set(sums), expected - {"SHA256SUMS"})
            for name, digest in sums.items(): self.assertEqual(digest, p.digest(archive.read(name)))
            release = json.loads(archive.read("release.json"))
            self.assertEqual(release["source_commit"], "a" * 40)
            self.assertEqual(release["version"], "0.2.0-dev")
            self.assertEqual(release["platform"], "win-x64")
            self.assertFalse(release["authenticity_verified"])
            self.assertFalse(release["hardware_qualified_by_packaging"])
            for name, spec in release["files"].items():
                self.assertEqual(spec, {"sha256": p.digest(archive.read(name)), "bytes": len(archive.read(name))})
            self.assertNotIn(str(self.base), archive.read("release.json").decode())
        self.assertEqual(result["sha256"], p.digest(self.output.read_bytes()))
        self.assertEqual(result["device_operations"], 0)

    def test_reproducible_despite_output_name_mtime_and_timezone(self):
        self.build_release()
        for directory in (self.bundle, self.installer):
            for item in directory.iterdir(): os.utime(item, (1700000000, 1700000000))
        with patch.dict(os.environ, {"TZ": "Pacific/Auckland"}):
            self.build_release(bundle=self.bundle / "bundle.json", output=self.base / "different.zip")
        self.assertEqual(self.output.read_bytes(), (self.base / "different.zip").read_bytes())

    def test_credentials_vendor_and_extra_build_files_never_included(self):
        secret = b"DO_NOT_PACKAGE_TEST_CREDENTIAL_OR_VENDOR_BANK"
        for directory in (self.root, self.bundle, self.installer):
            for name in ("token.json", "owner-restore.bin", "vendor.bin", "audit.jsonl", "extra.js"):
                (directory / name).write_bytes(secret)
            (directory / "nested").mkdir()
            (directory / "nested/private.txt").write_bytes(secret)
        self.build_release()
        self.assertNotIn(secret, self.output.read_bytes())
        with zipfile.ZipFile(self.output) as archive:
            self.assertFalse(any("restore.bin" in name or "token" in name for name in archive.namelist()))

    def test_optional_legal_file_and_missing_linked_notice(self):
        path = self.installer / "cli.js.LEGAL.txt"
        path.write_text("Original legal text\n")
        self.build_release()
        with zipfile.ZipFile(self.output) as archive:
            self.assertEqual(archive.read("installer/cli.js.LEGAL.txt"), path.read_bytes())
        self.output.unlink(); path.unlink()
        cli = (self.installer / "cli.js").read_bytes() + b"/* See cli.js.LEGAL.txt */"
        (self.installer / "cli.js").write_bytes(cli)
        self.write_json(self.installer / "build.json", {**self.build, "sha256": p.digest(cli), "bytes": len(cli)})
        self.rejects()

    def test_bundle_entry_traversal_alias_and_schema_denied(self):
        path = self.bundle / "bundle.json"
        document = json.loads(path.read_bytes())
        for name in ("../token.json", "/private.bin", "C:/private.bin", "..\\private.bin", "OFF1.oklnxp", "identity.oklnxp:secret"):
            value = copy.deepcopy(document); value["packages"]["identity"]["path"] = name
            self.write_json(path, value); self.rejects()
        for key, value in (("format", True), ("source_commit", "A" * 40), ("version", "0.2.1"), ("private", "secret")):
            changed = {**document, key: value}
            self.write_json(path, changed); self.rejects()

    def test_bundle_hash_length_and_controller_semantics(self):
        path = self.bundle / "bundle.json"
        document = json.loads(path.read_bytes())
        original = (self.bundle / "identity.oklnxp").read_bytes()
        for fault in ("hash", "length", "bool_length", "role", "duplicate_bank", "corrupt"):
            value = copy.deepcopy(document); raw = original
            if fault == "hash": value["packages"]["identity"]["sha256"] = "0" * 64
            elif fault == "length": value["packages"]["identity"]["bytes"] -= 1
            elif fault == "bool_length": value["packages"]["identity"]["bytes"] = True
            elif fault == "role": raw = (self.bundle / "lighting.oklnxp").read_bytes()
            elif fault == "duplicate_bank": raw = (self.bundle / "OFF1.oklnxp").read_bytes()
            elif fault == "corrupt": raw = original[:-1] + bytes([original[-1] ^ 1])
            if fault in ("role", "duplicate_bank", "corrupt"):
                value["packages"]["identity"]["sha256"] = p.digest(raw)
            (self.bundle / "identity.oklnxp").write_bytes(raw)
            self.write_json(path, value); self.rejects()

    def test_descriptor_and_embedded_assets_validated_even_when_rehashed(self):
        document = json.loads((self.bundle / "bundle.json").read_bytes())
        (self.root / "VERSION").write_text("0.2.1\n")
        self.write_json(self.bundle / "bundle.json", {**document, "version": "0.2.1"})
        self.rejects()
        (self.root / "VERSION").write_text("0.2.0-dev\n")
        asset_path = self.bundle / "asset-manifest.json"
        assets = json.loads(asset_path.read_bytes()); assets["files"][0]["sha256"] = "0" * 64
        self.write_json(asset_path, assets)
        document["assets"].update(sha256=p.digest(asset_path.read_bytes()), bytes=asset_path.stat().st_size)
        self.write_json(self.bundle / "bundle.json", document); self.rejects()

    def test_stale_installer_or_external_runtime_dependencies_denied(self):
        for key, value in (("sha256", "0" * 64), ("bytes", self.build["bytes"] + 1), ("format", True),
                           ("version", "0.1.9-dev"), ("version", True),
                           ("external_runtime_packages", 1), ("external_runtime_packages", False),
                           ("bundled_packages", True), ("node", ">=20")):
            self.write_json(self.installer / "build.json", {**self.build, key: value}); self.rejects()
        self.write_json(self.installer / "build.json", self.build)
        self.write_json(self.installer / "package.json", {"version": "0.2.0-dev", "type": "module", "engines": {"node": ">=22"},
                                                         "dependencies": {"unexpected": "1"}})
        self.rejects()

    def test_installer_version_metadata_is_required_and_consistent(self):
        missing = {key: value for key, value in self.build.items() if key != "version"}
        self.write_json(self.installer / "build.json", missing); self.rejects()
        self.write_json(self.installer / "build.json", self.build)
        for version in (None, "0.1.9-dev", True):
            package = {"type": "module", "engines": {"node": ">=22"}}
            if version is not None: package["version"] = version
            self.write_json(self.installer / "package.json", package); self.rejects()

    def test_extracted_jsonl_backend_imports_with_isolated_python_without_checkout(self):
        # Use the exact five shipped public modules, not mocked substitutes or
        # an import that can accidentally fall back to this checkout.
        for name in p.TOOLS:
            (self.root / "tools" / name).write_bytes((ROOT / "tools" / name).read_bytes())
        self.build_release()
        extracted = self.base / "extracted"
        with zipfile.ZipFile(self.output) as archive: archive.extractall(extracted)
        document = json.loads((extracted / "firmware/bundle.json").read_bytes())
        plan = copy.deepcopy(self.plan)
        for name, entry in document["packages"].items():
            plan["packages"][name] = {"path": "firmware/" + entry["path"], "sha256": entry["sha256"]}
        for name in ("esp", "assets"):
            entry = document[name]
            plan[name] = {"path": "firmware/" + entry["path"], "sha256": entry["sha256"]}
        plan["restore"]["path"] = str(self.input / "owner-restore.bin")
        self.write_json(extracted / "local-plan.json", plan)
        bootstrap = (
            "import runpy,sys,socket; from pathlib import Path; "
            "p=Path(sys.argv[1]).resolve(); sys.path.insert(0,str(p.parent)); "
            "import migration_events,migration_profiles,vendor_restore; "
            "socket.socket=lambda *a,**k: (_ for _ in ()).throw(AssertionError('No network')); "
            "assert all(Path(m.__file__).parent == p.parent for m in "
            "(migration_events,migration_profiles,vendor_restore)); "
            "sys.argv=sys.argv[1:]; runpy.run_path(str(p),run_name='__main__')")
        result = subprocess.run([sys.executable, "-I", "-S", "-X", "utf8", "-c", bootstrap,
                                 str(extracted / "tools/stock_migration.py"), "prepare",
                                 "--manifest", str(extracted / "local-plan.json"), "--events-jsonl"],
                                cwd=extracted, capture_output=True, text=True, timeout=10,
                                stdin=subprocess.DEVNULL)
        self.assertEqual(result.returncode, 0, result.stderr)
        rows = [json.loads(line) for line in result.stdout.splitlines()]
        self.assertEqual([(row["seq"], row["event"]) for row in rows], [(0, "status"), (1, "completed")])
        self.assertEqual(rows[0]["summary"]["device_operations"], 0)
        self.assertEqual(rows[0]["summary"]["esp"]["version"], "0.2.0-dev")
        self.assertEqual(rows[-1]["outcome"], "prepared")

    def test_validated_firmware_snapshot_is_not_reread(self):
        original = (self.bundle / "open-keylight.bin").read_bytes()
        validate = p.validate_assets
        def change_after_validation(raw, image):
            result = validate(raw, image)
            (self.bundle / "open-keylight.bin").write_bytes(b"changed after validated snapshot")
            return result
        with patch.object(p, "validate_assets", side_effect=change_after_validation): self.build_release()
        with zipfile.ZipFile(self.output) as archive:
            self.assertEqual(archive.read("firmware/open-keylight.bin"), original)

    def test_wrong_runtime_platform_and_missing_guide_fail_before_publication(self):
        path = self.root / "distribution/windows/runtimes.json"
        for value in ({"format": True, "platform": "win-x64"}, {"format": 1, "platform": "win-arm64"}):
            self.write_json(path, value); self.rejects()
        self.write_json(path, {"format": 1, "platform": "win-x64"})
        (self.root / p.GUIDE_IMAGES[-1]).unlink()
        self.rejects(FileNotFoundError)

    def test_public_guide_local_link_closure_is_explicit_and_complete(self):
        version = (ROOT / "VERSION").read_text().strip()
        included = {*p.GUIDE_FILES, *p.GUIDE_IMAGES, f"docs/releases/{version}.md"}
        for name in sorted(included):
            source = ROOT / name
            self.assertTrue(source.is_file(), name)
            if source.suffix != ".md": continue
            for target in re.findall(r"!?\[[^\]]*\]\(([^)]+)\)", source.read_text(encoding="utf-8")):
                target = target.split(' "', 1)[0].strip("<>")
                parsed = urlsplit(target)
                if parsed.scheme or parsed.netloc or not parsed.path: continue
                resolved = (source.parent / unquote(parsed.path)).resolve()
                self.assertTrue(resolved.is_relative_to(ROOT), (name, target))
                self.assertIn(resolved.relative_to(ROOT).as_posix(), included, (name, target))

    def test_existing_and_concurrent_destination_preserved(self):
        self.output.write_bytes(b"existing release")
        with patch.object(p, "bounded_read", side_effect=AssertionError("Read after collision")):
            with self.assertRaises(FileExistsError): self.build_release()
        self.assertEqual(self.output.read_bytes(), b"existing release")
        self.output.unlink()
        real_link = os.link
        def race(source, destination):
            Path(destination).write_bytes(b"concurrent publisher")
            return real_link(source, destination)
        with patch.object(p.os, "link", side_effect=race), self.assertRaises(FileExistsError): self.build_release()
        self.assertEqual(self.output.read_bytes(), b"concurrent publisher")
        self.assertEqual(list(self.base.glob(".release.zip.*")), [])

    def test_hardlink_collision_and_symlink_escape_denied(self):
        first = self.root / "tools/stock_migration.py"
        second = self.root / "tools/vendor_restore.py"
        original = second.read_bytes(); second.unlink(); os.link(first, second)
        self.rejects(); second.unlink(); second.write_bytes(original)
        outside = self.base / "private-secret"; outside.write_bytes(b"secret")
        second.unlink()
        try: second.symlink_to(outside)
        except OSError: self.skipTest("Host does not permit creating symlinks")
        self.rejects()

    def test_partial_write_and_no_hardlink_support_leave_no_archive(self):
        real_write = zipfile.ZipFile.writestr
        calls = 0
        def fail(archive, *args, **kwargs):
            nonlocal calls
            calls += 1
            if calls == 3: raise OSError("simulated write failure")
            return real_write(archive, *args, **kwargs)
        with patch.object(zipfile.ZipFile, "writestr", new=fail): self.rejects(OSError)
        with patch.object(p.os, "link", side_effect=OSError("unsupported")): self.rejects(OSError)

    def test_cli_offline_and_missing_file(self):
        with contextlib.redirect_stdout(io.StringIO()) as output:
            p.main(["--bundle", str(self.bundle), "--installer", str(self.installer),
                    "--output", str(self.output), "--root", str(self.root)])
        self.assertEqual(json.loads(output.getvalue())["device_operations"], 0)
        self.output.unlink()
        (self.installer / "package.json").unlink()
        self.rejects(FileNotFoundError)


if __name__ == "__main__": unittest.main(verbosity=2)
