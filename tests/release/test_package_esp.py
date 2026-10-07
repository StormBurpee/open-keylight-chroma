import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("package_esp", ROOT / "tools/package_esp.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class PackageTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.elf = b"\x7fELF" + bytes(128)
        header = bytearray(24)
        header[0], header[1], header[23] = 0xE9, 1, 1
        descriptor = bytearray(256)
        descriptor[:4] = bytes.fromhex("3254cdab")
        descriptor[16:21] = b"1.2.3"
        descriptor[48:61] = b"open_keylight"
        descriptor[112:118] = b"v5.5.5"
        descriptor[144:176] = hashlib.sha256(self.elf).digest()
        checksum = 0xEF
        for byte in descriptor:
            checksum ^= byte
        self.image = bytes(header) + struct.pack("<II", 0x3F400020, 256) + descriptor + bytes(15) + bytes([checksum])
        self.image += hashlib.sha256(self.image).digest()
        (self.root / "open_keylight.bin").write_bytes(self.image)
        (self.root / "open_keylight.elf").write_bytes(self.elf)
        (self.root / "project_description.json").write_text(json.dumps({
            "project_name": "open_keylight", "project_version": "1.2.3", "target": "esp32"}))
        (self.root / "index.html").write_bytes(b"local assets")
        (self.root / "asset-manifest.json").write_text(json.dumps({"version": 1, "files": [
            {"path": "/index.html", "sha256": module.sha256(b"local assets")}]}))

    def package(self, version="1.2.3", commit="a" * 40):
        return module.package(self.root, self.root / "asset-manifest.json", version, commit, self.root / "output")

    def test_exact_files_and_independent_checksums(self):
        result = self.package()
        self.assertFalse(result["hardware_qualified"])
        self.assertTrue(result["app_only"])
        self.assertEqual(result["elf_sha256"], module.sha256(self.elf))
        output = self.root / "output"
        self.assertEqual({p.name for p in output.iterdir()}, {"open_keylight.bin", "open_keylight.elf",
            "project_description.json", "asset-manifest.json", "manifest.json", "SHA256SUMS"})
        for line in (output / "SHA256SUMS").read_text().splitlines():
            digest, name = line.split("  ")
            self.assertEqual(digest, module.sha256((output / name).read_bytes()))
        with self.assertRaises(FileExistsError):
            self.package()

    def test_rejects_stale_version_and_incomplete_provenance(self):
        for version, commit in [("1.2.4", "a" * 40), ("1.2.3", "a" * 7), ("1.2.3", "HEAD")]:
            with self.subTest(version=version, commit=commit), self.assertRaises(ValueError):
                self.package(version, commit)
        self.assertFalse((self.root / "output").exists())

    def test_rejects_corruption_truncation_merged_image_and_wrong_elf(self):
        for image in [self.image[:-1], self.image + b"\0", bytes(4096) + self.image,
                      self.image[:100] + bytes([self.image[100] ^ 1]) + self.image[101:]]:
            with self.subTest(length=len(image)), self.assertRaises(ValueError):
                module.validate_image(image, self.elf, "1.2.3")
        with self.assertRaises(ValueError):
            module.validate_image(self.image, self.elf + b"changed", "1.2.3")

    def test_rejects_stale_build_description_or_assets_before_creating_output(self):
        for filename, value in [("project_description.json", b"{}"), ("index.html", b"changed")]:
            original = (self.root / filename).read_bytes()
            (self.root / filename).write_bytes(value)
            with self.assertRaises(ValueError):
                self.package()
            self.assertFalse((self.root / "output").exists())
            (self.root / filename).write_bytes(original)


if __name__ == "__main__":
    unittest.main()
