"""Actual pure predicates/codec under ASan; no hardware or network access."""
from pathlib import Path
import hashlib
import json
import os
import shutil
import subprocess

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
OUT = ROOT / "build/controller-diagnostic-tests"
OUT.mkdir(parents=True, exist_ok=True)
compiler = os.environ.get("CC") or shutil.which("clang") or shutil.which("cc")
if not compiler:
    raise SystemExit("Set CC to an AddressSanitizer-capable compiler")
driver = ROOT / "firmware/components/keylight_nxp"
exe = OUT / ("test_diagnostic.exe" if os.name == "nt" else "test_diagnostic")
sources = [HERE / "test_diagnostic.c", driver / "okl_nxp.c"]
command = [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wpedantic", "-g", "-O1",
           "-fsanitize=address", "-fno-omit-frame-pointer", "-I", str(ROOT / "firmware/main"),
           "-I", str(driver / "include"), *map(str, sources), "-o", str(exe)]
subprocess.run(command, check=True)
env = os.environ.copy()
env["ASAN_OPTIONS"] = f"detect_leaks={0 if os.name == 'nt' else 1}:halt_on_error=1"
result = subprocess.run([str(exe)], check=True, text=True, capture_output=True, env=env)
sources += [driver / "include/okl_nxp.h", ROOT / "firmware/main/controller_diagnostic.h", Path(__file__)]
(OUT / "result.json").write_text(json.dumps({"status": "pass", "device_operations": 0,
    "output": result.stdout.strip(), "sanitizer": "AddressSanitizer",
    "source_sha256": {p.relative_to(ROOT).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest() for p in sources},
    "limits": ["Register records are synthetic contract fixtures, not measured optical/electrical evidence",
               "No worker integration, SPI hardware timing, or LOW1 execution is exercised"]}, indent=2) + "\n")
print(result.stdout, end="")
