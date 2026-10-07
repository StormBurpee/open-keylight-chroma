import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import vendor_restore as v


def record(kind, address=0, data=b""):
    raw = bytes([len(data)]) + address.to_bytes(2, "big") + bytes([kind]) + data
    return ":" + (raw + bytes([-sum(raw) & 255])).hex().upper()


class Tests(unittest.TestCase):
    def setUp(self):
        # Synthetic bytes only: vendor executable contents never enter tests.
        self.payload = bytes(i % 251 for i in range(v.PAYLOAD_BYTES))
        self.lines = [record(4, data=bytes(2))]
        self.lines += [record(0, v.BANK_START + i, self.payload[i:i+16])
                       for i in range(0, len(self.payload), 16)]
        self.lines += [record(5, data=bytes.fromhex("000020c1")), record(1)]
        self.hex = self.encode(self.lines)
        self.bank = self.payload + b"\xff" * (v.BANK_BYTES - len(self.payload))
        self.version = bytes.fromhex("02030d0001000d0001030000") + bytes(20)
        self.patches = patch.multiple(v, HEX_BYTES=len(self.hex), HEX_SHA=v.digest(self.hex),
            PAYLOAD_SHA=v.digest(self.payload), BANK_SHA=v.digest(self.bank), VERSION_SHA=v.digest(self.version))
        self.patches.start(); self.addCleanup(self.patches.stop)

    @staticmethod
    def encode(lines):
        return ("\r\n".join(lines) + "\r\n").encode()

    def archive(self, *, names=None, version=None):
        data = io.BytesIO()
        with zipfile.ZipFile(data, "w", zipfile.ZIP_DEFLATED) as z:
            for name, content in (names or [("NXP.hex", self.hex), ("Version.bin", version or self.version),
                                           ("ESP.bin", bytes(1091040))]):
                z.writestr(name, content)
        return data.getvalue()

    def test_contiguous_payload_and_exact_erased_tail(self):
        self.assertEqual(v.decode_hex(self.hex), self.bank)
        self.assertEqual(len(self.bank), 28672)
        self.assertEqual(self.bank[24056:], b"\xff" * 4616)

    def test_profile_pin_rejects_even_a_valid_changed_hex_file(self):
        altered = self.hex.replace(b"\r\n", b"\n")
        with self.assertRaisesRegex(ValueError, "reviewed image"):
            v.decode_hex(altered)

    def test_record_boundaries_checksums_addresses_and_termination(self):
        variants = []
        for index, replacement in [(0, record(4, data=b"\0\1")),
                                   (1, record(0, v.BANK_START + 1, self.payload[:16])),
                                   (2, record(0, v.BANK_START, self.payload[16:32])),
                                   (2, record(2, v.BANK_START + 16, self.payload[16:32])),
                                   (-2, record(5, data=bytes.fromhex("000020c3"))),
                                   (-1, record(1, data=b"\0"))]:
            changed = list(self.lines); changed[index] = replacement; variants.append(self.encode(changed))
        variants += [self.hex[:-2], self.encode(self.lines[:-1]), self.hex.replace(b":10", b":11", 1)]
        for data in variants:
            with self.subTest(digest=v.digest(data)), patch.multiple(v, HEX_BYTES=len(data), HEX_SHA=v.digest(data)):
                # Removing the final newline alone is harmless to Intel HEX.
                if data == self.hex[:-2]:
                    self.assertEqual(v.decode_hex(data), self.bank)
                else:
                    with self.assertRaises(ValueError): v.decode_hex(data)

    def test_decoded_payload_and_complete_bank_are_both_pinned(self):
        for field in ("PAYLOAD_SHA", "BANK_SHA"):
            with self.subTest(field=field), patch.object(v, field, "0" * 64), self.assertRaises(ValueError):
                v.decode_hex(self.hex)

    def test_archive_and_member_metadata(self):
        valid = self.archive()
        with patch.multiple(v, ARCHIVE_BYTES=len(valid), ARCHIVE_SHA=v.digest(valid)):
            self.assertEqual(v.bank_from_archive(valid), self.bank)
            with self.assertRaises(ValueError): v.bank_from_archive(valid + b"x")
        for raw in [self.archive(names=[("../NXP.hex", self.hex)]),
                    self.archive(version=bytes(32)),
                    self.archive(names=[("NXP.hex", self.hex), ("ESP.bin", b"x"), ("Version.bin", self.version)])]:
            with patch.multiple(v, ARCHIVE_BYTES=len(raw), ARCHIVE_SHA=v.digest(raw)), self.assertRaises(ValueError):
                v.bank_from_archive(raw)

    def test_cache_hit_never_downloads_and_mismatch_never_overwrites(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "stock.bin"; path.write_bytes(self.bank)
            def forbidden(): raise AssertionError("Cache hit made a network request")
            result = v.acquire(path, fetch=forbidden)
            self.assertTrue(result["cached"]); self.assertFalse(result["device_backup"])
            path.write_bytes(b"owner data")
            with self.assertRaises(ValueError): v.acquire(path, fetch=forbidden)
            self.assertEqual(path.read_bytes(), b"owner data")

    def test_download_is_validated_before_any_file_is_published(self):
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / "new" / "stock.bin"
            with self.assertRaises(ValueError): v.acquire(output, fetch=lambda: b"wrong archive")
            self.assertFalse(output.exists()); self.assertFalse(output.parent.exists())
            raw = self.archive()
            with patch.multiple(v, ARCHIVE_BYTES=len(raw), ARCHIVE_SHA=v.digest(raw)):
                result = v.acquire(output, fetch=lambda: raw)
            self.assertEqual(output.read_bytes(), self.bank)
            self.assertEqual(json.loads(json.dumps(result))["sha256"], v.digest(self.bank))
            self.assertFalse(result["cached"])

    def test_redirects_do_not_change_the_publisher(self):
        with self.assertRaisesRegex(ValueError, "redirected"):
            v.NoRedirects().redirect_request(None, None, 302, "", {}, "https://example.com/firmware")

    def test_publication_failure_does_not_leave_a_partial_recovery_file(self):
        raw = self.archive()
        for operation in ("fsync", "link"):
            with tempfile.TemporaryDirectory() as tmp, patch.multiple(v, ARCHIVE_BYTES=len(raw), ARCHIVE_SHA=v.digest(raw)):
                path = Path(tmp) / "stock.bin"
                with patch.object(v.os, operation, side_effect=OSError("injected persistence failure")), self.assertRaises(OSError):
                    v.acquire(path, fetch=lambda: raw)
                self.assertFalse(path.exists())
                self.assertEqual(list(Path(tmp).iterdir()), [])


if __name__ == "__main__":
    unittest.main()
