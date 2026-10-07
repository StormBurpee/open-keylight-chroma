import assert from 'node:assert/strict';
import {execFileSync} from 'node:child_process';
import {mkdtemp, mkdir, copyFile, writeFile, rm} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join} from 'node:path';

const root = await mkdtemp(join(tmpdir(), 'open-keylight isolated release '));
try {
  await mkdir(join(root, 'installer')); await mkdir(join(root, 'tools')); await mkdir(join(root, 'firmware'));
  await writeFile(join(root, 'tools/stock_migration.py'), 'raise RuntimeError("Offline smoke test must never run a backend")\n');
  for (const name of ['cli.js', 'package.json', 'THIRD_PARTY_NOTICES.txt']) await copyFile(`dist/${name}`, join(root, 'installer', name));
  const run = args => execFileSync(process.execPath, [join(root, 'installer/cli.js'), '--root', root, '--python', 'MUST-NOT-SPAWN', ...args], {cwd: root, encoding: 'utf8', timeout: 15000});
  assert.match(run(['--help']), /Open Keylight/);
  for (const scene of ['discover', 'review', 'install', 'observe', 'complete']) assert.match(run(['--demo', scene]), /NO DEVICE ACTIVITY/);
  assert.match(run(['--preview', '--plain']), /Illustrative progress/);
  assert.throws(() => run(['--demo', 'unknown']));
  console.log('Isolated release smoke PASS: no node_modules, Python or network required for help/demo/preview.');
} finally {await rm(root, {recursive: true, force: true});}
