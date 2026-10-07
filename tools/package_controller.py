"""Package an owner's original controller bank. Never opens a device or uploads."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import struct

BANK_BYTES = 28672
HEADER_BYTES = 64
PART_ID = 0x0000BC40


def parse_version(text: str) -> tuple[int, int, int, int]:
    parts = text.split(".")
    if len(parts) != 4 or any(not part or not part.isascii() or not part.isdecimal() for part in parts):
        raise ValueError("Version must contain four decimal bytes, for example 0.1.0.0")
    values = tuple(int(part) for part in parts)
    if any(value > 255 for value in values) or not any(values):
        raise ValueError("Version bytes must be 0..255 and the complete version must be nonzero")
    return values


def build_package(bank: bytes, version: tuple[int, int, int, int], role: str = "lighting") -> tuple[bytes, dict]:
    if role not in ("lighting", "diagnostic"):
        raise ValueError("Role must explicitly be lighting or diagnostic")
    if not isinstance(bank, bytes) or len(bank) != BANK_BYTES:
        raise ValueError("Supply one exact 28,672-byte application bank; padding is never guessed")
    if len(version) != 4 or any(type(value) is not int or not 0 <= value <= 255 for value in version) or not any(version):
        raise ValueError("Version must contain four bytes and must not be 0.0.0.0")
    vectors = struct.unpack_from("<48I", bank)
    if vectors[0] != 0x10001000:
        raise ValueError("Initial stack must match the conservative public layout at 0x10001000")
    if any(not (address & 1) or not 0x20C1 <= address <= 0x8FFF for address in vectors[1:]):
        raise ValueError("All 47 handlers must be Thumb addresses inside the application after its 192-byte vectors")
    digest = hashlib.sha256(bank).digest()
    header = struct.pack(">8sHHII4B4B32s4s", b"OKLCNXP\0", 1, HEADER_BYTES, BANK_BYTES, PART_ID,
                         1, 0, 2 if role == "lighting" else 1, 0, *version, digest, bytes(4))
    package = header + bank
    metadata = {"format": 1, "package_bytes": len(package), "bank_bytes": len(bank),
                "package_sha256": hashlib.sha256(package).hexdigest(), "bank_sha256": digest.hex(),
                "part_id": f"0x{PART_ID:08x}", "controller_abi": "1.0", "declared_role": role,
                "version": ".".join(map(str, version)), "reset_vector": f"0x{vectors[1]:08x}",
                "device_operations": 0,
                "qualification": "Metadata and digest do not prove original source, publisher authenticity or hardware qualification."}
    return package, metadata


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bank", type=Path, help="Exact original 28KiB application bank, without the resident loader")
    parser.add_argument("output", type=Path, help="New package file; existing files are protected by default")
    parser.add_argument("--version", required=True, help="Four version bytes, for example 0.1.0.0")
    parser.add_argument("--role", choices=("lighting", "diagnostic"), default="lighting",
                        help="Declared application role; diagnostics must never be trial-confirmed or enable normal output")
    parser.add_argument("--overwrite", action="store_true", help="Explicitly permit replacing an existing output package")
    args = parser.parse_args()
    try:
        if args.bank.resolve() == args.output.resolve():
            raise ValueError("Output must differ from the source bank")
        if args.bank.stat().st_size != BANK_BYTES:
            raise ValueError("Input must be exactly 28,672 bytes")
        package, metadata = build_package(args.bank.read_bytes(), parse_version(args.version), args.role)
        with args.output.open("wb" if args.overwrite else "xb") as output:
            output.write(package)
    except (OSError, ValueError) as exc:
        parser.error(str(exc))
    metadata["output"] = str(args.output)
    print(json.dumps(metadata, indent=2))


if __name__ == "__main__":
    main()
