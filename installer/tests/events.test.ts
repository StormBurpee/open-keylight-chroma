import {test} from 'node:test';
import assert from 'node:assert/strict';
import {EventEmitter} from 'node:events';
import {PassThrough} from 'node:stream';
import type {ChildProcessWithoutNullStreams} from 'node:child_process';
import {EventLines, EventState} from '../src/events.js';
import {installationRunner} from '../src/execution.js';
import {stages} from '../src/model.js';
import {summary} from './fixtures.js';
import {backendStream} from './backend-stream.js';
import {parseSummary} from '../src/model.js';
const state = () => new EventState({ip: summary.target_ip, deviceId: summary.device_id, version: summary.esp.version, elf: summary.esp.elf_sha256, manifest: summary.manifest_sha256, controllerVersion: summary.controller_version});
const event = (s: EventState, e: Record<string, unknown>) => s.accept({v: 1, seq: s.lastSequence + 1, ...e});
const stage = (s: EventState, n: number, phase = 'started') => event(s, {event: 'stage', id: stages[n]![0], index: n + 1, total: 7, phase, message: 'Checked'});

test('seven measured stages are required; exit or counters never invent completion', () => {
  const s = state();
  assert.throws(() => event(s, {event: 'completed', outcome: 'installed'}));
  for (let i = 0; i < 7; i++) {stage(s, i); stage(s, i, 'completed');}
  event(s, {event: 'completed', outcome: 'installed'});
  assert.equal(s.terminal, 'installed');
  assert.throws(() => event(s, {event: 'status', code: 'extra', message: 'No'}));
});
test('strict sequence, stage order, counts and unknown events fail closed', () => {
  for (const e of [{v: 2, seq: 0, event: 'status'}, {v: 1, seq: 1, event: 'status'}, {v: 1, seq: 0, event: 'surprise'}]) assert.throws(() => state().accept(e));
  const s = state(); assert.throws(() => stage(s, 1)); stage(s, 0);
  const p = {event: 'progress', stage_id: 'stock', scope: 'controller_verify', completed: 1, total: 448, unit: 'blocks'};
  for (const bad of [{stage_id: 'esp'}, {completed: 449}, {completed: -1}, {total: 0}, {unit: 'guessed'}]) assert.throws(() => event(s, {...p, ...bad}));
  event(s, p); assert.equal(s.progress.completed, 1); assert.equal(s.terminal, undefined);
});
test('optical prompts require exact pending id and explicit answer; cancellation is single', () => {
  const s = state(); stage(s, 0);
  event(s, {event: 'prompt', id: 'random-1', kind: 'off1_observation', message: 'Was it dark?', choices: ['yes', 'no']});
  assert.throws(() => s.answer('old', 'yes'));
  assert.deepEqual(JSON.parse(s.answer('random-1', 'no')), {v: 1, id: 'random-1', answer: 'no'});
  assert.throws(() => s.answer('random-1', 'yes'));
  assert.throws(() => event(s, {event: 'prompt', id: 'random-1', kind: 'off1_observation', message: 'Duplicate', choices: ['yes', 'no']}));
  assert.equal(JSON.parse(s.cancel()!).command, 'cancel'); assert.equal(s.cancel(), undefined);
});
test('browser action must bind exact selected target and image with no credential URL', () => {
  const s = state(); for (let i = 0; i < 6; i++) {stage(s, i); stage(s, i, 'completed');} stage(s, 6);
  const a = {event: 'action', kind: 'native_acceptance', url: `http://${summary.target_ip}/`, device_id: summary.device_id, firmware: summary.esp.version, elf_sha256: summary.esp.elf_sha256, manifest_sha256: summary.manifest_sha256, controller_version: summary.controller_version, pairing_open: true, remaining_ms: 150000};
  for (const bad of [{url: 'https://example.com/'}, {url: `http://user:secret@${summary.target_ip}/`}, {firmware: 'wrong'}, {elf_sha256: '0'.repeat(64)}]) assert.throws(() => event(s, {...a, ...bad}));
  event(s, a); assert.equal(s.progress.action?.remainingSeconds, 150);
});
test('JSONL handles arbitrary UTF-8 byte boundaries and rejects partial, oversized, empty lines', () => {
  const values: unknown[] = [], lines = new EventLines(v => values.push(v));
  for (const b of Buffer.from(JSON.stringify({message: '🌈 éclair'}) + '\n')) lines.write(Buffer.from([b]));
  lines.end(); assert.deepEqual(values, [{message: '🌈 éclair'}]);
  const partial = new EventLines(() => {}); partial.write(Buffer.from('{')); assert.throws(() => partial.end());
  assert.throws(() => new EventLines(() => {}).write(Buffer.from('x'.repeat(65537))));
  assert.throws(() => new EventLines(() => {}).write(Buffer.from('\n')));
});
function fake() {
  const child = Object.assign(new EventEmitter(), {stdin: new PassThrough(), stdout: new PassThrough(), stderr: new PassThrough(), kill: () => {throw new Error('Never kill flashing child');}});
  return child as unknown as ChildProcessWithoutNullStreams & {stdout: PassThrough; stderr: PassThrough};
}
test('live launcher has one argv-only process; malformed stream requests safe cancel without killing', async () => {
  const child = fake(), writes: string[] = []; child.stdin.on('data', b => writes.push(b.toString()));
  let args: string[] = [], launches = 0;
  const run = installationRunner('python', '.', a => {args = a; launches++; return child;});
  const job = run('plan $(no).json', 'audit.jsonl', summary, () => {});
  child.stdout.write('not JSON\n'); child.emit('close', 1);
  await assert.rejects(job.done, /malformed/);
  assert.equal(launches, 1); assert.equal(args.at(-1), '--events-jsonl'); assert.ok(args.includes(summary.manifest_sha256));
  assert.deepEqual(writes.map(w => JSON.parse(w)), [{v: 1, command: 'cancel'}]);
});
test('process exit zero without terminal event is not success; stop propagates without retry', async () => {
  const child = fake(); const job = installationRunner('python', '.', () => child)('p', 'a', summary, () => {});
  child.emit('close', 0); await assert.rejects(job.done, /without a verified/);
  const stopped = fake(); const second = installationRunner('python', '.', () => stopped)('p', 'a', summary, () => {});
  stopped.stdout.write(JSON.stringify({v: 1, seq: 0, event: 'stopped', message: 'Check failed', error_type: 'Mismatch', automatic_retry: false, automatic_restore: false}) + '\n');
  stopped.emit('close', 1); assert.equal(await second.done, 'stopped');
});

