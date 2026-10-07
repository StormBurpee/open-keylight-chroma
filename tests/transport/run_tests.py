"""Test the real ESP transport and protocol driver using deterministic SDK boundaries."""
from pathlib import Path
import hashlib
import json
import os
import shutil
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
OUT = ROOT / "build/transport-tests"
OUT.mkdir(parents=True, exist_ok=True)
compiler = os.environ.get("CC") or shutil.which("clang") or shutil.which("cc")
if not compiler:
    raise SystemExit("Set CC to an AddressSanitizer-capable C compiler")
stubs = OUT / "stubs"
for header in ("esp_err.h", "esp_timer.h", "driver/gpio.h", "driver/spi_master.h",
               "freertos/FreeRTOS.h", "freertos/semphr.h", "freertos/task.h"):
    path = stubs / header
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text('#include "transport_mocks.h"\n')
exe = OUT / ("test_transport.exe" if os.name == "nt" else "test_transport")
driver = ROOT / "firmware/components/keylight_nxp"
command = [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wpedantic", "-g", "-O1",
           "-fsanitize=address", "-fno-omit-frame-pointer"]
for path in (HERE, stubs, ROOT / "firmware/main", driver / "include"):
    command += ["-I", str(path)]
sources = [HERE / "test_transport.c", driver / "okl_nxp.c"]
command += [str(path) for path in sources] + ["-o", str(exe)]
subprocess.run(command, check=True)
env = os.environ.copy()
env["ASAN_OPTIONS"] = f"detect_leaks={0 if os.name == 'nt' else 1}:halt_on_error=1"
result = subprocess.run([str(exe)], capture_output=True, text=True, env=env)
if result.returncode:
    print(result.stdout, end="")
    print(result.stderr, file=sys.stderr, end="")
    result.check_returncode()
sources += [ROOT / "firmware/main/nxp_transport.c", ROOT / "firmware/main/nxp_transport.h",
            driver / "include/okl_nxp.h", HERE / "transport_mocks.h", Path(__file__)]
report = {"status": "pass", "sanitizer": "AddressSanitizer", "device_operations": 0,
          "output": result.stdout.strip(), "source_sha256": {
              path.relative_to(ROOT).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest() for path in sources},
          "limits": ["GPIO, clock, RTOS mutex and SPI hardware calls are mocked; actual transport and protocol driver execute",
                     "Original expired response versus stock delayed response remains intentionally unresolved without peer evidence",
                     "Does not establish hardware SPI timing, DMA failure phase, or retained-body phase after ESP reset"]}
(OUT / "result.json").write_text(json.dumps(report, indent=2) + "\n")
print(result.stdout, end="")
