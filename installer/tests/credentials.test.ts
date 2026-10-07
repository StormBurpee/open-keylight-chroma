import {test} from 'node:test';
import assert from 'node:assert/strict';
import {mkdtemp, readFile, rm, writeFile} from 'node:fs/promises';
import {join} from 'node:path';
import {tmpdir} from 'node:os';
import {privateDirectory} from '../src/credentials.js';
import {nativeAcceptance} from '../src/native-session.js';
import {summary} from './fixtures.js';

test('Windows credential setup never resolves system tools from Git Bash or another PATH entry', {skip: process.platform !== 'win32'}, async () => {
  const root = await mkdtemp(join(tmpdir(), 'okl shadowed system tools '));
  const original = process.env['PATH'];
  try {
    await writeFile(join(root, 'whoami.exe'), 'not the Windows identity program');
    await writeFile(join(root, 'icacls.exe'), 'not the Windows ACL program');
    process.env['PATH'] = root;
    const directory = await privateDirectory(root);
    assert.equal(directory.startsWith(join(root, 'install-')), true);
  } finally {
    if (original === undefined) delete process.env['PATH']; else process.env['PATH'] = original;
    await rm(root, {recursive: true, force: true});
  }
});

test('a local Windows access failure is recorded before any pairing attempt', {skip: process.platform !== 'win32'}, async () => {
  const root = await mkdtemp(join(tmpdir(), 'okl access failure '));
  const audit = join(root, 'attempt'), systemRoot = process.env['SystemRoot'];
  try {
    process.env['SystemRoot'] = '';
    await assert.rejects(nativeAcceptance(summary, {manifest: summary.manifest_sha256, controllerVersion: '0.1.1.0', remainingMs: 120000}, audit, () => false, () => {}), /Windows system directory/);
    const evidence = JSON.parse(await readFile(`${audit}.native.json`, 'utf8'));
    assert.equal(evidence.accepted, false); assert.deepEqual(evidence.attempts, []);
    assert.match(evidence.error, /Windows system directory/);
  } finally {
    if (systemRoot === undefined) delete process.env['SystemRoot']; else process.env['SystemRoot'] = systemRoot;
    await rm(root, {recursive: true, force: true});
  }
});

test('late native admission leaves a useful audit without device operations', async () => {
  const root = await mkdtemp(join(tmpdir(), 'okl late acceptance ')), audit = join(root, 'attempt');
  try {
    await assert.rejects(nativeAcceptance(summary, {manifest: summary.manifest_sha256, controllerVersion: '0.1.1.0', remainingMs: 1}, audit, () => false, () => {}), /Not enough verified trial time/);
    const evidence = JSON.parse(await readFile(`${audit}.native.json`, 'utf8'));
    assert.equal(evidence.accepted, false); assert.deepEqual(evidence.attempts, []);
    assert.deepEqual(evidence.checks, []); assert.match(evidence.error, /Not enough verified trial time/);
  } finally {await rm(root, {recursive: true, force: true});}
});
