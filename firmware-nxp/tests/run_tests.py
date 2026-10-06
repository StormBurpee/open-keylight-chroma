"""Host software contracts only; never opens a device or network connection."""
from pathlib import Path
import hashlib
import json
import os
import shutil
import subprocess

root = Path(__file__).resolve().parents[1]
build = root / "build"
build.mkdir(exist_ok=True)
driver = root.parent / "firmware/components/keylight_nxp"
compiler = os.environ.get("CC") or shutil.which("clang")
if not compiler:
    raise SystemExit("clang is required for this AddressSanitizer suite; add it to PATH or set CC.")
exe = build / ("nxp_tests.exe" if os.name == "nt" else "nxp_tests")
sources = [root / "src/nxp_app.c", root / "tests/test_nxp.c", driver / "okl_nxp.c"]
if not sources[-1].is_file():
    sources[-1] = driver / "src/okl_nxp.c"
command = [compiler, "-std=c99", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-pedantic",
           "-fsanitize=address", "-fno-omit-frame-pointer", "-I", str(root / "include"),
           "-I", str(driver / "include"), *map(str, sources), "-o", str(exe)]
compiled = subprocess.run(command, capture_output=True, text=True)
(build / "host-build.log").write_text(compiled.stdout + compiled.stderr)
if compiled.returncode:
    raise SystemExit(compiled.stdout + compiled.stderr)
tested = subprocess.run([str(exe)], capture_output=True, text=True)
print(tested.stdout + tested.stderr, end="")
(build / "host-tests.log").write_text(tested.stdout + tested.stderr)
board_sources = [root / "src/nxp_app.c", root / "src/nxp_board.c", root / "tests/test_board.c"]
board_exe = build / ("board_tests.exe" if os.name == "nt" else "board_tests")
board_command = [compiler, "-std=c99", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-pedantic",
                 "-fsanitize=address", "-fno-omit-frame-pointer", "-I", str(root / "include"),
                 *map(str, board_sources), "-o", str(board_exe)]
board_compiled = subprocess.run(board_command, capture_output=True, text=True)
(build / "board-build.log").write_text(board_compiled.stdout + board_compiled.stderr)
if board_compiled.returncode:
    raise SystemExit(board_compiled.stdout + board_compiled.stderr)
board_tested = subprocess.run([str(board_exe)], capture_output=True, text=True)
print(board_tested.stdout + board_tested.stderr, end="")
(build / "board-tests.log").write_text(board_tested.stdout + board_tested.stderr)
result = {"status": "pass" if tested.returncode == board_tested.returncode == 0 else "fail", "device_operations": 0,
          "sanitizer": "AddressSanitizer", "summary": tested.stdout.strip(),
          "board_summary": board_tested.stdout.strip(),
          "source_sha256": {str(p.relative_to(root.parent)): hashlib.sha256(p.read_bytes()).hexdigest()
                            for p in sources + board_sources + list((root / "include").glob("*.h"))},
          "limitations": "Mocks and pure arithmetic only; electrical timing, power and boot unqualified."}
(build / "host-results.json").write_text(json.dumps(result, indent=2) + "\n")
raise SystemExit(tested.returncode or board_tested.returncode)
