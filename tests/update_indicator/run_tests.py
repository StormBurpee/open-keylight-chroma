"""Test portable indicator math and actual coordinator with bounded driver mocks."""
from pathlib import Path
import hashlib
import json
import os
import shutil
import subprocess

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
OUT = ROOT / "build/update-indicator-tests"
OUT.mkdir(parents=True, exist_ok=True)
compiler = os.environ.get("CC") or shutil.which("clang") or shutil.which("cc")
if not compiler:
    raise SystemExit("Set CC to an AddressSanitizer-capable C compiler")
sources = [HERE / "test_indicator.c", ROOT / "firmware/main/update_indicator.c"]
exe = OUT / ("test_indicator.exe" if os.name == "nt" else "test_indicator")
subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wpedantic", "-O1", "-g",
                "-fsanitize=address", "-fno-omit-frame-pointer", "-I", str(ROOT / "firmware/main"),
                *map(str, sources), *([] if os.name == "nt" else ["-lm"]), "-o", str(exe)], check=True)
env = os.environ.copy()
env["ASAN_OPTIONS"] = f"detect_leaks={0 if os.name == 'nt' else 1}:halt_on_error=1"
result = subprocess.run([str(exe)], env=env, capture_output=True, text=True)
print(result.stdout, end="")
if result.returncode:
    print(result.stderr, end="")
    result.check_returncode()
sources += [ROOT / "firmware/main/update_indicator.h", Path(__file__)]
output_sources = [HERE / "test_output.c", ROOT / "firmware/main/update_indicator.c",
                  ROOT / "firmware/components/keylight_nxp/okl_nxp.c"]
output_exe = OUT / ("test_output.exe" if os.name == "nt" else "test_output")
subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wpedantic", "-O1", "-g",
                "-fsanitize=address", "-fno-omit-frame-pointer", "-I", str(ROOT / "firmware/main"),
                "-I", str(ROOT / "firmware/components/keylight_nxp/include"),
                *map(str, output_sources), "-o", str(output_exe)], check=True)
output_result = subprocess.run([str(output_exe)], env=env, capture_output=True, text=True)
print(output_result.stdout, end="")
if output_result.returncode:
    print(output_result.stderr, end=""); output_result.check_returncode()
sources += output_sources + [ROOT / "firmware/main/update_indicator_output.c",
                            ROOT / "firmware/main/update_indicator_output.h"]
(OUT / "result.json").write_text(json.dumps({"status":"pass", "device_operations":0,
    "sanitizer":"AddressSanitizer", "output":(result.stdout+output_result.stdout).strip(),
    "source_sha256":{str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in sources},
    "limits":["Driver exchange boundaries mocked; actual coordinator, requests, state and channel math used",
              "Optical appearance and physical scheduling require live qualification"]}, indent=2)+"\n")
