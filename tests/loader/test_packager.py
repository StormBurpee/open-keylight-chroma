"""Cross-platform package authoring CLI tests, without any controller contact."""
from pathlib import Path
import hashlib
import importlib.util
import json
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / "tools/package_controller.py"
spec = importlib.util.spec_from_file_location("packager", TOOL)
packager = importlib.util.module_from_spec(spec)
spec.loader.exec_module(packager)


class PackageTests(unittest.TestCase):
    bank = struct.pack("<48I", 0x10001000, *([0x20C1] * 47)) + bytes(28672 - 192)

    def test_wire_format(self):
        package, metadata = packager.build_package(self.bank, (1, 2, 3, 255))
        self.assertEqual(len(package), 28736)
        self.assertEqual(package[:28], bytes.fromhex("4f4b4c434e58500000010040000070000000bc4001000200010203ff"))
        self.assertEqual(package[28:60], hashlib.sha256(self.bank).digest())
        self.assertEqual(package[60:64], bytes(4))
        self.assertEqual(package[64:], self.bank)
        self.assertEqual(metadata["package_sha256"], hashlib.sha256(package).hexdigest())

    def test_versions_and_sizes(self):
        for text in ("", "1", "1.2.3", "1.2.3.4.5", "0.0.0.0", "256.0.0.0", "-1.0.0.0", "1.2.3. 4", "１.2.3.4"):
            with self.subTest(text=text), self.assertRaises(ValueError):
                packager.parse_version(text)
        self.assertEqual(packager.parse_version("255.1.0.0"), (255, 1, 0, 0))
        for size in (0, 192, 28671, 28673):
            with self.assertRaises(ValueError):
                packager.build_package(bytes(size), (0, 1, 0, 0))

    def test_every_vector(self):
        for index in range(48):
            for address in (0, 0x2001, 0x20C0, 0x9001, 0xFFFFFFFF):
                bank = bytearray(self.bank); struct.pack_into("<I", bank, index * 4, address)
                with self.assertRaises(ValueError):
                    packager.build_package(bytes(bank), (0, 1, 0, 0))

    def test_cli_exclusive_output_and_same_path(self):
        with tempfile.TemporaryDirectory() as temp:
            bank, output = Path(temp) / "input.bin", Path(temp) / "output.oklnxp"
            bank.write_bytes(self.bank)
            command = [sys.executable, str(TOOL), str(bank), str(output), "--version", "0.1.0.0"]
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            metadata = json.loads(result.stdout)
            self.assertEqual(metadata["package_sha256"], hashlib.sha256(output.read_bytes()).hexdigest())
            self.assertEqual(metadata["device_operations"], 0)
            output.write_bytes(b"preserve existing output")
            self.assertNotEqual(subprocess.run(command, capture_output=True).returncode, 0)
            self.assertEqual(output.read_bytes(), b"preserve existing output")
            self.assertEqual(subprocess.run(command + ["--overwrite"], capture_output=True).returncode, 0)
            same = [sys.executable, str(TOOL), str(bank), str(bank), "--version", "0.1.0.0", "--overwrite"]
            self.assertNotEqual(subprocess.run(same, capture_output=True).returncode, 0)
            self.assertEqual(bank.read_bytes(), self.bank)


if __name__ == "__main__":
    unittest.main(verbosity=2)
