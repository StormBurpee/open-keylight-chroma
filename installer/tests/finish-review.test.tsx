import React from 'react';
import {test} from 'node:test';
import assert from 'node:assert/strict';
import {EventEmitter} from 'node:events';
import {PassThrough} from 'node:stream';
import type {ChildProcessWithoutNullStreams} from 'node:child_process';
import {setTimeout as pause} from 'node:timers/promises';
import {render} from 'ink-testing-library';
import {App} from '../src/App.js';
import {EventState} from '../src/events.js';
import {installationRunner} from '../src/execution.js';
import {finishStages, type InstallationMode, type Progress} from '../src/model.js';
import {summary} from './fixtures.js';

const target = {ip: summary.target_ip, deviceId: summary.device_id, version: summary.esp.version,
  elf: summary.esp.elf_sha256, manifest: summary.manifest_sha256, controllerVersion: summary.controller_version, workflow: 'finish' as const};
const action = {event: 'action', kind: 'native_acceptance', url: `http://${summary.target_ip}/`,
  device_id: summary.device_id, firmware: summary.esp.version, elf_sha256: summary.esp.elf_sha256,
  manifest_sha256: summary.manifest_sha256, controller_version: summary.controller_version, pairing_open: true, remaining_ms: 90000};
const emit = (state: EventState, value: object) => state.accept({v: 1, seq: state.lastSequence + 1, ...value});
const stage = (index: number, phase: 'started' | 'completed') => ({event: 'stage', index: index + 1,
  total: 3, id: finishStages[index]![0], phase, message: 'Verified fixture'});

function fakeChild() {
  return Object.assign(new EventEmitter(), {stdin: new PassThrough(), stdout: new PassThrough(), stderr: new PassThrough(),
    kill: () => {throw new Error('A flashing backend must not be killed');}}) as unknown as ChildProcessWithoutNullStreams;
}
function stream(child: ChildProcessWithoutNullStreams) {
  let sequence = 0;
  return (value: object) => child.stdout.emit('data', Buffer.from(JSON.stringify({v: 1, seq: sequence++, ...value}) + '\n'));
}
function reachNative(send: (value: object) => void) {
  for (let i = 0; i < 2; i++) {send(stage(i, 'started')); send(stage(i, 'completed'));}
  send(stage(2, 'started')); send(action);
}

test('finish reducer binds the selected three-stage workflow and retains exact native identity gates', () => {
  const state = new EventState(target);
  assert.throws(() => emit(state, {event: 'status', code: 'installation_workflow', workflow: 'install', message: 'Wrong workflow'}));
  assert.throws(() => emit(state, {...stage(0, 'started'), total: 7}));
  assert.throws(() => emit(state, action));
  emit(state, stage(0, 'started'));
  assert.throws(() => emit(state, {event: 'prompt', id: 'unexpected', kind: 'off1_observation', message: 'Unexpected write profile', choices: ['yes', 'no']}));
  emit(state, stage(0, 'completed')); emit(state, stage(1, 'started')); emit(state, stage(1, 'completed'));
  assert.throws(() => emit(state, {event: 'completed', outcome: 'installed'}));
  emit(state, stage(2, 'started'));
  for (const changed of [{manifest_sha256: 'f'.repeat(64)}, {controller_version: '1.3.0.0'}, {device_id: 'keylight-000000'},
    {elf_sha256: 'f'.repeat(64)}, {url: `http://${summary.target_ip}/?token=secret`}]) assert.throws(() => emit(state, {...action, ...changed}));
  emit(state, action); emit(state, stage(2, 'completed')); emit(state, {event: 'completed', outcome: 'installed'});
  assert.equal(state.terminal, 'installed'); assert.equal(state.progress.workflow, 'finish');
  assert.deepEqual(state.progress.finishedStages, [0, 1, 2]);
});

test('finish launcher rejects mode injection before spawning and passes one literal mode and audit', async () => {
  let launches = 0; let args: string[] = [];
  const child = fakeChild(), run = installationRunner('python', '.', value => {launches++; args = value; return child;});
  assert.throws(() => run('selected plan.json', 'new audit.jsonl', summary, () => {}, 'finish --execute' as InstallationMode), /Unknown installation mode/);
  assert.equal(launches, 0);
  const job = run('selected plan.json', 'new audit.jsonl', summary, () => {}, 'finish');
  assert.equal(launches, 1); assert.equal(args.filter(value => value === 'finish').length, 1);
  assert.equal(args.includes('install'), false); assert.ok(args.includes(summary.manifest_sha256));
  stream(child)({event: 'stopped', message: 'Wrong controller', error_type: 'Mismatch', automatic_retry: false, automatic_restore: false});
  child.emit('close', 1); assert.equal(await job.done, 'stopped'); assert.equal(launches, 1);
});

