"""Acquire the reviewed stock recovery bank from its original HTTPS publisher.

No device connection, flash dump or vendor redistribution. The erased tail is
part of this exact reviewed profile; other HEX images are never padded by guess.
"""
from __future__ import annotations

import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import tempfile
import time
import urllib.request
import zipfile

URL = "https://mobileapp-assets.razerzone.com/iOS/Jade/T1/02.03.13.00.zip"
ARCHIVE_BYTES = 1159087
ARCHIVE_SHA = "038489e9f48df6771f5280e652cca36984fccd17b870488fe5edf6e479276831"
HEX_BYTES = 67715
HEX_SHA = "ea5ba2e6e2cc868795757e6f3d1c94b9a767b170a2d4c8d952c5496cfc7069b4"
VERSION_SHA = "75a84aa2961cab2d4a224f4423bd7d308e4e9450a74e5819c06876b832822bf8"
PAYLOAD_SHA = "72268a6f965776d78efa9d6fa63f55a2f78ddcbd7b6c3aa4e83f0e6fbfc5ec25"
BANK_SHA = "f46d19f50bba9fd97c6c3e207557b15470ad05d1877402a0cd15e0adc3a87a96"
PAYLOAD_BYTES, BANK_BYTES, BANK_START = 24056, 28672, 0x2000
PROVENANCE = (
    "Original Razer 02.03.13.00 HTTPS archive; checksum-validated NXP 1.3.0.0 "
    "Intel HEX covers 0x2000..0x7df7. The reviewed profile supplies an erased "
    "0xff tail through 0x8fff; the complete bank matches the independently "
    "captured reference staging bank byte for byte. This is a stock recovery "
    "image, not a backup of this device or proof of restoration on it."
)


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def require(condition, message):
    if not condition:
        raise ValueError(message)


def decode_hex(data: bytes) -> bytes:
    """Decode only this bounded, contiguous application profile."""
    require(len(data) == HEX_BYTES and digest(data) == HEX_SHA, "Stock NXP HEX differs from the reviewed image")
    lines = data.decode("ascii").splitlines()
    require(len(lines) == 1507, "Unexpected HEX record count")
    payload = bytearray()
    for index, line in enumerate(lines):
        require(line.startswith(":"), "Missing HEX record marker")
        record = bytes.fromhex(line[1:])
        require(len(record) >= 5 and len(record) == record[0] + 5 and sum(record) % 256 == 0,
                "Invalid HEX record length/checksum")
        size, address, kind = record[0], int.from_bytes(record[1:3], "big"), record[3]
        content = record[4:-1]
        if index == 0:
            require(record == bytes.fromhex("020000040000fa"), "Unexpected extended address")
        elif index == len(lines) - 2:
            require(record == bytes.fromhex("04000005000020c116"), "Unexpected start address")
        elif index == len(lines) - 1:
            require(record == bytes.fromhex("00000001ff"), "Missing final EOF record")
        else:
            require(kind == 0 and 0 < size <= 16 and address == BANK_START + len(payload)
                    and len(payload) + size <= PAYLOAD_BYTES, "HEX data is outside the contiguous application")
            payload.extend(content)
    require(len(payload) == PAYLOAD_BYTES and digest(payload) == PAYLOAD_SHA, "Decoded application digest differs")
    bank = bytes(payload) + b"\xff" * (BANK_BYTES - len(payload))
    require(digest(bank) == BANK_SHA, "Complete stock recovery bank differs")
    return bank


def bank_from_archive(data: bytes) -> bytes:
    require(len(data) == ARCHIVE_BYTES and digest(data) == ARCHIVE_SHA, "Stock archive differs from the reviewed download")
    with zipfile.ZipFile(io.BytesIO(data)) as archive:
        items = archive.infolist()
        require(len(items) == 3 and {item.filename for item in items} == {"ESP.bin", "NXP.hex", "Version.bin"},
                "Unexpected stock archive entries")
        require(all(not item.flag_bits & 1 and not item.is_dir() for item in items), "Encrypted/directory archive entry")
        require(archive.getinfo("NXP.hex").file_size == HEX_BYTES
                and archive.getinfo("Version.bin").file_size == 32
                and archive.getinfo("ESP.bin").file_size == 1091040, "Unexpected expanded stock image sizes")
        version = archive.read("Version.bin")
        require(digest(version) == VERSION_SHA and version[:12] == bytes.fromhex("02030d0001000d0001030000"),
                "Stock package version differs")
        return decode_hex(archive.read("NXP.hex"))


class NoRedirects(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        raise ValueError("Stock download redirected; the reviewed publisher URL must match exactly")


def download() -> bytes:
    """One bounded TLS-verified GET; never follows a changed download location."""
    opener = urllib.request.build_opener(NoRedirects())
    started = time.monotonic()
    with opener.open(URL, timeout=15) as response:
        require(response.status == 200 and response.geturl() == URL, "Stock download URL/status differs")
        require(response.headers.get("Content-Encoding", "identity").lower() == "identity", "Unexpected transfer encoding")
        chunks, total = [], 0
        while True:
            require(time.monotonic() - started < 60, "Stock download exceeded its deadline")
            chunk = response.read1(min(65536, ARCHIVE_BYTES + 1 - total))
            if not chunk:
                break
            total += len(chunk)
            require(total <= ARCHIVE_BYTES, "Stock download exceeds its pinned size")
            chunks.append(chunk)
    require(time.monotonic() - started < 60, "Stock download exceeded its deadline")
    return b"".join(chunks)


def acquire(output: Path, *, fetch=download) -> dict:
    if output.exists():
        require(output.is_file() and output.stat().st_size == BANK_BYTES
                and digest(output.read_bytes()) == BANK_SHA, "Existing recovery file differs; it was not overwritten")
        cached = True
    else:
        bank = bank_from_archive(fetch())
        output.parent.mkdir(parents=True, exist_ok=True)
        temporary = None
        try:
            descriptor, name = tempfile.mkstemp(prefix=f".{output.name}.", suffix=".tmp", dir=output.parent)
            temporary = Path(name)
            with os.fdopen(descriptor, "wb") as stream:
                stream.write(bank)
                stream.flush()
                os.fsync(stream.fileno())
            os.link(temporary, output)
        finally:
            if temporary is not None:
                temporary.unlink(missing_ok=True)
        cached = False
    return {"format": 1, "profile": "keylight-chroma-1.0.13", "path": str(output.resolve()),
            "bytes": BANK_BYTES, "sha256": BANK_SHA, "version": "1.3.0.0", "cached": cached,
            "source_url": URL, "archive_sha256": ARCHIVE_SHA, "provenance": PROVENANCE,
            "device_backup": False, "device_operations": 0}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path, help="Local recovery bank; an existing exact match is reused")
    args = parser.parse_args()
    try:
        result = acquire(args.output)
    except (OSError, ValueError, zipfile.BadZipFile) as error:
        parser.exit(1, f"Recovery download stopped: {error}\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
