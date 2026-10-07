import {test} from 'node:test';
import assert from 'node:assert/strict';
import {realpathSync} from 'node:fs';
import {mkdtemp, mkdir, writeFile, rm} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import {pythonRunner, findRepository} from '../src/backend.js';
import {pythonArguments} from '../src/python.js';
import {execFileSync} from 'node:child_process';

const python = process.env['OKL_PYTHON'] ?? (process.platform === 'win32' ? 'python' : 'python3');
async function fixture(body: string, fn: (root: string) => Promise<void>) {
  const root = await mkdtemp(join(tmpdir(), 'okl backend '));
  try {await mkdir(join(root, 'tools')); await mkdir(join(root, 'firmware')); await writeFile(join(root, 'tools/stock_migration.py'), body); await fn(root);}
  finally {await rm(root, {recursive: true, force: true});}
}
test('actual Python subprocess preserves split UTF-8 and argv metacharacters', async () => {
  await fixture('import sys,json,time\nb=json.dumps({"text":"🌈 café","args":sys.argv[1:]},ensure_ascii=False).encode()\nfor c in b:\n sys.stdout.buffer.write(bytes([c]));sys.stdout.buffer.flush()\n', async root => {
    assert.equal(findRepository(join(root, 'installer/dist/src')), root);
    const result = await pythonRunner(python, root)('stock_migration.py', ['prepare', '--manifest', 'x $(not-a-command); y']) as {text: string; args: string[]};
    assert.equal(result.text, '🌈 café'); assert.equal(result.args.at(-1), 'x $(not-a-command); y');
  });
});
test('offline runner refuses unknown/live scripts and pre-aborted work without spawning', async () => {
  const run = pythonRunner('executable-that-does-not-exist');
  await assert.rejects(run('evil.py', []), /Unknown backend/);
  await assert.rejects(run('stock_migration.py', ['install']), /Live events/);
  await assert.rejects(run('stock_migration.py', ['prepare'], AbortSignal.abort()), /Cancelled before/);
});

test('shared bootstrap imports adjacent modules with isolated Python and preserves script argv', async () => {
  await fixture('import json,sys,companion\nprint(json.dumps({"value":companion.VALUE,"args":sys.argv}))', async root => {
    await writeFile(join(root, 'tools/companion.py'), 'VALUE = 42\n');
    const args = pythonArguments(root, 'stock_migration.py', ['prepare', 'spaces & $(literal)']);
    const result = JSON.parse(execFileSync(python, ['-I', '-S', ...args], {encoding: 'utf8'}));
    assert.equal(result.value, 42);
    assert.equal(realpathSync.native(result.args[0]), realpathSync.native(join(root, 'tools/stock_migration.py')));
    assert.deepEqual(result.args.slice(1), ['prepare', 'spaces & $(literal)']);
    assert.throws(() => pythonArguments(root, '../outside.py', []), /Unknown/);
  });
});
test('actual backend rejects malformed, nonzero and oversized output; abort is bounded', async () => {
  for (const body of ["print('not-json')", "import sys\nprint('denied',file=sys.stderr)\nsys.exit(1)", "print('x'*270000)"]) {
    await fixture(body, async root => {await assert.rejects(pythonRunner(python, root)('stock_migration.py', ['prepare']));});
  }
  await fixture('import time\ntime.sleep(30)', async root => {
    const signal = new AbortController(); const work = pythonRunner(python, root)('stock_migration.py', ['prepare'], signal.signal);
    setTimeout(() => signal.abort(), 30); await assert.rejects(work, /cancelled/);
  });
});
