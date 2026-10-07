"""Exercise the real controller worker adapter together with the real loader core."""
from pathlib import Path
import hashlib
import json
import os
import shutil
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
OUT = ROOT / "build/controller-worker-tests"
OUT.mkdir(parents=True, exist_ok=True)
compiler = os.environ.get("CC") or shutil.which("clang") or shutil.which("cc")
if not compiler:
    raise SystemExit("Set CC to an AddressSanitizer-capable compiler")
stubs = OUT / "stubs"
for header in ("esp_err.h", "cJSON.h", "freertos/FreeRTOS.h", "freertos/semphr.h", "freertos/task.h"):
    path = stubs / header
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text('#include "worker_mocks.h"\n')
(stubs / "mbedtls").mkdir(exist_ok=True)
(stubs / "mbedtls/sha256.h").write_text('#include <stddef.h>\nint mbedtls_sha256(const unsigned char *,size_t,unsigned char[32],int);\n')
components = ROOT / "firmware/components"
sources = [HERE / "test_integration.c", ROOT / "firmware/main/output_policy.c",
           components / "keylight_core/keylight_core.c", components / "keylight_nxp/okl_nxp.c",
           components / "keylight_loader/okl_loader.c"]
exe = OUT / ("test_integration.exe" if os.name == "nt" else "test_integration")
command = [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wpedantic", "-g", "-O1",
           "-fsanitize=address", "-fno-omit-frame-pointer"]
for path in (ROOT / "tests/worker", stubs, ROOT / "firmware/main",
             *(components / name / "include" for name in ("keylight_core", "keylight_json", "keylight_nxp", "keylight_loader"))):
    command += ["-I", str(path)]
command += [str(path) for path in sources] + ([] if os.name == "nt" else ["-lm"]) + ["-o", str(exe)]
subprocess.run(command, check=True)
env = os.environ.copy()
env["ASAN_OPTIONS"] = f"detect_leaks={0 if os.name == 'nt' else 1}:halt_on_error=1"
result = subprocess.run([str(exe)], capture_output=True, text=True, env=env)
print(result.stdout, end="")
if result.returncode:
    print(result.stderr, file=sys.stderr, end="")
    result.check_returncode()
recovery_source = HERE / "test_recovery.c"
recovery_exe = OUT / ("test_recovery.exe" if os.name == "nt" else "test_recovery")
recovery_command = [str(recovery_source) if part == str(sources[0]) else part for part in command]
recovery_command[-1] = str(recovery_exe)
subprocess.run(recovery_command, check=True)
recovery_result = subprocess.run([str(recovery_exe)], capture_output=True, text=True, env=env)
print(recovery_result.stdout, end="")
if recovery_result.returncode:
    print(recovery_result.stderr, file=sys.stderr, end="")
    recovery_result.check_returncode()
sources += [recovery_source]
sources += [ROOT / "firmware/main/controller_worker.c", ROOT / "firmware/main/controller_worker.h",
            ROOT / "firmware/main/controller_diagnostic.h",
            ROOT / "firmware/main/controller_job.h", ROOT / "firmware/main/nxp_transport.h",
            ROOT / "tests/worker/worker_mocks.h", Path(__file__),
            components / "keylight_loader/include/okl_loader.h", components / "keylight_nxp/include/okl_nxp.h"]
report = {"status": "pass", "sanitizer": "AddressSanitizer", "device_operations": 0,
          "output": (result.stdout + recovery_result.stdout).strip(), "source_sha256": {
              p.relative_to(ROOT).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest() for p in sources},
          "limits": ["SPI, native driver, clock, SHA and durable storage boundaries mocked",
                     "Actual controller-worker callbacks, loader sequencing, report codec and output policy compiled",
                     "SHA256 is an equality oracle here; real SHA256 cross-language validation is in tests/loader",
                     "Physical transport, RTOS concurrency, final FD confirmation and journal clearing are separate tests"]}
(OUT / "result.json").write_text(json.dumps(report, indent=2) + "\n")
