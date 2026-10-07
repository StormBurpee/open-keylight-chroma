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
for header in ("esp_err.h", "esp_system.h", "cJSON.h", "freertos/FreeRTOS.h", "freertos/semphr.h", "freertos/task.h"):
    path = stubs / header
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text('#include "worker_mocks.h"\n')
for header in ("esp_log.h", "esp_mac.h", "esp_timer.h", "nvs_flash.h"):
    (stubs / header).write_text('#include "app_mocks.h"\n')
(stubs / "mbedtls").mkdir(exist_ok=True)
(stubs / "mbedtls/sha256.h").write_text('#include <stddef.h>\nint mbedtls_sha256(const unsigned char *,size_t,unsigned char[32],int);\n')
exe = OUT / ("test_worker.exe" if os.name == "nt" else "test_worker")
components = ROOT / "firmware/components"
command = [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wpedantic", "-g", "-O1",
           "-fsanitize=address", "-fno-omit-frame-pointer"]
for path in (HERE, stubs, ROOT / "firmware/main", components / "keylight_core/include",
             components / "keylight_json/include", components / "keylight_nxp/include",
             components / "keylight_loader/include"):
    command += ["-I", str(path)]
sources = [HERE / "test_worker.c", ROOT / "firmware/main/output_policy.c",
           ROOT / "firmware/main/update_indicator.c",
           components / "keylight_core/keylight_core.c", components / "keylight_nxp/okl_nxp.c"]
base_command = command.copy()
command += [str(path) for path in sources] + ([] if os.name == "nt" else ["-lm"]) + ["-o", str(exe)]
subprocess.run(command, check=True)
env = os.environ.copy()
env["ASAN_OPTIONS"] = f"detect_leaks={0 if os.name == 'nt' else 1}:halt_on_error=1"
result = subprocess.run([str(exe)], capture_output=True, text=True, env=env)
if result.returncode:
    print(result.stdout, end="")
    print(result.stderr, file=sys.stderr, end="")
    result.check_returncode()
app_exe = OUT / ("test_app.exe" if os.name == "nt" else "test_app")
app_sources = [HERE / "test_app.c", components / "keylight_core/keylight_core.c"]
subprocess.run(base_command + [str(path) for path in app_sources]
               + ([] if os.name == "nt" else ["-lm"]) + ["-o", str(app_exe)], check=True)
app_result = subprocess.run([str(app_exe)], capture_output=True, text=True, env=env)
if app_result.returncode:
    print(app_result.stdout, end="")
    print(app_result.stderr, file=sys.stderr, end="")
    app_result.check_returncode()
sources += app_sources + [ROOT / "firmware/main/app.c", HERE / "app_mocks.h"]
adapter_exe = OUT / ("test_controller_worker.exe" if os.name == "nt" else "test_controller_worker")
adapter_sources = [HERE / "test_controller_worker.c", ROOT / "firmware/main/output_policy.c",
                   components / "keylight_core/keylight_core.c", components / "keylight_nxp/okl_nxp.c"]
subprocess.run(base_command + [str(path) for path in adapter_sources]
               + ([] if os.name == "nt" else ["-lm"]) + ["-o", str(adapter_exe)], check=True)
adapter_result = subprocess.run([str(adapter_exe)], capture_output=True, text=True, env=env)
if adapter_result.returncode:
    print(adapter_result.stdout, end=""); print(adapter_result.stderr, file=sys.stderr, end="")
    adapter_result.check_returncode()
sources += adapter_sources + [ROOT / "firmware/main/controller_worker.c", ROOT / "firmware/main/controller_worker.h"]
sources += [ROOT / "firmware/main/worker.c", ROOT / "firmware/main/app.h",
            ROOT / "firmware/main/update_indicator_output.c", ROOT / "firmware/main/update_indicator_output.h",
            ROOT / "firmware/main/update_indicator.h",
            ROOT / "firmware/main/output_policy.h", ROOT / "firmware/main/controller_job.h",
            components / "keylight_loader/include/okl_loader.h", HERE / "worker_mocks.h", Path(__file__)]
report = {"status": "pass", "sanitizer": "AddressSanitizer", "device_operations": 0,
          "output": (result.stdout + app_result.stdout + adapter_result.stdout).strip(), "source_sha256": {
              path.relative_to(ROOT).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest() for path in sources},
          "limits": ["RTOS scheduling and NXP exchange boundaries mocked; real worker, output policy and frame arithmetic used",
                     "Driver ownership atomicity and physical output timing are covered separately"]}
(OUT / "result.json").write_text(json.dumps(report, indent=2) + "\n")
print(result.stdout, end="")
print(app_result.stdout, end="")
print(adapter_result.stdout, end="")
