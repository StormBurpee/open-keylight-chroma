"""Compile the real flash admission guard with 1 ms and 10 ms RTOS ticks."""
from pathlib import Path
import hashlib
import json
import os
import shutil
import subprocess

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
OUT = ROOT / 'build/flash-guard-tests'
OUT.mkdir(parents=True, exist_ok=True)
compiler = os.environ.get('CC') or shutil.which('clang') or shutil.which('cc')
if not compiler:
    raise SystemExit('Set CC to an AddressSanitizer-capable compiler')
stubs = OUT / 'stubs'
for name in ('esp_err.h', 'esp_timer.h', 'freertos/FreeRTOS.h', 'freertos/semphr.h'):
    path = stubs / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text('#include "guard_mocks.h"\n')
outputs = []
for tick in (1, 10):
    exe = OUT / (f'guard-{tick}' + ('.exe' if os.name == 'nt' else ''))
    command = [compiler, '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wpedantic', '-O1', '-g',
               '-fsanitize=address,undefined', '-fno-omit-frame-pointer', f'-DMOCK_TICK_MS={tick}',
               '-I', str(HERE), '-I', str(stubs), str(HERE / 'test_guard.c'), '-o', str(exe)]
    subprocess.run(command, check=True)
    result = subprocess.run([str(exe)], check=True, text=True, capture_output=True)
    print(result.stdout, end=''); outputs.append(result.stdout.strip())
files = [HERE / 'test_guard.c', HERE / 'guard_mocks.h', ROOT / 'firmware/main/flash_guard.c', ROOT / 'firmware/main/flash_guard.h']
(OUT / 'result.json').write_text(json.dumps({'status': 'pass', 'device_operations': 0, 'outputs': outputs,
    'source_sha256': {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in files}}, indent=2) + '\n')
