"""Build and run the portable loader fault model with AddressSanitizer; no device I/O."""
from pathlib import Path
import hashlib
import ctypes
import importlib.util
import json
import os
import shutil
import subprocess
import struct
import sys

root = Path(__file__).resolve().parents[2]
build = root / "build/loader-tests"
build.mkdir(parents=True, exist_ok=True)
loader = root / "firmware/components/keylight_loader"
driver = root / "firmware/components/keylight_nxp"
compiler = os.environ.get("CC") or shutil.which("clang") or shutil.which("cc")
if not compiler:
    raise SystemExit("Set CC to an AddressSanitizer-capable C compiler.")
sources = [Path(__file__).with_name("test_loader.c"), loader / "okl_loader.c", driver / "okl_nxp.c"]
exe = build / ("loader_tests.exe" if os.name == "nt" else "loader_tests")
command = [compiler, "-std=c99", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-pedantic",
           "-fsanitize=address", "-fno-omit-frame-pointer", "-I", str(loader / "include"),
           "-I", str(driver / "include"), *map(str, sources), "-o", str(exe)]
compiled = subprocess.run(command, capture_output=True, text=True)
(build / "build.log").write_text(compiled.stdout + compiled.stderr)
if compiled.returncode:
    raise SystemExit(compiled.stdout + compiled.stderr)
tested = subprocess.run([str(exe)], capture_output=True, text=True)
print(tested.stdout + tested.stderr, end="")
(build / "test.log").write_text(tested.stdout + tested.stderr)
if tested.returncode:
    raise SystemExit(tested.returncode)
# Exercise the real C package validator through an actual hashlib SHA256
# callback, independent of the buffered-flash suite's equality oracle.
adapter = Path(__file__).with_name("hashlib_adapter.c")
library = build / ("loader_hashlib.dll" if os.name == "nt" else "loader_hashlib.so")
shared_command = [compiler, "-shared", "-O1", "-Wall", "-Wextra", "-Werror", "-pedantic",
                  "-I", str(loader / "include"), "-I", str(driver / "include"),
                  str(adapter), str(loader / "okl_loader.c"), str(driver / "okl_nxp.c"), "-o", str(library)]
if os.name != "nt":
    shared_command.insert(1, "-fPIC")
shared = subprocess.run(shared_command, capture_output=True, text=True)
if shared.returncode:
    raise SystemExit(shared.stdout + shared.stderr)
sha_calls = []
sha_type = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p)
@sha_type
def actual_sha256(user, data, size, out):
    block = ctypes.string_at(data, size)
    digest = hashlib.sha256(block).digest()
    ctypes.memmove(out, digest, 32)
    sha_calls.append((size, digest.hex()))
    return 0

dll = ctypes.CDLL(str(library))
validate = dll.loader_prepare_with_sha256
validate.argtypes = [ctypes.c_void_p, ctypes.c_size_t, sha_type]
validate.restype = ctypes.c_int
bank = struct.pack("<48I", 0x10001000, *([0x20c1] * 47)) + bytes([0xff]) * (28672 - 192)
digest = hashlib.sha256(bank).digest()
packager_path = root / "tools/package_controller.py"
spec = importlib.util.spec_from_file_location("package_controller", packager_path)
packager = importlib.util.module_from_spec(spec)
spec.loader.exec_module(packager)
package, metadata = packager.build_package(bank, packager.parse_version("0.1.0.0"))
assert metadata["bank_sha256"] == digest.hex()
assert len(package) == 28736
def check(data, expected):
    buf = ctypes.create_string_buffer(bytes(data))
    result = validate(buf, len(data), actual_sha256)
    assert result == expected, (result, expected)
check(package, 0)
assert sha_calls[-1] == (28672, digest.hex())
hash_cases = 1
for offset in range(64 + 192, len(package), 211):
    changed = bytearray(package); changed[offset] ^= 1
    check(changed, 7)  # OKL_LOADER_VERIFY
    hash_cases += 1
changed = bytearray(package); changed[28] ^= 1
check(changed, 7); hash_cases += 1
diagnostic, diagnostic_metadata = packager.build_package(bank, (0, 1, 0, 0), "diagnostic")
check(diagnostic, 0); hash_cases += 1
assert diagnostic[22] == 1 and diagnostic_metadata['declared_role'] == 'diagnostic'
assert diagnostic_metadata['bank_sha256'] == metadata['bank_sha256']
for role in (0, 3, 255):
    changed = bytearray(package); changed[22] = role
    check(changed, 1); hash_cases += 1
print(f"{hash_cases} real SHA256 callback package cases passed; no device I/O.")
packager_tests = subprocess.run([sys.executable,
                                str(Path(__file__).with_name("test_packager.py"))], capture_output=True, text=True)
print(packager_tests.stdout + packager_tests.stderr, end="")
if packager_tests.returncode:
    raise SystemExit(packager_tests.returncode)
report = {"status": "pass" if not tested.returncode else "fail", "device_operations": 0,
          "sanitizer": "AddressSanitizer", "summary": tested.stdout.strip(),
          "real_sha256_package_cases": hash_cases, "real_sha256_fixture_bank_sha256": digest.hex(),
          "source_sha256": {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest()
                            for p in sources + [adapter, Path(__file__), packager_path] + list((loader / "include").glob("*.h"))},
          "limitations": "Pure protocol/state-machine model plus real Python hashlib callback into C package validation. Embedded mbedTLS adapter, SPI timing, persistence and physical recovery need separate validation."}
(build / "result.json").write_text(json.dumps(report, indent=2) + "\n")
raise SystemExit(tested.returncode)
