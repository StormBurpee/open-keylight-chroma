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
compiler = os.environ.get("CC") or shutil.which("clang") or shutil.which("cc")
if not compiler:
    raise SystemExit("Set CC to an AddressSanitizer-capable C compiler.")
exe = build / ("nxp_tests.exe" if os.name == "nt" else "nxp_tests")
sources = [root / "src/nxp_app.c", root / "tests/test_nxp.c", driver / "okl_nxp.c"]
if not sources[-1].is_file():
    sources[-1] = driver / "src/okl_nxp.c"
core = root.parent / "firmware/components/keylight_core"
sources += [root.parent / "firmware/main/output_policy.c", core / "keylight_core.c"]
command = [compiler, "-std=c99", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-pedantic",
           "-fsanitize=address", "-fno-omit-frame-pointer", "-I", str(root / "include"),
           "-I", str(driver / "include"), "-I", str(core / "include"),
           "-I", str(root.parent / "firmware/main"), *map(str, sources),
           *([] if os.name == "nt" else ["-lm"]), "-o", str(exe)]
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
extra_results = []
for name, extra_sources in (
    ("pwm", [root / "tests/test_pwm.c"]),
    ("pwm_off", [root / "tests/test_pwm_off.c", root / "src/nxp_pwm_off_trial.c"]),
    ("pwm_low", [root / "tests/test_pwm_low.c", root / "src/nxp_pwm_low_trial.c"]),
):
    test_sources = board_sources[:2] + extra_sources
    test_exe = build / (f"{name}_tests.exe" if os.name == "nt" else f"{name}_tests")
    test_command = board_command[:board_command.index(str(board_sources[0]))]
    if name == "pwm_low":
        test_command += ["-DNXP_PWM_LOW_TRIAL=1", "-I", str(root.parent / "firmware/main"),
                         "-I", str(driver / "include")]
    test_command += [*map(str, test_sources), "-o", str(test_exe)]
    subprocess.run(test_command, check=True)
    completed = subprocess.run([str(test_exe)], capture_output=True, text=True)
    print(completed.stdout + completed.stderr, end="")
    (build / f"{name}-tests.log").write_text(completed.stdout + completed.stderr)
    extra_results.append(completed)
    sources += extra_sources
exit_code = tested.returncode or board_tested.returncode or next((r.returncode for r in extra_results if r.returncode), 0)
result = {"status": "pass" if exit_code == 0 else "fail", "device_operations": 0,
          "sanitizer": "AddressSanitizer", "summary": tested.stdout.strip(),
          "board_summary": board_tested.stdout.strip(),
          "pwm_summaries": [r.stdout.strip() for r in extra_results],
          "source_sha256": {str(p.relative_to(root.parent)): hashlib.sha256(p.read_bytes()).hexdigest()
                            for p in sources + board_sources + list((root / "include").glob("*.h"))},
          "limitations": "Mocks and pure arithmetic only; electrical timing, power and boot unqualified."}
(build / "host-results.json").write_text(json.dumps(result, indent=2) + "\n")
raise SystemExit(exit_code)
