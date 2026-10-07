"""Offline release packaging with original synthetic artifacts and real files."""
import argparse
import contextlib
import copy
import io
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from test_cli import manifest_fixture
import package_installer as p
import stock_migration as m
from package_controller import build_package


class Tests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.inputs = self.root / "original builds é"
        self.inputs.mkdir()
        _, self.plan, _ = manifest_fixture(self.inputs)
        # Give the valid ESP fixture a realistic release VERSION and rebuild
        # its integrity fields; never bypass the real image parser.
        image = bytearray((self.inputs / "open-keylight.bin").read_bytes())
        image[48:80] = b"0.2.0-alpha.1".ljust(32, b"\0")
        self._write_image(image)
        self.version_file = self.inputs / "VERSION"
        self.version_file.write_text("0.2.0-alpha.1\n", encoding="ascii")
        self.parent = self.root / "releases with spaces"
        self.parent.mkdir()
        self.args = argparse.Namespace(output=self.parent / "release é", source_commit="a" * 40,
            version_file=self.version_file, identity=self.inputs / "identity.oklnxp",
            off1=self.inputs / "OFF1.oklnxp", low1=self.inputs / "LOW1.oklnxp",
            lighting=self.inputs / "lighting.oklnxp", esp=self.inputs / "open-keylight.bin",
            assets=self.inputs / "asset-manifest.json")
        self.network = patch("socket.socket", side_effect=AssertionError("No network permitted"))
        self.network.start()
        self.addCleanup(self.network.stop)

    def _write_image(self, raw):
        import struct
        image = bytearray(raw)
        offset, checksum = 24, 0xEF
        for _ in range(image[1]):
            _, size = struct.unpack_from("<II", image, offset)
            offset += 8
            for value in image[offset:offset + size]:
                checksum ^= value
            offset += size
        image[-33] = checksum
        image[-32:] = bytes.fromhex(m.digest(image[:-32]))
        (self.inputs / "open-keylight.bin").write_bytes(image)

    def fails_cleanly(self, kind=ValueError):
        with self.assertRaises(kind):
            p.build_bundle(self.args)
        self.assertFalse(self.args.output.exists())
        self.assertEqual(list(self.parent.iterdir()), [])

    def test_exact_original_contents_and_portable_manifest(self):
        original = {path: path.read_bytes() for path in self.inputs.iterdir()}
        result = p.build_bundle(self.args)
        manifest = self.args.output / "bundle.json"
        document = json.loads(manifest.read_bytes())
        self.assertEqual(set(document), {"format", "product", "version", "source_commit",
                         "stock_profile", "packages", "esp", "assets"})
        self.assertEqual(document["format"], 1)
        self.assertEqual(document["product"], "open-keylight-chroma")
        self.assertEqual(document["version"], "0.2.0-alpha.1")
        self.assertEqual(document["source_commit"], "a" * 40)
        self.assertEqual(document["stock_profile"], "keylight-chroma-1.0.13")
        expected = {"identity.oklnxp", "OFF1.oklnxp", "LOW1.oklnxp", "lighting.oklnxp",
                    "open-keylight.bin", "asset-manifest.json", "bundle.json"}
        self.assertEqual({item.name for item in self.args.output.iterdir()}, expected)
        for entry in [*document["packages"].values(), document["esp"], document["assets"]]:
            self.assertEqual(set(entry), {"path", "sha256", "bytes"})
            self.assertNotIn("/", entry["path"])
            copied = (self.args.output / entry["path"]).read_bytes()
            self.assertEqual(copied, original[self.inputs / entry["path"]])
            self.assertEqual(entry["sha256"], m.digest(copied))
            self.assertEqual(entry["bytes"], len(copied))
        self.assertEqual({path: path.read_bytes() for path in self.inputs.iterdir()}, original)
        self.assertEqual(result["bundle_sha256"], m.digest(manifest.read_bytes()))
        self.assertEqual(result["files"], 7)
        self.assertEqual(result["device_operations"], 0)
        self.assertFalse(result["authenticity_verified"])
        self.assertFalse(result["hardware_qualified_by_packaging"])

    def test_public_migration_loader_accepts_exported_files_without_sources(self):
        p.build_bundle(self.args)
        bundle = json.loads((self.args.output / "bundle.json").read_bytes())
        plan = copy.deepcopy(self.plan)
        for stage in plan["packages"]:
            plan["packages"][stage] = {key: value for key, value in bundle["packages"][stage].items()
                                       if key != "bytes"}
        for key in ("esp", "assets"):
            plan[key] = {field: value for field, value in bundle[key].items() if field != "bytes"}
        # Restore is deliberately outside the release and separately supplied.
        plan["restore"]["path"] = str(self.inputs / "owner-restore.bin")
        migration = self.args.output / "local-plan.json"
        migration.write_text(json.dumps(plan))
        loaded = m.load_plan(migration)
        self.assertEqual(loaded["esp_metadata"]["version"], bundle["version"])

    def test_version_relabel_and_invalid_commit_fail_before_output(self):
        for version in ("0.2.0-alpha.2", "test", "01.2.3", "0.2.0-alpha.1\nextra", "0.2.0-" + "a" * 32):
            self.version_file.write_text(version, encoding="ascii")
            with self.subTest(version=version):
                self.fails_cleanly()
        self.version_file.write_text("0.2.0-alpha.1\n", encoding="ascii")
        for commit in ("a" * 39, "A" * 40, "g" * 40, True):
            self.args.source_commit = commit
            with self.subTest(commit=commit):
                self.fails_cleanly()

    def test_package_role_relabel_and_same_bank_cannot_be_repeated(self):
        original = self.args.identity
        self.args.identity = self.args.lighting
        self.fails_cleanly()
        self.args.identity = original
        copied = self.inputs / "copied diagnostic.oklnxp"
        copied.write_bytes(original.read_bytes())
        self.args.off1 = copied
        self.fails_cleanly()
        # A different role/version header must not hide reuse of the same bank.
        copied.write_bytes(build_package(original.read_bytes()[64:], (0, 1, 1, 0), "lighting")[0])
        self.args.off1 = self.inputs / "OFF1.oklnxp"
        self.args.lighting = copied
        self.fails_cleanly()

    def test_input_path_and_hard_link_collision(self):
        original = self.args.off1
        self.args.off1 = self.args.identity
        self.fails_cleanly()
        linked = self.inputs / "hardlinked.oklnxp"
        os.link(self.args.identity, linked)
        self.args.off1 = linked
        self.fails_cleanly()
        self.args.off1 = original

    def test_controller_corruption_bounds_vectors_abi_and_version(self):
        raw = self.args.identity.read_bytes()
        for kind in ("short", "oversize", "digest", "version", "abi", "role", "vectors", "reserved"):
            value = bytearray(raw)
            if kind == "short": value = value[:-1]
            elif kind == "oversize": value += b"x"
            elif kind == "digest": value[-1] ^= 1
            elif kind == "version": value[24:28] = bytes(4)
            elif kind == "abi": value[20] = 2
            elif kind == "role": value[22] = 0
            elif kind == "vectors":
                value[68:72] = bytes(4)
                value[28:60] = bytes.fromhex(m.digest(value[64:]))
            elif kind == "reserved": value[63] = 1
            self.args.identity.write_bytes(value)
            with self.subTest(kind=kind):
                self.fails_cleanly()
        self.args.identity.write_bytes(raw)

    def test_wrong_esp_or_oversize_input_cannot_be_published(self):
        raw = self.args.esp.read_bytes()
        for value in (b"not firmware", raw[:-1], raw + b"x", bytes(m.ESP_SLOT_BYTES + 1)):
            self.args.esp.write_bytes(value)
            self.fails_cleanly()
        self.args.esp.write_bytes(raw)

    def test_asset_mixups_schema_bounds_and_traversal(self):
        original = json.loads(self.args.assets.read_bytes())
        for kind in ("hash", "size", "boolean_version", "missing_index", "duplicate", "traversal", "oversize"):
            value = copy.deepcopy(original)
            if kind == "hash": value["files"][0]["sha256"] = "b" * 64
            elif kind == "size": value["files"][0]["size"] += 1
            elif kind == "boolean_version": value["version"] = True
            elif kind == "missing_index": value["files"] = value["files"][1:]
            elif kind == "duplicate": value["files"].append(value["files"][0])
            elif kind == "traversal": value["files"][0]["path"] = "/assets/../token"
            elif kind == "oversize": value["files"][0]["size"] = 1048577
            self.args.assets.write_text(json.dumps(value))
            with self.subTest(kind=kind):
                self.fails_cleanly()

    def test_existing_output_and_input_directory_never_changed(self):
        self.args.output.mkdir()
        with patch.object(p, "bounded_read", side_effect=AssertionError("Reject existing before inputs")):
            with self.assertRaises(FileExistsError):
                p.build_bundle(self.args)
        self.assertEqual(list(self.args.output.iterdir()), [])
        self.args.output = self.inputs
        existing = {path: path.read_bytes() for path in self.inputs.iterdir()}
        with self.assertRaises(FileExistsError):
            p.build_bundle(self.args)
        self.assertEqual(existing, {path: path.read_bytes() for path in self.inputs.iterdir()})

    def test_concurrent_empty_directory_is_not_replaced(self):
        publish = p._publish_directory
        def raced(source, destination):
            destination.mkdir()
            return publish(source, destination)
        with patch.object(p, "_publish_directory", side_effect=raced):
            with self.assertRaises(FileExistsError):
                p.build_bundle(self.args)
        self.assertEqual(list(self.parent.iterdir()), [self.args.output])
        self.assertEqual(list(self.args.output.iterdir()), [])

    def test_partial_write_flush_and_publication_failure_remove_only_temp(self):
        writer = p._write_file
        for phase in ("partial_write", "flush", "publish"):
            calls = []
            def partial(path, raw):
                calls.append(path)
                if len(calls) == 3:
                    path.write_bytes(raw[:7])
                    raise OSError("Disk full")
                writer(path, raw)
            target, effect = {"partial_write": ("_write_file", partial),
                              "flush": ("os.fsync", OSError("Cannot flush")),
                              "publish": ("_publish_directory", OSError("Cannot publish"))}[phase]
            with self.subTest(phase=phase), patch("package_installer." + target, side_effect=effect):
                self.fails_cleanly(OSError)
        self.assertTrue((self.inputs / "owner-restore.bin").is_file())

    def test_validated_snapshots_not_later_source_edits_are_copied(self):
        original = self.args.esp.read_bytes()
        write = p._write_file
        def edited(path, raw):
            self.args.esp.write_bytes(b"changed after validation")
            write(path, raw)
        with patch.object(p, "_write_file", side_effect=edited):
            p.build_bundle(self.args)
        self.assertEqual((self.args.output / "open-keylight.bin").read_bytes(), original)

    def test_cli_arguments_with_spaces_and_no_secret_or_restore_arguments(self):
        argv = [part for name, value in vars(self.args).items()
                for part in ("--" + name.replace("_", "-"), str(value))]
        with contextlib.redirect_stdout(io.StringIO()) as output:
            p.main(argv)
        result = json.loads(output.getvalue())
        self.assertEqual(result["version"], "0.2.0-alpha.1")
        self.assertFalse((self.args.output / "owner-restore.bin").exists())
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            p.main(argv + ["--token", "never copy me"])


if __name__ == "__main__":
    unittest.main()
