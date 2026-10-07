"""Actual controller job/core with deterministic NVS, crypto and lock failures."""
from pathlib import Path
import hashlib
import json
import os
import shutil
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
OUT = ROOT / "build/controller-job-tests"
OUT.mkdir(parents=True, exist_ok=True)
compiler = os.environ.get("CC") or shutil.which("clang") or shutil.which("cc")
if not compiler:
    raise SystemExit("Set CC to an ASan-capable C compiler; libcJSON development files required")
stubs = OUT / "stubs"
for name in ("esp_err.h", "freertos/FreeRTOS.h", "freertos/semphr.h", "nvs.h", "mbedtls/sha256.h"):
    path = stubs / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text('#include "job_mocks.h"\n')
components = ROOT / "firmware/components"
includes = [HERE, stubs, ROOT / "firmware/main", components / "keylight_core/include",
            components / "keylight_json/include", components / "keylight_nxp/include",
            components / "keylight_loader/include", Path(os.environ.get("CJSON_INCLUDE_DIR", "/usr/include/cjson"))]
sources = [HERE / "test_job.c", components / "keylight_loader/okl_loader.c", components / "keylight_nxp/okl_nxp.c"]
exe = OUT / ("test_job.exe" if os.name == "nt" else "test_job")
command = [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wpedantic", "-g", "-O1",
           "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
for path in includes:
    command += ["-I", str(path)]
command += [str(path) for path in sources] + ["-lcjson", "-o", str(exe)]
subprocess.run(command, check=True)
env = os.environ.copy()
env["ASAN_OPTIONS"] = f"detect_leaks={0 if os.name == 'nt' else 1}:halt_on_error=1"
env["UBSAN_OPTIONS"] = "halt_on_error=1"
result = subprocess.run([str(exe)], capture_output=True, text=True, env=env)
print(result.stdout, end="")
if result.returncode:
    print(result.stderr, file=sys.stderr, end="")
    result.check_returncode()
sources += [ROOT / "firmware/main/controller_job.c", ROOT / "firmware/main/controller_job.h",
            ROOT / "firmware/main/app.h", HERE / "job_mocks.h", Path(__file__)]
report = {"status": "pass", "sanitizer": "AddressSanitizer+UndefinedBehaviorSanitizer", "device_operations": 0,
          "output": result.stdout.strip(), "source_sha256": {
              path.relative_to(ROOT).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest() for path in sources},
          "limits": ["Actual job manager, package validation and cJSON used; RTOS lock, NVS and SHA boundaries mocked",
                     "Single-record flash atomicity and physical power loss require separate hardware qualification"]}
(OUT / "result.json").write_text(json.dumps(report, indent=2) + "\n")