test('actual Python mock-backend stream composes with strict reducer, prompts and fractional timing', () => {
  const plan = parseSummary(backendStream[0].summary);
  const s = new EventState({ip: plan.target_ip, deviceId: plan.device_id, version: plan.esp.version, elf: plan.esp.elf_sha256, manifest: plan.manifest_sha256, controllerVersion: plan.controller_version});
  const answers: string[] = [];
  for (const item of backendStream) {
    s.accept(item);
    if (item.event === 'prompt') answers.push(s.answer(item.id, 'yes'));
  }
  assert.equal(s.terminal, 'installed'); assert.equal(answers.length, 2);
  assert.equal(s.progress.dashboardUrl, 'http://192.168.1.25/');
  assert.deepEqual(s.progress.finishedStages, [0, 1, 2, 3, 4, 5, 6]);
});

test('native client waits for open pairing, runs once, and cannot turn backend-only completion into client success', async () => {
  const child = fake(), plan = parseSummary(backendStream[0].summary);
  let calls = 0, finish!: (path: string) => void, sequence = 0;
  let latest: import('../src/model.js').Progress | undefined;
  const job = installationRunner('python', '.', () => child, async () => {calls++; return new Promise(resolve => {finish = resolve;});})('p', 'a', plan, p => {latest = p;});
  const send = (item: object) => child.stdout.write(JSON.stringify({...item, seq: sequence++}) + '\n');
  child.stdin.on('data', () => {});
  for (const item of backendStream.slice(0, 21)) {
    send(item); if (item.event === 'prompt') job.answer(item.id, 'yes');
  }
  assert.equal(calls, 0);
  const action = backendStream[20];
  send({...action, pairing_open: true}); send({...action, pairing_open: true});
  assert.equal(calls, 1);
  send({...action, pairing_open: false});
  assert.equal(latest?.action?.pairingOpen, false); assert.equal(latest?.acceptanceState, 'running'); assert.equal(calls, 1);
  for (const item of backendStream.slice(21)) send(item);
  child.emit('close', 0);
  let settled = false; void job.done.then(() => {settled = true;});
  await Promise.resolve(); assert.equal(settled, false);
  finish('/private/credential.json'); assert.equal(await job.done, 'installed');
  assert.equal(latest?.acceptanceState, 'complete');
});
