"""Actual storage and HTTP handlers with host cJSON and deterministic IDF mocks."""
from pathlib import Path
import hashlib
import json
import os
import shutil
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
OUT = ROOT / "build" / "service-tests"
OUT.mkdir(parents=True, exist_ok=True)
compiler = os.environ.get("CC") or shutil.which("clang") or shutil.which("cc")
if not compiler:
    raise SystemExit("Set CC to a host C compiler; libcJSON development headers/library are required")
stubs = OUT / "stubs"
for header in ["esp_err.h", "esp_random.h", "esp_app_desc.h", "esp_system.h", "esp_partition.h", "esp_http_server.h", "freertos/FreeRTOS.h", "freertos/semphr.h", "mbedtls/sha256.h", "nvs.h", "nvs_flash.h", "web_assets.h"]:
    path = stubs / header
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text('#include "service_mocks.h"\n')
for header in ["esp_event.h", "esp_netif.h", "esp_wifi.h", "freertos/task.h", "mdns.h"]:
    path = stubs / header
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text('#include "network_mocks.h"\n')
exe = OUT / ("test_services.exe" if os.name == "nt" else "test_services")
components = ROOT / "firmware/components"
include_dirs = [HERE, stubs, components / "keylight_core/include", components / "keylight_json/include", Path(os.environ.get("CJSON_INCLUDE_DIR", "/usr/include/cjson"))]
command = [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wpedantic", "-g", "-O1", "-fsanitize=address", "-fno-omit-frame-pointer"]
for path in include_dirs:
    command += ["-I", str(path)]
command += [str(HERE / "test_services.c"), str(components / "keylight_core/keylight_core.c"), str(components / "keylight_json/keylight_json.c"), str(components / "keylight_json/keylight_policy.c"), "-lcjson", "-lm", "-o", str(exe)]
subprocess.run(command, check=True)
env = os.environ.copy()
env["ASAN_OPTIONS"] = f"detect_leaks={0 if os.name == 'nt' else 1}:halt_on_error=1"
def run(path):
    result = subprocess.run([str(path)], capture_output=True, text=True, env=env)
    if result.returncode:
        print(result.stdout, end="")
        print(result.stderr, file=sys.stderr, end="")
        result.check_returncode()
    print(result.stdout, end="")
    return result.stdout.strip()
outputs = [run(exe)]
network_exe = OUT / ("test_network.exe" if os.name == "nt" else "test_network")
network_command = command[:command.index(str(HERE / "test_services.c"))]
network_command += [str(HERE / "test_network.c"), "-o", str(network_exe)]
subprocess.run(network_command, check=True)
outputs.append(run(network_exe))
sources = [ROOT / f"firmware/main/{name}" for name in ["storage.c", "scene_store.c", "scene_store.h", "http_server.c", "network.c", "app.h"]]
sources += [HERE / name for name in ["test_services.c", "service_mocks.h", "test_network.c", "network_mocks.h", "run_tests.py"]]
report = {"status": "pass", "device_operations": 0, "sanitizer": "AddressSanitizer", "output": outputs, "source_sha256": {path.relative_to(ROOT).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest() for path in sources}, "limits": ["IDF/NVS/socket/crypto boundaries mocked; real cJSON and actual service sources used", "Single-record atomicity relies on NVS; physical flash and reset failure behavior not executed"]}
(OUT / "result.json").write_text(json.dumps(report, indent=2) + "\n")
