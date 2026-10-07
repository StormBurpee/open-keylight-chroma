#!/usr/bin/env python3
"""Assemble a reproducible Windows x64 release from reviewed original builds.

Only named public inputs are included. Packaging validates consistency, not
publisher authenticity, physical qualification or the absence of source bugs.
No network or device access occurs; existing release files are never replaced.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import tempfile
import zipfile

from package_installer import VERSION_PATTERN, validate_assets
from stock_migration import (ESP_SLOT_BYTES, NXP_BANK_BYTES, _json, bounded_read,
                             digest, inspect_controller_package,
                             inspect_esp_application, require)

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ("stock_migration.py", "prepare_migration.py", "migration_profiles.py",
         "migration_events.py", "vendor_restore.py")
LAUNCHERS = ("start-open-keylight.cmd", "start-open-keylight.ps1", "runtimes.json")
INSTALLER = ("cli.js", "package.json", "THIRD_PARTY_NOTICES.txt", "build.json")
STAGES = ("identity", "OFF1", "LOW1", "lighting")
GUIDE_IMAGES = ("assets/brand/readme-hero.png", "assets/dashboard/light-live.jpg",
                "assets/dashboard/scenes-live.jpg", "assets/installer/setup-preview.png")


def _json_bytes(value: dict) -> bytes:
    return (json.dumps(value, sort_keys=True, indent=2, ensure_ascii=True,
                       allow_nan=False) + "\n").encode("ascii")


def build_release(bundle: Path, installer: Path, output: Path, root: Path = ROOT) -> dict:
    """Snapshot, validate and atomically publish one deterministic ZIP.

    ``bundle`` is bundle.json or its directory; ``installer`` is the built dist
    directory. The source root supplies VERSION, public tools, docs and launchers.
    No input is re-read after validation or selected by a wildcard. Exclusive
    hard-link publication fails closed on filesystems without hard-link support.
    """
    requested = Path(output).absolute()
    output = requested.parent.resolve(strict=True) / requested.name
    if output.exists() or output.is_symlink():
        raise FileExistsError(f"Output already exists: {output}")
    require(output.suffix.lower() == ".zip", "Release output must be a new .zip file")
    root = Path(root).resolve(strict=True)
    installer = Path(installer).resolve(strict=True)
    bundle = Path(bundle)
    if bundle.is_dir():
        bundle = bundle / "bundle.json"
    require(bundle.name == "bundle.json", "Select the validated bundle.json")
    bundle_root = bundle.parent.resolve(strict=True)
    files: dict[str, bytes] = {}
    sources: set[tuple[int, int]] = set()

    def read(base: Path, name: str, maximum: int) -> bytes:
        # All callers supply fixed names, never manifest-controlled paths.
        candidate = base / name
        require(not candidate.is_symlink(), f"Symlink input is not allowed: {name}")
        resolved = candidate.resolve(strict=True)
        require(resolved.is_relative_to(base) and resolved.is_file(), f"Input escapes its root: {name}")
        stat = resolved.stat()
        identity = (stat.st_dev, stat.st_ino)
        require(identity not in sources, f"Input file collision: {name}")
        sources.add(identity)
        raw = bounded_read(resolved, maximum)
        require(raw, f"Empty release input: {name}")
        return raw

    def put(name: str, raw: bytes) -> None:
        require(name not in files and name.lower() not in {entry.lower() for entry in files},
                "Release archive path collision")
        files[name] = raw

    version_raw = read(root, "VERSION", 64)
    version = version_raw.decode("ascii").strip()
    require(VERSION_PATTERN.fullmatch(version) and len(version) <= 31, "Invalid release VERSION")
    put("VERSION", version_raw)
    raw = read(bundle_root, "bundle.json", 32768)
    document = _json(raw)
    require(isinstance(document, dict) and set(document) == {
        "format", "product", "version", "source_commit", "stock_profile", "packages", "esp", "assets"},
        "Unexpected firmware bundle fields")
    require(type(document["format"]) is int and document["format"] == 1
            and document["product"] == "open-keylight-chroma"
            and document["stock_profile"] == "keylight-chroma-1.0.13"
            and document["version"] == version
            and isinstance(document["source_commit"], str)
            and re.fullmatch(r"[0-9a-f]{40}", document["source_commit"]), "Firmware bundle identity differs")
    require(isinstance(document["packages"], dict) and set(document["packages"]) == set(STAGES),
            "Firmware stages differ")
    put("firmware/bundle.json", raw)

    def firmware(entry: dict, name: str, maximum: int) -> bytes:
        require(isinstance(entry, dict) and set(entry) == {"path", "bytes", "sha256"}
                and entry["path"] == name and type(entry["bytes"]) is int
                and 0 < entry["bytes"] <= maximum, f"Invalid fixed bundle entry: {name}")
        value = read(bundle_root, name, maximum)
        require(len(value) == entry["bytes"] and digest(value) == entry["sha256"],
                f"Firmware bundle hash or length differs: {name}")
        put("firmware/" + name, value)
        return value

    banks = set()
    for stage in STAGES:
        package = firmware(document["packages"][stage], stage + ".oklnxp", NXP_BANK_BYTES + 64)
        metadata = inspect_controller_package(package)
        require(metadata["role"] == (2 if stage == "lighting" else 1)
                and metadata["bank_sha256"] not in banks, "Controller stage or bank collision")
        banks.add(metadata["bank_sha256"])
    image = firmware(document["esp"], "open-keylight.bin", ESP_SLOT_BYTES)
    esp = inspect_esp_application(image)
    require(esp["version"] == version, "ESP descriptor differs from release VERSION")
    assets = firmware(document["assets"], "asset-manifest.json", 131072)
    validate_assets(assets, image)

    for name in INSTALLER:
        put("installer/" + name, read(installer, name, 16777216 if name == "cli.js" else 1048576))
    build = _json(files["installer/build.json"])
    require(isinstance(build, dict) and set(build) == {
        "format", "node", "bytes", "sha256", "bundled_packages", "external_runtime_packages"},
        "Unexpected installer build fields")
    require(type(build["format"]) is int and build["format"] == 1 and build["node"] == ">=22"
            and type(build["bytes"]) is int and build["bytes"] == len(files["installer/cli.js"])
            and build["sha256"] == digest(files["installer/cli.js"])
            and type(build["bundled_packages"]) is int and 0 < build["bundled_packages"] <= 1000
            and type(build["external_runtime_packages"]) is int and build["external_runtime_packages"] == 0,
            "Installer build is stale or contains external runtime packages")
    require(_json(files["installer/package.json"]) == {"type": "module", "engines": {"node": ">=22"}},
            "Installer runtime manifest must not add dependencies or scripts")
    legal = installer / "cli.js.LEGAL.txt"
    if legal.exists() or legal.is_symlink():
        put("installer/cli.js.LEGAL.txt", read(installer, legal.name, 1048576))
    require(b"cli.js.LEGAL.txt" not in files["installer/cli.js"] or "installer/cli.js.LEGAL.txt" in files,
            "Linked installer license text is missing")
    for name in TOOLS:
        put("tools/" + name, read(root, "tools/" + name, 1048576))
    for name in LAUNCHERS:
        put(name, read(root, "distribution/windows/" + name, 1048576))
    runtimes = _json(files["runtimes.json"])
    require(isinstance(runtimes, dict) and type(runtimes.get("format")) is int
            and runtimes["format"] == 1 and runtimes.get("platform") == "win-x64",
            "Wrong launcher runtime platform")
    for name in ("LICENSE", "README.md", "docs/getting-started.md"):
        put(name, read(root, name, 1048576))
    for name in GUIDE_IMAGES:
        put(name, read(root, name, 8388608))
    put("START_HERE.txt", (
        "Open Keylight Chroma - Windows x64\n\n"
        "Extract the entire ZIP, then open start-open-keylight.cmd.\n"
        "Read docs/getting-started.md before connecting to a light.\n"
        "README.md and the guide images are included for offline reading.\n"
        "Engineering references linked from those documents are in the online source:\n"
        "https://github.com/StormBurpee/open-keylight-chroma\n\n"
        "First setup downloads pinned Node/Python runtimes and the stock recovery\n"
        "image from their original publishers. They are not bundled here.\n"
        "SHA256SUMS covers every release payload and release.json. Checksums detect\n"
        "changes; verify the download's publisher and qualification notes separately.\n"
    ).encode("ascii"))

    release = {"format": 1, "product": "open-keylight-chroma", "platform": "win-x64",
               "version": version, "source_commit": document["source_commit"],
               "firmware_bundle_sha256": digest(files["firmware/bundle.json"]),
               "installer_sha256": build["sha256"], "esp_elf_sha256": esp["elf_sha256"],
               "authenticity_verified": False, "hardware_qualified_by_packaging": False,
               "files": {name: {"sha256": digest(value), "bytes": len(value)}
                         for name, value in sorted(files.items())}}
    put("release.json", _json_bytes(release))
    put("SHA256SUMS", "".join(f"{digest(value)}  {name}\n" for name, value in sorted(files.items())).encode("ascii"))

    descriptor, temporary_name = tempfile.mkstemp(prefix=f".{output.name}.", suffix=".tmp", dir=output.parent)
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "w+b") as stream:
            with zipfile.ZipFile(stream, "w", compression=zipfile.ZIP_STORED, allowZip64=False) as archive:
                for name, content in sorted(files.items()):
                    entry = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
                    entry.create_system = 3
                    entry.external_attr = 0o100644 << 16
                    entry.compress_type = zipfile.ZIP_STORED
                    archive.writestr(entry, content)
            stream.flush()
            os.fsync(stream.fileno())
            stream.seek(0)
            archive_sha = digest(stream.read())
        # Unlike rename/replace, link refuses even a concurrent destination.
        os.link(temporary, output)
    finally:
        temporary.unlink(missing_ok=True)
    return {"path": str(output), "sha256": archive_sha, "files": len(files),
            "version": version, "source_commit": document["source_commit"], "device_operations": 0}


def main(argv=None) -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bundle", type=Path, required=True)
    parser.add_argument("--installer", type=Path, required=True, help="Already bundled installer dist directory")
    parser.add_argument("--output", type=Path, required=True, help="New Windows x64 release ZIP")
    parser.add_argument("--root", type=Path, default=ROOT, help="Public source root")
    args = parser.parse_args(argv)
    try:
        result = build_release(args.bundle, args.installer, args.output, args.root)
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.error(str(error))
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
