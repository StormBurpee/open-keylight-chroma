import {test} from 'node:test';
import assert from 'node:assert/strict';
import {mkdtemp, mkdir, writeFile, rm} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import {display, privateIPv4, emptyDraft, draftErrors, parseSummary} from '../src/model.js';
import {edit} from '../src/input.js';
import {discoverArtifacts} from '../src/artifacts.js';
import {createPlan, acquireRestore, findRepository} from '../src/backend.js';
import {summary} from './fixtures.js';

test('target validation and terminal strings are bounded', () => {
  for (const ip of ['10.0.0.2', '172.16.0.1', '172.31.255.2', '192.168.1.2']) assert.equal(privateIPv4(ip), true);
  for (const ip of ['127.0.0.1', '8.8.8.8', '172.32.0.1', '192.168.001.2', '192.168.1.256', 'localhost']) assert.equal(privateIPv4(ip), false);
  assert.equal(display('\x1b[31mRED\x1b[0m\n\x07'), 'RED  ');
  assert.ok(draftErrors(emptyDraft()).length > 6);
  assert.deepEqual(parseSummary(summary), summary);
  for (const override of [{device_operations: 1}, {manifest_sha256: 'bad'}, {packages: {}}, {esp: {...summary.esp, bytes: 2000000}}]) assert.throws(() => parseSummary({...summary, ...override}));
});
test('editing preserves Unicode cursor and distinguishes delete/backspace', () => {
  assert.deepEqual(edit('a🌈b', 2, '', {backspace: true}), {value: 'ab', cursor: 1});
  assert.deepEqual(edit('a🌈b', 1, '', {delete: true}), {value: 'ab', cursor: 1});
  assert.deepEqual(edit('ab', 1, '🌈', {}), {value: 'a🌈b', cursor: 2});
  assert.deepEqual(edit('a', 1, '\x1b[31m', {}), {value: 'a', cursor: 1});
  assert.deepEqual(edit('a', 1, 'u', {ctrl: true}), {value: '', cursor: 0});
  assert.equal(edit('x'.repeat(1000), 1000, 'y', {}).value.length, 1000);
});
test('discovery offers unique local names and refuses ambiguous ESP candidates', async () => {
  const dir = await mkdtemp(join(tmpdir(), 'okl artifacts '));
  try {
    await writeFile(join(dir, 'identity.oklnxp'), 'fixture');
    await writeFile(join(dir, 'open-keylight-one.bin'), 'a');
    await writeFile(join(dir, 'open-keylight-two.bin'), 'b');
    await writeFile(join(dir, 'asset-manifest.json'), '{}');
    await mkdir(join(dir, 'LOW1.oklnxp'));
    const result = await discoverArtifacts(dir);
    assert.equal(result.suggested.identity, join(dir, 'identity.oklnxp'));
    assert.equal(result.suggested.low1, undefined);
    assert.equal(result.suggested.esp, undefined);
    assert.deepEqual(result.ambiguous, ['esp']);
    assert.equal(result.files.length, 4);
  } finally {await rm(dir, {recursive: true, force: true});}
});
test('plan builder sends literal argument values, then revalidates; no retry after failure', async () => {
  const d = {...emptyDraft(), ip: summary.target_ip, name: summary.target_name, deviceId: summary.device_id, commit: 'a'.repeat(40), identity: 'a $(echo nope).oklnxp', off1: 'b', low1: 'c', lighting: 'd', esp: 'e', assets: 'f', restore: 'g', output: 'new plan.json', provenance: 'Explicit fixture provenance'};
  const calls: {script: string; args: string[]}[] = [];
  await createPlan(d, async (script, args) => {calls.push({script, args}); return summary;});
  assert.equal(calls.length, 2);
  assert.ok(calls[0]!.args.some(a => a.endsWith('a $(echo nope).oklnxp')));
  assert.deepEqual(calls[1]!.args.slice(0, 2), ['prepare', '--manifest']);
  let count = 0;
  await assert.rejects(createPlan(d, async () => {count++; throw new Error('Exists');}), /Exists/);
  assert.equal(count, 1);
});
test('vendor helper must declare exact derived artifact, never a device backup', async () => {
  const path = join(tmpdir(), 'restore.bin');
  const v = {format: 1, profile: summary.profile, path, bytes: 28672, sha256: 'f46d19f50bba9fd97c6c3e207557b15470ad05d1877402a0cd15e0adc3a87a96', version: '1.3.0.0', cached: true, source_url: 'https://mobileapp-assets.razerzone.com/iOS/Jade/T1/02.03.13.00.zip', archive_sha256: 'a'.repeat(64), provenance: 'Pinned vendor-derived fixture', device_backup: false, device_operations: 0};
  assert.equal((await acquireRestore(path, async () => v)).cached, true);
  for (const bad of [{device_backup: true}, {sha256: 'b'.repeat(64)}, {source_url: 'http://untrusted.invalid'}, {bytes: 1}]) await assert.rejects(acquireRestore(path, async () => ({...v, ...bad})));
  assert.ok(findRepository().endsWith('open-keylight'));
});
