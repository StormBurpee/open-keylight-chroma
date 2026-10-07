"""Compile the actual settings API against real cJSON and bounded storage spies."""
from pathlib import Path
import hashlib
import json
import os
import shutil
import subprocess

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
OUT = ROOT / "build/config-tests"
OUT.mkdir(parents=True, exist_ok=True)
compiler = os.environ.get("CC") or shutil.which("clang") or shutil.which("cc")
if not compiler:
    raise SystemExit("Set CC to a host ASan compiler; cJSON development headers/library required")
stubs = OUT / "stubs"
for name in ["esp_err.h", "esp_system.h", "esp_http_server.h", "freertos/FreeRTOS.h", "freertos/semphr.h", "freertos/task.h"]:
    path = stubs / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text('#include "config_mocks.h"\n')
components = ROOT / "firmware/components"
sources = [HERE / "test_config.c", components / "keylight_core/keylight_core.c",
           components / "keylight_json/keylight_json.c", components / "keylight_json/keylight_policy.c"]
exe = OUT / ("test_config.exe" if os.name == "nt" else "test_config")
command = [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wpedantic", "-O1", "-g", "-fsanitize=address", "-fno-omit-frame-pointer"]
for path in [HERE, stubs, ROOT / "firmware/main", components / "keylight_core/include", components / "keylight_json/include", Path(os.environ.get("CJSON_INCLUDE_DIR", "/usr/include/cjson"))]:
    command += ["-I", str(path)]
subprocess.run(command + list(map(str,sources)) + ["-lcjson", "-lm", "-o", str(exe)], check=True)
env = os.environ.copy()
env["ASAN_OPTIONS"] = f"detect_leaks={0 if os.name == 'nt' else 1}:halt_on_error=1"
result = subprocess.run([str(exe)], env=env, text=True, capture_output=True)
print(result.stdout, end="")
if result.returncode:
    print(result.stderr, end=""); result.check_returncode()
sources += [ROOT / "firmware/main/config_api.c", ROOT / "firmware/main/app.h", HERE / "config_mocks.h", Path(__file__)]
(OUT / "result.json").write_text(json.dumps({"status":"pass", "sanitizer":"AddressSanitizer", "device_operations":0,
    "output":result.stdout.strip(), "source_sha256":{str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in sources},
    "limits":["Actual config API and cJSON used; storage persistence tested separately in services suite"]}, indent=2)+"\n")
