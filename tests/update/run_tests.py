"""Compile actual ESP update.c with deterministic IDF boundary mocks and ASan."""
from pathlib import Path
import hashlib
import json
import os
import shutil
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
OUT = ROOT / "build" / "update-tests"
OUT.mkdir(parents=True, exist_ok=True)
compiler = os.environ.get("CC") or shutil.which("clang") or shutil.which("cc")
if not compiler:
    raise SystemExit("Set CC to an AddressSanitizer-capable C compiler")
stubs = OUT / "stubs"
for header in ["esp_app_desc.h", "esp_ota_ops.h", "esp_system.h", "freertos/task.h", "mbedtls/sha256.h", "nvs.h", "lwip/sockets.h"]:
    path = stubs / header
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text('#include "update_mocks.h"\n')
exe = OUT / ("test_update.exe" if os.name == "nt" else "test_update")
command = [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wpedantic", "-g", "-O1", "-fsanitize=address", "-fno-omit-frame-pointer", "-I", str(HERE), "-I", str(stubs), str(HERE / "test_update.c"), str(ROOT / "firmware/main/update_indicator.c"), "-o", str(exe)]
subprocess.run(command, check=True)
env = os.environ.copy()
env["ASAN_OPTIONS"] = f"detect_leaks={0 if os.name == 'nt' else 1}:halt_on_error=1"
result = subprocess.run([str(exe)], capture_output=True, text=True, env=env)
if result.returncode:
    print(result.stdout, end="")
    print(result.stderr, file=sys.stderr, end="")
    result.check_returncode()
sources = [ROOT / "firmware/main/update.c", ROOT / "firmware/main/update_indicator.c", ROOT / "firmware/main/update_indicator.h", HERE / "test_update.c", HERE / "update_mocks.h", Path(__file__)]
report = {"status": "pass", "device_operations": 0, "sanitizer": "AddressSanitizer", "output": result.stdout.strip(), "source_sha256": {str(path.relative_to(ROOT)): hashlib.sha256(path.read_bytes()).hexdigest() for path in sources}, "limits": ["IDF, NVS, socket and OTA boundaries mocked; no device or bootloader execution", "SHA256 implementation is supplied by IDF and mocked here; this suite checks API error handling and digest comparison"]}
(OUT / "result.json").write_text(json.dumps(report, indent=2) + "\n")
print(result.stdout, end="")
