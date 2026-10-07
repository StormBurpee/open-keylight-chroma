"""Package one development application and its provenance; never contacts a device."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import struct

SLOT_BYTES = 0x180000


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def validate_image(image: bytes, elf: bytes, version: str) -> dict:
    require(288 <= len(image) <= SLOT_BYTES and image[0] == 0xE9, "Require an application fitting the existing slot")
    require(1 <= image[1] <= 16 and image[23] == 1 and image[12:14] == bytes(2), "Require a hashed ESP32 image")
    offset, checksum, descriptor = 24, 0xEF, b""
    for index in range(image[1]):
        require(offset + 8 <= len(image), "Truncated segment header")
        _, size = struct.unpack_from("<II", image, offset)
        offset += 8
        require(size % 4 == 0 and size <= len(image) - offset, "Invalid segment length")
        segment = image[offset:offset + size]
        if index == 0:
            descriptor = segment[:256]
        for byte in segment:
            checksum ^= byte
        offset += size
    checksum_at = offset | 15
    require(checksum_at + 33 == len(image) and not any(image[offset:checksum_at]), "Invalid image padding or trailing data")
    require(image[checksum_at] == checksum and hashlib.sha256(image[:-32]).digest() == image[-32:], "Image integrity check failed")
    require(len(descriptor) == 256 and descriptor[:4] == bytes.fromhex("3254cdab"), "Missing application descriptor")

    def text(start: int) -> str:
        value = descriptor[start:start + 32]
        require(b"\0" in value, "Unterminated descriptor field")
        return value.split(b"\0", 1)[0].decode("ascii")

    require(text(16) == version and text(48) == "open_keylight", "Descriptor version/project differs")
    require(elf.startswith(b"\x7fELF") and descriptor[144:176].hex() == sha256(elf), "ELF does not match application")
    return {"version": version, "idf_version": text(112), "sha256": sha256(image),
            "elf_sha256": sha256(elf), "bytes": len(image), "slot_bytes": SLOT_BYTES}


def package(build: Path, asset_manifest: Path, version: str, commit: str, output: Path) -> dict:
    require(bool(re.fullmatch(r"[0-9a-f]{40}", commit)), "Require the full source commit")
    require(bool(re.fullmatch(r"\d+\.\d+\.\d+(?:-[A-Za-z0-9.-]+)?", version)) and len(version) < 32,
            "Invalid firmware version")
    files = {name: (build / name).read_bytes() for name in
             ("open_keylight.bin", "open_keylight.elf", "project_description.json")}
    files["asset-manifest.json"] = asset_manifest.read_bytes()
    image = validate_image(files["open_keylight.bin"], files["open_keylight.elf"], version)
    description = json.loads(files["project_description.json"])
    require(description.get("project_name") == "open_keylight" and description.get("project_version") == version
            and description.get("target") == "esp32", "Build description differs")
    assets = json.loads(files["asset-manifest.json"])
    require(assets.get("version") == 1 and isinstance(assets.get("files"), list) and assets["files"], "Invalid asset manifest")
    for asset in assets["files"]:
        path = asset.get("path", "")
        require(isinstance(path, str) and path.startswith("/") and ".." not in path and "\\" not in path,
                "Invalid asset path")
        local = (asset_manifest.parent / path[1:]).resolve()
        require(local.is_relative_to(asset_manifest.parent.resolve()), "Asset escapes the dashboard directory")
        require(sha256(local.read_bytes()) == asset.get("sha256"), "Dashboard asset differs")
    manifest = {"format": 1, "kind": "development-application", "app_only": True,
                "hardware_qualified": False, "source_commit": commit, **image,
                "files": {name: sha256(data) for name, data in files.items()}}
    files["manifest.json"] = (json.dumps(manifest, indent=2) + "\n").encode()
    checksums = "".join(f"{sha256(data)}  {name}\n" for name, data in sorted(files.items()))
    # An existing artifact directory is evidence, not a scratch space to overwrite.
    output.mkdir(parents=True, exist_ok=False)
    for name, data in files.items():
        (output / name).write_bytes(data)
    (output / "SHA256SUMS").write_text(checksums, encoding="ascii", newline="\n")
    return manifest


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--dashboard-manifest", type=Path, required=True)
    parser.add_argument("--version-file", type=Path, default=Path("VERSION"))
    parser.add_argument("--commit", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        manifest = package(args.build_dir, args.dashboard_manifest,
                           args.version_file.read_text().strip(), args.commit, args.output)
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.error(str(error))
    print(json.dumps(manifest, indent=2))


if __name__ == "__main__":
    main()
