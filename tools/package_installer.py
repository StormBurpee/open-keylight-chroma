#!/usr/bin/env python3
"""Build an original-firmware installer bundle offline; never contacts a light.

Package metadata and hashes establish consistency, not publisher authenticity or
hardware qualification. Release builders must supply reviewed original builds.
Vendor firmware, restore banks, credentials and source trees are not included.
"""
from __future__ import annotations

import argparse
import ctypes
import errno
import json
import os
from pathlib import Path
import re
import shutil
import sys
import tempfile
import zlib

from stock_migration import (ESP_SLOT_BYTES, NXP_BANK_BYTES, _asset_path, _json,
                             _sha, bounded_read, digest, inspect_controller_package,
                             inspect_esp_application, require)

ROOT = Path(__file__).resolve().parents[1]
STAGES = (("identity", "identity"), ("OFF1", "off1"),
          ("LOW1", "low1"), ("lighting", "lighting"))
VERSION_PATTERN = re.compile(r"(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\."
                             r"(?:0|[1-9][0-9]*)(?:-[0-9A-Za-z]+(?:[.-][0-9A-Za-z]+)*)?")


def validate_assets(raw: bytes, image: bytes) -> dict:
    """Require bounded, distinct asset paths and their exact embedded contents.

    This is the offline bundle counterpart of load_plan's asset check. It does
    not invent the target/restore evidence required by a migration plan.
    """
    assets = _json(raw)
    require(isinstance(assets, dict) and type(assets.get("version")) is int
            and assets["version"] == 1 and isinstance(assets.get("files"), list)
            and 1 <= len(assets["files"]) <= 8, "Invalid dashboard manifest")
    paths, expected = set(), []
    for item in assets["files"]:
        require(isinstance(item, dict) and _asset_path(item.get("path"))
                and item["path"] not in paths, "Invalid or duplicate dashboard asset path")
        paths.add(item["path"])
        _sha(item.get("sha256"))
        require(type(item.get("size")) is int and 0 < item["size"] <= 1048576,
                "Invalid dashboard asset size")
        expected.append((item["sha256"], item["size"]))
    require("/index.html" in paths, "Dashboard index is absent")
    embedded, offset, candidates = [], 0, 0
    while True:
        offset = image.find(b"\x1f\x8b\x08", offset)
        if offset < 0:
            break
        candidates += 1
        require(candidates <= 32, "Too many embedded gzip candidates")
        try:
            decoder = zlib.decompressobj(31)
            decoded = decoder.decompress(image[offset:], 1048577)
            if decoder.eof and len(decoded) <= 1048576:
                embedded.append((digest(decoded), len(decoded)))
        except zlib.error:
            pass
        offset += 3
    require(all(embedded.count(item) == 1 for item in expected),
            "Dashboard manifest does not match exactly one copy of each embedded asset")
    return assets


def _write_file(path: Path, content: bytes) -> None:
    with path.open("xb") as stream:
        require(stream.write(content) == len(content), "Incomplete bundle file write")
        stream.flush()
        os.fsync(stream.fileno())


def _publish_directory(source: Path, destination: Path) -> None:
    """Atomically publish without replacing even a concurrently created directory.

    POSIX rename alone can replace an empty directory. Linux's explicit
    NOREPLACE operation and Windows rename both reject that race. Unsupported
    platforms fail closed rather than offering a weaker overwrite guarantee.
    """
    if os.name == "nt":
        os.rename(source, destination)
        return
    if sys.platform.startswith("linux"):
        library = ctypes.CDLL(None, use_errno=True)
        rename = getattr(library, "renameat2", None)
        if rename is None:
            raise OSError(errno.ENOTSUP, "Atomic no-replace directory publication is unavailable")
        rename.argtypes = (ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_uint)
        rename.restype = ctypes.c_int
        if rename(-100, os.fsencode(source), -100, os.fsencode(destination), 1):
            code = ctypes.get_errno()
            raise OSError(code, os.strerror(code), str(destination))
        return
    raise OSError(errno.ENOTSUP, "Bundle publication currently supports Windows and Linux")


