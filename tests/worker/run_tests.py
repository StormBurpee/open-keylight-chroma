"""Compile the actual ESP worker with bounded, deterministic boundary mocks."""
from pathlib import Path
import hashlib
import json
import os
import shutil
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
OUT = ROOT / "build/worker-tests"
OUT.mkdir(parents=True, exist_ok=True)
compiler = os.environ.get("CC") or shutil.which("clang") or shutil.which("cc")
if not compiler:
    raise SystemExit("Set CC to an AddressSanitizer-capable C compiler")
stubs = OUT / "stubs"
for header in ("esp_err.h", "cJSON.h", "freertos/FreeRTOS.h", "freertos/semphr.h", "freertos/task.h"):
    path = stubs / header
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text('#include "worker_mocks.h"\n')
exe = OUT / ("test_worker.exe" if os.name == "nt" else "test_worker")
components = ROOT / "firmware/components"
command = [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wpedantic", "-g", "-O1",
           "-fsanitize=address", "-fno-omit-frame-pointer"]
for path in (HERE, stubs, ROOT / "firmware/main", components / "keylight_core/include",
             components / "keylight_json/include", components / "keylight_nxp/include"):
    command += ["-I", str(path)]
sources = [HERE / "test_worker.c", ROOT / "firmware/main/output_policy.c",
           components / "keylight_core/keylight_core.c", components / "keylight_nxp/okl_nxp.c"]
command += [str(path) for path in sources] + ["-lm", "-o", str(exe)]
subprocess.run(command, check=True)
env = os.environ.copy()
env["ASAN_OPTIONS"] = f"detect_leaks={0 if os.name == 'nt' else 1}:halt_on_error=1"
result = subprocess.run([str(exe)], capture_output=True, text=True, env=env)
if result.returncode:
    print(result.stdout, end="")
    print(result.stderr, file=sys.stderr, end="")
    result.check_returncode()
sources += [ROOT / "firmware/main/worker.c", ROOT / "firmware/main/app.h",
            ROOT / "firmware/main/output_policy.h", HERE / "worker_mocks.h", Path(__file__)]
report = {"status": "pass", "sanitizer": "AddressSanitizer", "device_operations": 0,
          "output": result.stdout.strip(), "source_sha256": {
              path.relative_to(ROOT).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest() for path in sources},
          "limits": ["RTOS scheduling and NXP exchange boundaries mocked; real worker, output policy and frame arithmetic used",
                     "Driver ownership atomicity and physical output timing are covered separately"]}
(OUT / "result.json").write_text(json.dumps(report, indent=2) + "\n")
print(result.stdout, end="")
