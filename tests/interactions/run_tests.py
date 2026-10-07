"""Actual MQTT and GPIO task sources with deterministic broker/RTOS boundaries."""
from pathlib import Path
import hashlib
import json
import os
import shutil
import subprocess

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
OUT = ROOT / "build/interaction-tests"
stubs = OUT / "stubs"
for name in ("esp_err.h", "freertos/FreeRTOS.h", "freertos/semphr.h", "freertos/task.h",
             "esp_app_desc.h", "esp_crt_bundle.h", "mqtt_client.h", "driver/gpio.h"):
    path = stubs / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text('#include "interaction_mocks.h"\n')
components = ROOT / "firmware/components"
compiler = os.environ.get("CC") or shutil.which("clang") or shutil.which("cc")
if not compiler:
    raise SystemExit("ASan-capable compiler and cJSON development library required")
base = [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wpedantic", "-g", "-O1",
        "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
for directory in (HERE, stubs, ROOT / "firmware/main", components / "keylight_core/include",
                  components / "keylight_json/include", components / "keylight_nxp/include",
                  components / "keylight_loader/include", Path(os.getenv("CJSON_INCLUDE_DIR", "/usr/include/cjson"))):
    base += ["-I", str(directory)]
env = os.environ.copy()
env["ASAN_OPTIONS"] = f"detect_leaks={0 if os.name == 'nt' else 1}:halt_on_error=1"
env["UBSAN_OPTIONS"] = "halt_on_error=1"
sources = [ROOT / f"firmware/main/{name}" for name in ("mqtt.c", "button.c", "app.h", "output_policy.c")]
outputs = []
for name, extra in {
    "mqtt": [ROOT / "firmware/main/output_policy.c", components / "keylight_core/keylight_core.c",
             components / "keylight_json/keylight_json.c", components / "keylight_nxp/okl_nxp.c"],
    "button": [components / "keylight_nxp/okl_button.c"],
}.items():
    test = HERE / f"test_{name}.c"
    executable = OUT / (f"test_{name}.exe" if os.name == "nt" else f"test_{name}")
    command = base + [str(test)] + [str(path) for path in extra] + ["-lcjson", "-lm", "-o", str(executable)]
    subprocess.run(command, check=True)
    result = subprocess.run([str(executable)], capture_output=True, text=True, env=env)
    print(result.stdout, end="")
    if result.returncode:
        print(result.stderr, end="")
        result.check_returncode()
    outputs.append(result.stdout.strip())
    sources += [test] + extra
sources += [HERE / "interaction_mocks.h", Path(__file__)]
(OUT / "result.json").write_text(json.dumps({"status": "pass", "device_operations": 0,
    "sanitizer": "AddressSanitizer+UndefinedBehaviorSanitizer", "output": outputs,
    "source_sha256": {p.relative_to(ROOT).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest() for p in sources},
    "limits": ["Broker event delivery and GPIO/task scheduling mocked; actual MQTT and gesture/task sources used"]}, indent=2) + "\n")