def build_bundle(args: argparse.Namespace) -> dict:
    """Validate immutable input snapshots and publish a new seven-file bundle.

    Output names are fixed portable relative paths, independent of input names.
    The complete directory becomes visible in one no-replace operation. The
    source files are never moved, edited or re-read after validation.
    """
    requested = Path(args.output).absolute()
    output = requested.parent.resolve(strict=True) / requested.name
    if output.exists() or output.is_symlink():
        raise FileExistsError(f"Output already exists: {output}")
    require(isinstance(args.source_commit, str)
            and re.fullmatch(r"[0-9a-f]{40}", args.source_commit), "Full lowercase source commit required")
    version_raw = bounded_read(Path(args.version_file).resolve(strict=True), 64)
    try:
        version = version_raw.decode("ascii").strip()
    except UnicodeDecodeError as error:
        raise ValueError("VERSION must contain an ASCII release version") from error
    require(VERSION_PATTERN.fullmatch(version) and len(version) <= 31, "Invalid release VERSION")

    inputs: list[Path] = []
    files: dict[str, bytes] = {}

    def read_input(value, maximum: int) -> bytes:
        path = Path(value).resolve(strict=True)
        require(path.is_file(), "Bundle input must be a regular file")
        require(not any(path.samefile(previous) for previous in inputs), "Bundle input paths collide")
        inputs.append(path)
        return bounded_read(path, maximum)

    def spec(name: str, raw: bytes) -> dict:
        files[name] = raw
        return {"path": name, "sha256": digest(raw), "bytes": len(raw)}

    packages, bank_digests = {}, set()
    for stage, option in STAGES:
        raw = read_input(getattr(args, option), NXP_BANK_BYTES + 64)
        metadata = inspect_controller_package(raw)
        require(metadata["role"] == (2 if stage == "lighting" else 1), "Package stage/role mismatch")
        require(metadata["bank_sha256"] not in bank_digests,
                "Distinct controller stages must not relabel the same bank")
        bank_digests.add(metadata["bank_sha256"])
        packages[stage] = spec(stage + ".oklnxp", raw)
    image = read_input(args.esp, ESP_SLOT_BYTES)
    metadata = inspect_esp_application(image)
    require(metadata["version"] == version, "ESP application version differs from VERSION")
    asset_raw = read_input(args.assets, 131072)
    validate_assets(asset_raw, image)
    document = {"format": 1, "product": "open-keylight-chroma", "version": version,
                "source_commit": args.source_commit, "stock_profile": "keylight-chroma-1.0.13",
                "packages": packages, "esp": spec("open-keylight.bin", image),
                "assets": spec("asset-manifest.json", asset_raw)}
    manifest = (json.dumps(document, indent=2, ensure_ascii=True, allow_nan=False) + "\n").encode("utf-8")

    temporary = Path(tempfile.mkdtemp(prefix=f".{output.name}.", dir=output.parent)).resolve()
    try:
        for name, raw in files.items():
            _write_file(temporary / name, raw)
        _write_file(temporary / "bundle.json", manifest)
        if os.name != "nt":
            descriptor = os.open(temporary, os.O_RDONLY | os.O_DIRECTORY)
            try:
                os.fsync(descriptor)
            finally:
                os.close(descriptor)
        _publish_directory(temporary, output)
    finally:
        if temporary.exists():
            # Only remove the exact private sibling created above, never an
            # existing output, a computed ancestor or an external symlink.
            require(temporary.parent == output.parent and temporary.name.startswith(f".{output.name}.")
                    and not temporary.is_symlink(), "Unexpected temporary bundle path")
            shutil.rmtree(temporary)
    return {"bundle": str(output / "bundle.json"), "bundle_sha256": digest(manifest),
            "version": version, "source_commit": args.source_commit,
            "files": len(files) + 1, "device_operations": 0,
            "authenticity_verified": False, "hardware_qualified_by_packaging": False}


def main(argv=None) -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path, help="New bundle directory; never replaced")
    parser.add_argument("--source-commit", required=True, help="Full reviewed original-source commit")
    parser.add_argument("--version-file", type=Path, default=ROOT / "VERSION", help="Release VERSION file")
    for option in ("identity", "off1", "low1", "lighting"):
        parser.add_argument("--" + option, required=True, type=Path, help="Reviewed original controller package")
    parser.add_argument("--esp", required=True, type=Path, help="Original standalone ESP application")
    parser.add_argument("--assets", required=True, type=Path, help="Matching embedded dashboard asset manifest")
    args = parser.parse_args(argv)
    try:
        result = build_bundle(args)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
