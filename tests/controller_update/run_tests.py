"""Exercise the actual controller HTTP upload handler with bounded IDF mocks."""
from pathlib import Path
import os
import shutil
import subprocess

here = Path(__file__).resolve().parent
root = here.parents[1]
out = root / "build/controller-http-tests"
out.mkdir(parents=True, exist_ok=True)
compiler = os.environ.get("CC") or shutil.which("clang") or shutil.which("cc")
if not compiler:
    raise SystemExit("An AddressSanitizer-capable C compiler is required")
stubs = out / "stubs"
for name in ("mbedtls/sha256.h", "lwip/sockets.h"):
    path = stubs / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text('#include "http_mocks.h"\n')
binary = out / ("test_http.exe" if os.name == "nt" else "test_http")
command = [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wpedantic", "-O1", "-g",
           "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
for path in (here, stubs, root / "firmware/components/keylight_loader/include",
             root / "firmware/components/keylight_nxp/include"):
    command.extend(("-I", str(path)))
subprocess.run([*command, str(here / "test_http.c"), "-o", str(binary)], check=True)
env = os.environ.copy()
env["ASAN_OPTIONS"] = f"detect_leaks={0 if os.name == 'nt' else 1}:halt_on_error=1"
subprocess.run([str(binary)], check=True, env=env)