test('backend stopped terminal cancels an in-flight native acceptance before any later mutation', async () => {
  const child = fakeChild(); let cancelled!: () => boolean; let complete!: (value: string) => void;
  const job = installationRunner('python', '.', () => child, async (_summary, _action, _audit, check) => {
    cancelled = check; return new Promise(resolve => {complete = resolve;});
  })('plan', 'audit', summary, () => {}, 'finish');
  const send = stream(child); reachNative(send); assert.equal(cancelled(), false);
  try {
    send({event: 'stopped', message: 'Native health failed', error_type: 'Mismatch', automatic_retry: false, automatic_restore: false});
    assert.equal(cancelled(), true, 'Backend stop must revoke the acceptance client before its next request');
  } finally {complete('/private/fixture'); child.emit('close', 1); await job.done.catch(() => {});}
});

test('unexpected backend close cancels pending acceptance before waiting for it to settle', async () => {
  const child = fakeChild(); let cancelled!: () => boolean; let complete!: (value: string) => void;
  const job = installationRunner('python', '.', () => child, async (_summary, _action, _audit, check) => {
    cancelled = check; return new Promise(resolve => {complete = resolve;});
  })('plan', 'audit', summary, () => {}, 'finish');
  const send = stream(child); reachNative(send); assert.equal(cancelled(), false);
  const result = job.done.catch(error => error);
  try {
    child.emit('close', 1);
    assert.equal(cancelled(), true, 'Unverified process exit must not leave native writes authorized');
  } finally {complete('/private/fixture'); assert.match(String(await result), /without a verified/);}
});

test('finish after an early stop needs a fresh explicit review, audit and exclusive-control choice', async () => {
  const calls: {audit: string; mode: InstallationMode | undefined}[] = [];
  let update!: (progress: Progress) => void, stop!: (result: 'stopped') => void;
  const screen = render(<App initialPlan="plan.json" run={async () => summary} start={(_plan, audit, _summary, publish, mode) => {
    calls.push({audit, mode}); update = publish;
    publish({workflow: mode, stage: 0, state: 'running', label: 'Checking selected controller'});
    return {answer: () => {}, cancel: () => {}, done: new Promise(resolve => {stop = resolve;})};
  }} />);
  const key = async (value: string) => {screen.stdin.write(value); await pause(40);};
  try {
    await pause(40); await key('\r'); await key('\r'); await key(' '); await key('\r');
    assert.equal(calls.length, 1); assert.equal(calls[0]!.mode, 'install');
    await key('f'); assert.equal(calls.length, 1); assert.doesNotMatch(screen.lastFrame()!, /FINISH YOUR INSTALLATION/);
    update({workflow: 'install', stage: 0, state: 'stopped', label: 'Controller already original'}); stop('stopped'); await pause(40);
    assert.match(screen.lastFrame()!, /Installation stopped/);
    assert.doesNotMatch(screen.lastFrame()!, /No firmware was uploaded/);
    assert.doesNotMatch(screen.lastFrame()!, /Controller already original|Audit:|Finish a previous|F ·/);
    await key('f'); assert.equal(calls.length, 1); assert.match(screen.lastFrame()!, /Installation stopped/);
    await key('d'); assert.match(screen.lastFrame()!, /Controller already original/); assert.match(screen.lastFrame()!, /Audit:/);
    assert.doesNotMatch(screen.lastFrame()!, /Finish a previous|F ·/);
    await key('f'); assert.equal(calls.length, 1); assert.match(screen.lastFrame()!, /Installation stopped/);
    await key('d'); assert.doesNotMatch(screen.lastFrame()!, /Controller already original|Audit:/);
    await key('\x1b'); assert.match(screen.lastFrame()!, /READY TO REVIEW/);
    await key('d'); assert.match(screen.lastFrame()!, /F · Finish a previous installation/);
    await key('f'); assert.match(screen.lastFrame()!, /FINISH YOUR INSTALLATION/);
    assert.match(screen.lastFrame()!, /light engine is retained/); await key('\r'); assert.equal(calls.length, 1);
    await key(' '); await key('\r'); assert.equal(calls.length, 2); assert.equal(calls[1]!.mode, 'finish');
    assert.notEqual(calls[0]!.audit, calls[1]!.audit);
    assert.match(calls[1]!.audit, /\.finish-[0-9a-f-]{36}\.jsonl$/);
    stop('stopped'); await pause(20);
  } finally {screen.unmount();}
});
