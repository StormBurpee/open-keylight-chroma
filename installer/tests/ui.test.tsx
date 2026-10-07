import React from 'react';
import {test} from 'node:test';
import assert from 'node:assert/strict';
import {setTimeout as pause} from 'node:timers/promises';
import {render} from 'ink-testing-library';
import {renderToString} from 'ink';
import {resolve} from 'node:path';
import {App} from '../src/App.js';
import {ProgressView, PlanCard} from '../src/components.js';
import {previewProgress, type Progress} from '../src/model.js';
import {summary} from './fixtures.js';

test('preview is visibly illustrative and completed ticks are evidence-based', () => {
  const output = renderToString(<ProgressView progress={previewProgress} preview />, {columns: 96});
  assert.match(output, /NO DEVICE ACTIVITY/); assert.match(output, /312 \/ 448/); assert.match(output, /Illustrative progress/);
  assert.doesNotMatch(output, /✓/);
  assert.doesNotMatch(renderToString(<PlanCard summary={summary} />), /A fixture only/);
  assert.match(renderToString(<PlanCard summary={summary} details />), /has not been contacted/);
});

test('finish progress labels only retained-controller checks, ESP upload and acceptance', () => {
  const output = renderToString(<ProgressView progress={{workflow: 'finish', stage: 0, state: 'stopped', label: 'Controller unavailable'}} auditPath="/private/new-audit.jsonl" />, {columns: 96});
  assert.match(output, /FINISH YOUR INSTALLATION/); assert.match(output, /STEP 1 \/ 3/);
  assert.match(output, /Installation stopped/); assert.doesNotMatch(output, /No firmware was uploaded/);
  assert.doesNotMatch(output, /Controller unavailable|new-audit|F ·|Finish a previous/);
  const details = renderToString(<ProgressView progress={{workflow: 'finish', stage: 0, state: 'stopped', label: 'Controller unavailable'}} auditPath="/private/new-audit.jsonl" details />, {columns: 96});
  assert.match(details, /Controller unavailable/); assert.match(details, /new-audit/);
  assert.doesNotMatch(output, /Check darkness|Check five channels|Install the light engine/);
});

test('no-upload copy requires explicit backend evidence, never stage zero or matching error text alone', () => {
  const generic: Progress = {workflow: 'install', stage: 0, state: 'stopped', label: 'stock_loader_entry_unconfirmed'};
  assert.doesNotMatch(renderToString(<ProgressView progress={generic} />), /No firmware was uploaded/);
  const proven: Progress = {...generic, failureCode: 'stock_loader_entry_unconfirmed'};
  assert.match(renderToString(<ProgressView progress={proven} />), /No firmware was uploaded/);
  assert.doesNotMatch(renderToString(<ProgressView progress={{...proven, stage: 2}} />), /No firmware was uploaded/);
});

test('stopped progress hides stale prompts, dashboard actions and credentials even with details open', () => {
  const progress: Progress = {stage: 6, state: 'stopped', label: 'Fixture late failure', completed: 100, total: 100,
    prompt: {id: 'old-prompt', question: 'Stale optical question'}, credentialPath: '/private/stale-token.json',
    dashboardUrl: 'http://192.168.1.25/', acceptanceState: 'complete',
    action: {url: 'http://192.168.1.25/', pairingOpen: false, remainingSeconds: 90, manifest: 'a'.repeat(64), controllerVersion: '0.1.1.0'}};
  for (const details of [false, true]) for (const running of [false, true]) {
    const output = renderToString(<ProgressView progress={progress} running={running} details={details} auditPath="/private/audit.jsonl" />, {columns: 96});
    assert.doesNotMatch(output, /Stale optical question|No \/ unsure|Enter confirm|new dashboard is ready|192\.168\.1\.25|3 seconds|Connection saved|stale-token|100%/);
    if (details) {assert.match(output, /Fixture late failure/); assert.match(output, /audit.jsonl/);}
    else assert.doesNotMatch(output, /Fixture late failure|audit.jsonl/);
    if (running) {
      assert.match(output, /Stopping safely/); assert.match(output, /Keep.*power/i); assert.match(output, /window open|terminal/i);
      assert.doesNotMatch(output, /Installation stopped|Esc back|Ctrl\+C exit/);
    } else assert.match(output, /Installation stopped/);
  }
});

test('quiet seconds and stopped counters never render a completed transfer bar', () => {
  for (const state of ['quiet', 'running'] as const) {
    const output = renderToString(<ProgressView progress={{stage: 1, state, label: 'Quiet period complete', completed: 3, total: 3, unit: 'seconds'}} details />, {columns: 96});
    assert.match(output, /Waiting for the controller|Waiting for the dashboard/);
    assert.doesNotMatch(output, /100%|3 \/ 3|━/);
  }
  const progress: Progress = {stage: 2, state: 'stopped', label: 'ProtocolError: raw failed readback', completed: 448, total: 448, unit: 'blocks', finishedStages: [0, 1]};
  const output = renderToString(<ProgressView progress={progress} auditPath="/private/failure.jsonl" />, {columns: 96});
  assert.match(output, /Installation stopped/); assert.match(output, /Open details before continuing/);
  assert.doesNotMatch(output, /100%|448|━|ProtocolError|failure.jsonl/);
  const detailed = renderToString(<ProgressView progress={progress} auditPath="/private/failure.jsonl" details />, {columns: 96});
  assert.match(detailed, /ProtocolError: raw failed readback/); assert.match(detailed, /failure.jsonl/);
  assert.doesNotMatch(detailed, /100%|448 \/ 448|━/);
});

test('finish selection needs explicit review and starts once with a fresh audit', async () => {
  const starts: {mode: string | undefined; audit: string}[] = [];
  const screen = render(<App initialPlan="fixture.json" run={async () => summary} start={(_p, audit, _s, update, mode) => {
    starts.push({mode, audit}); update({workflow: mode, stage: 0, state: 'stopped', label: 'Fixture stopped'});
    return {answer: () => {}, cancel: () => {}, done: Promise.resolve('stopped')};
  }} />);
  await pause(30); screen.stdin.write('\r'); await pause(60); screen.stdin.write('f'); await pause(40);
  assert.match(screen.lastFrame()!, /FINISH YOUR INSTALLATION/); assert.match(screen.lastFrame()!, /light engine is retained/);
  screen.stdin.write('\r'); await pause(30); assert.equal(starts.length, 0);
  screen.stdin.write(' '); await pause(30); screen.stdin.write('\r'); await pause(60);
  assert.equal(starts.length, 1); assert.equal(starts[0]!.mode, 'finish');
  screen.stdin.write('\x1b'); await pause(30); screen.stdin.write('f'); await pause(30);
  screen.stdin.write(' '); await pause(30); screen.stdin.write('\r'); await pause(60);
  assert.equal(starts.length, 2); assert.notEqual(starts[0]!.audit, starts[1]!.audit);
  screen.unmount();
});

test('closed pairing asks for one physical hold only before native acceptance starts', () => {
  const p = {stage: 6, state: 'running' as const, label: 'Completing setup', action: {url: 'http://192.168.1.25/', pairingOpen: false, remainingSeconds: 90, manifest: 'a'.repeat(64), controllerVersion: '0.1.1.0'}};
  assert.match(renderToString(<ProgressView progress={p} />), /3 seconds/);
  for (const acceptanceState of ['running', 'complete'] as const) assert.doesNotMatch(renderToString(<ProgressView progress={{...p, acceptanceState}} />), /3 seconds/);
});

test('step seven keeps its required button hold visible without opening details', () => {
  const output = renderToString(<ProgressView running progress={{stage: 6, state: 'running', label: 'Trial confirmation pending',
    unit: 'seconds', completed: 35, total: 175, remainingSeconds: 140,
    action: {url: 'http://192.168.1.25/', pairingOpen: false, remainingSeconds: 150, manifest: 'a'.repeat(64), controllerVersion: '0.1.1.0'}}} />);
  assert.match(output, /Hold.*button.*3 seconds/); assert.match(output, /140s left/);
  assert.doesNotMatch(output, /Finishing setup|100%|Trial confirmation pending/);
});
test('open plan validation cannot silently start installation; start requires explicit exclusive control', async () => {
  let validations = 0, starts = 0;
  const screen = render(<App initialPlan="fixture.json" run={async () => {validations++; return summary;}} start={() => {starts++; throw new Error('fixture start');}} />);
  await pause(50); screen.stdin.write('\r'); await pause(60);
  assert.equal(validations, 1); assert.equal(starts, 0); assert.match(screen.lastFrame()!, /READY TO REVIEW/);
  assert.doesNotMatch(screen.lastFrame()!, /A fixture only/);
  screen.stdin.write('d'); await pause(30); assert.match(screen.lastFrame()!, /A fixture only/);
  screen.stdin.write('\r'); await pause(40); screen.stdin.write('\r'); await pause(40);
  assert.equal(starts, 0); assert.match(screen.lastFrame()!, /☐.*I can watch the light/);
  screen.stdin.write(' '); await pause(40); screen.stdin.write('\r'); await pause(40);
  assert.equal(starts, 1); assert.match(screen.lastFrame()!, /Installation stopped/);
  // Technical details were explicitly enabled on the review page.
  assert.match(screen.lastFrame()!, /fixture start/); screen.unmount();
});

function recoveryResult(args: string[]) {
  return {format: 1, profile: 'keylight-chroma-1.0.13', path: args[1], bytes: 28672,
    sha256: 'f46d19f50bba9fd97c6c3e207557b15470ad05d1877402a0cd15e0adc3a87a96', version: '1.3.0.0',
    device_backup: false, device_operations: 0, cached: true,
    source_url: 'https://mobileapp-assets.razerzone.com/iOS/Jade/T1/02.03.13.00.zip',
    archive_sha256: 'b'.repeat(64), provenance: 'Reviewed fixture recovery provenance, no real bytes.'};
}

test('guided discovery prepares and validates automatically but only final checkbox plus Enter starts firmware', async () => {
  let queries = 0, bundles = 0, releaseRestore!: () => void, releaseValidation!: () => void, stop!: () => void;
  const backends: {script: string; args: string[]}[] = [], starts: {plan: string; audit: string; mode?: string}[] = [];
  const screen = render(<App run={async (script, args) => {
    backends.push({script, args});
    if (script === 'vendor_restore.py') {await new Promise<void>(done => {releaseRestore = done;}); return recoveryResult(args);}
    if (script === 'prepare_migration.py') return {};
    assert.equal(script, 'stock_migration.py'); assert.equal(args[0], 'prepare');
    await new Promise<void>(done => {releaseValidation = done;}); return summary;
  }} start={(plan, audit, selected, publish, mode) => {
    starts.push({plan, audit, mode}); assert.deepEqual(selected, summary);
    publish({stage: 0, state: 'running', label: 'Checking target'});
    return {answer: () => {}, cancel: () => {}, done: new Promise<'stopped'>(done => {stop = () => done('stopped');})};
  }}
    discover={async () => {queries++; return [{name: 'Studio light', ip: '192.168.1.25', deviceId: 'keylight-aabbcc', mac: 'AA:BB:CC:AA:BB:CC', installed: false}];}}
    bundleLoader={async () => {bundles++; return {path: '/release/bundle.json', version: '0.2.0-alpha.1', commit: 'a'.repeat(40), files: {identity: 'i', off1: 'o', low1: 'l', lighting: 'p', esp: 'e', assets: 'a'}};}} />);
  const key = async (value: string) => {screen.stdin.write(value); await pause(40);};
  try {
    await pause(30); assert.equal(queries, 0); assert.equal(backends.length, 0); assert.equal(starts.length, 0);
    assert.match(screen.lastFrame()!, /› Find my light/); assert.doesNotMatch(screen.lastFrame()!, /40-character/);
    await key('\r'); assert.equal(queries, 1); assert.equal(bundles, 0); assert.equal(backends.length, 0);
    assert.match(screen.lastFrame()!, /Studio light/);
    await key('\r'); assert.equal(bundles, 1); assert.deepEqual(backends.map(v => v.script), ['vendor_restore.py']);
    await key(' '); await key('\r'); assert.equal(starts.length, 0);
    releaseRestore(); await pause(60);
    assert.deepEqual(backends.map(v => v.script), ['vendor_restore.py', 'prepare_migration.py', 'stock_migration.py']);
    const args = backends[1]!.args, value = (name: string) => args[args.indexOf(`--${name}`) + 1];
    assert.equal(value('target-ip'), summary.target_ip); assert.equal(value('target-name'), summary.target_name);
    assert.equal(value('device-id'), summary.device_id); assert.equal(value('source-commit'), 'a'.repeat(40));
    assert.equal(value('identity'), resolve('i')); assert.equal(value('esp'), resolve('e'));
    assert.equal(value('restore'), backends[0]!.args[1]);
    assert.equal(backends[2]!.args[2], value('output'));
    await key(' '); await key('\r'); assert.equal(starts.length, 0);
    releaseValidation(); await pause(60);
    assert.match(screen.lastFrame()!, /READY TO INSTALL/); assert.match(screen.lastFrame()!, /Studio light/);
    assert.match(screen.lastFrame()!, /0.2.0-alpha.1/); assert.match(screen.lastFrame()!, /☐/);
    assert.doesNotMatch(screen.lastFrame()!, /READY TO REVIEW|Audit:|source commit/);
    await key('\r'); assert.equal(starts.length, 0);
    await key('d'); assert.match(screen.lastFrame()!, /Audit:/); assert.equal(starts.length, 0);
    await key('d'); assert.doesNotMatch(screen.lastFrame()!, /Audit:/);
    await key(' '); assert.match(screen.lastFrame()!, /☑/); assert.equal(starts.length, 0);
    await key('\r'); assert.equal(starts.length, 1); assert.equal(starts[0]!.mode, 'install');
    assert.equal(starts[0]!.plan, value('output')); assert.match(starts[0]!.audit, /\.install-[0-9a-f-]{36}\.jsonl$/);
    await key('\r'); await key(' '); assert.equal(starts.length, 1); assert.equal(backends.length, 3);
  } finally {stop?.(); screen.unmount();}
});

test('guided preparation failure never reaches confirmation, starts firmware or automatically retries', async () => {
  for (const failure of ['bundle', 'restore', 'plan', 'validation'] as const) {
    const calls: string[] = []; let starts = 0;
    const screen = render(<App run={async (script, args) => {
      calls.push(script);
      if (script === 'vendor_restore.py') {if (failure === 'restore') throw new Error('Fixture restore failure'); return recoveryResult(args);}
      if (script === 'prepare_migration.py') {if (failure === 'plan') throw new Error('Fixture plan failure'); return {};}
      assert.equal(script, 'stock_migration.py'); assert.equal(args[0], 'prepare');
      return {...summary, device_operations: 1}; // The real summary validator must refuse this.
    }} start={() => {starts++; throw new Error('Must not start');}}
      discover={async () => [{name: summary.target_name, ip: summary.target_ip, deviceId: summary.device_id, installed: false}]}
      bundleLoader={async () => {
        calls.push('bundle'); if (failure === 'bundle') throw new Error('Fixture bundle failure');
        return {path: '/release/bundle.json', version: summary.esp.version, commit: 'a'.repeat(40), files: {identity: 'i', off1: 'o', low1: 'l', lighting: 'p', esp: 'e', assets: 'a'}};
      }} />);
    try {
      await pause(30); screen.stdin.write('\r'); await pause(40); screen.stdin.write('\r'); await pause(70);
      const expected = ['bundle', 'vendor_restore.py', 'prepare_migration.py', 'stock_migration.py'];
      assert.deepEqual(calls, expected.slice(0, ['bundle', 'restore', 'plan', 'validation'].indexOf(failure) + 1));
      assert.equal(starts, 0); assert.doesNotMatch(screen.lastFrame()!, /READY TO INSTALL|☑|☐/);
      assert.match(screen.lastFrame()!, failure === 'validation' ? /invalid offline plan summary/ : new RegExp(`Fixture ${failure} failure`));
      await pause(60); assert.equal(starts, 0); assert.equal(calls.length, expected.indexOf(calls.at(-1)!) + 1);
    } finally {screen.unmount();}
  }
});

test('already installed light gets a dedicated verified dashboard view and back navigation without mutations', async () => {
  let calls = 0, reads = 0, discoveries = 0;
  const unexpected = async () => {calls++; throw new Error('Unexpected operation');};
  const screen = render(<App run={unexpected} bundleLoader={unexpected}
    nativeReader={async light => {reads++; return {deviceId: light.deviceId, firmware: '0.1.9-dev', trialPending: false};}}
    discover={async () => {discoveries++; return [{name: 'Desk', ip: '192.168.1.26', deviceId: 'keylight-ddeeff', installed: true}];}} />);
  await pause(30); screen.stdin.write('\r'); await pause(50); screen.stdin.write('\r'); await pause(50);
  assert.equal(calls, 0); assert.equal(reads, 1); assert.equal(discoveries, 1);
  assert.match(screen.lastFrame()!, /YOUR EXISTING INSTALLATION/); assert.match(screen.lastFrame()!, /http:\/\/192\.168\.1\.26\//);
  assert.match(screen.lastFrame()!, /0\.1\.9-dev/); assert.match(screen.lastFrame()!, /keylight-ddeeff/);
  assert.match(screen.lastFrame()!, /No reinstall needed/); assert.match(screen.lastFrame()!, /System/);
  screen.stdin.write('\r'); await pause(30); assert.match(screen.lastFrame()!, /FIND YOUR LIGHT/);
  assert.equal(discoveries, 1); assert.equal(reads, 1);
  screen.stdin.write('\x1b'); await pause(30); assert.match(screen.lastFrame()!, /Find my light/); screen.unmount();
});

test('empty discovery offers power/network guidance, retry and an explicit manual stock path', async () => {
  let queries = 0, other = 0;
  const unexpected = async () => {other++; throw new Error('Unexpected operation');};
  const screen = render(<App run={unexpected} bundleLoader={unexpected} nativeReader={unexpected}
    discover={async () => {queries++; return [];}} />);
  await pause(30); screen.stdin.write('\r'); await pause(40);
  assert.match(screen.lastFrame()!, /light powered/); assert.match(screen.lastFrame()!, /same local network/);
  assert.match(screen.lastFrame()!, /Check power/); assert.match(screen.lastFrame()!, /Enter stock address manually/);
  screen.stdin.write('\r'); await pause(40); assert.equal(queries, 2);
  screen.stdin.write('\t'); await pause(30); screen.stdin.write('\r'); await pause(30);
  assert.match(screen.lastFrame()!, /Light IP address/); assert.match(screen.lastFrame()!, /Exact stock name/);
  assert.equal(other, 0); screen.unmount();
});

test('failed native detail read keeps a useful unverified view without retry or preparation', async () => {
  let reads = 0;
  const unexpected = async () => {throw new Error('No preparation expected');};
  const screen = render(<App run={unexpected} bundleLoader={unexpected}
    nativeReader={async () => {reads++; throw new Error('Device details unavailable');}}
    discover={async () => [{name: 'Desk', ip: '192.168.1.26', deviceId: 'keylight-ddeeff', installed: true}]} />);
  await pause(30); screen.stdin.write('\r'); await pause(40); screen.stdin.write('\r'); await pause(40);
  assert.equal(reads, 1); assert.match(screen.lastFrame()!, /Version is not verified/);
  assert.match(screen.lastFrame()!, /Device details unavailable/); assert.match(screen.lastFrame()!, /http:\/\/192\.168\.1\.26\//);
  assert.match(screen.lastFrame()!, /Verify this light/);
  assert.doesNotMatch(screen.lastFrame()!, /identity checked|No reinstall needed|YOUR EXISTING INSTALLATION/); screen.unmount();
});

test('native pending trial directs to dashboard confirmation without confirming it', async () => {
  let reads = 0;
  const screen = render(<App run={async () => {throw new Error('No backend expected');}}
    nativeReader={async light => {reads++; return {deviceId: light.deviceId, firmware: '0.2.0-alpha.1', trialPending: true};}}
    discover={async () => [{name: 'Desk', ip: '192.168.1.26', deviceId: 'keylight-ddeeff', installed: true}]} />);
  await pause(30); screen.stdin.write('\r'); await pause(40); screen.stdin.write('\r'); await pause(40);
  assert.equal(reads, 1); assert.match(screen.lastFrame()!, /firmware trial is pending/);
  assert.match(screen.lastFrame()!, /confirm the trial/); screen.unmount();
});
test('offline cancellation sends AbortSignal and never retries preparation', async () => {
  let calls = 0, aborted = false;
  const screen = render(<App initialPlan="fixture.json" run={async (_s, _a, signal) => {calls++; return new Promise((_resolve, reject) => signal!.addEventListener('abort', () => {aborted = true; reject(new Error('Cancelled'));}));}} />);
  await pause(30); screen.stdin.write('\r'); await pause(30); screen.stdin.write('\x03'); await pause(30);
  assert.equal(calls, 1); assert.equal(aborted, true); assert.match(screen.lastFrame()!, /Cancelled/); screen.unmount();
});
test('prompt defaults to No and live Ctrl+C requests cooperative stop, not exit', async () => {
  let update: ((p: typeof previewProgress) => void) | undefined, cancelled = 0; const answers: string[] = [];
  const screen = render(<App initialPlan="fixture.json" run={async () => summary} start={(_p, _a, _s, u) => {update = u; return {cancel: () => {cancelled++;}, answer: (_id, answer) => answers.push(answer), done: new Promise(() => {})};}} />);
  await pause(30); screen.stdin.write('\r'); await pause(30); screen.stdin.write('\r'); await pause(30); screen.stdin.write(' '); await pause(30); screen.stdin.write('\r'); await pause(30);
  update!({stage: 2, state: 'prompt', label: 'Observation', prompt: {id: 'id1', question: 'Dark?'}}); await pause(30);
  screen.stdin.write('\r'); await pause(30); assert.deepEqual(answers, ['no']);
  screen.stdin.write('\x03'); await pause(30); assert.equal(cancelled, 1); assert.match(screen.lastFrame()!, /Studio light/);
  screen.unmount();
});

test('a stopping backend cannot receive an old optical answer or be replaced before its safe completion', async () => {
  let starts = 0, update!: (p: Progress) => void, finish!: () => void; const answers: string[] = [];
  const screen = render(<App initialPlan="fixture.json" run={async () => summary} start={(_p, _a, _s, publish) => {
    starts++; update = publish;
    return {cancel: () => {}, answer: (_id, answer) => answers.push(answer),
      done: new Promise<'stopped'>(done => {finish = () => done('stopped');})};
  }} />);
  const key = async (value: string) => {screen.stdin.write(value); await pause(40);};
  try {
    await pause(30); await key('\r'); await key('\r'); await key(' '); await key('\r');
    update({stage: 2, state: 'stopped', label: 'Hidden fixture failure', prompt: {id: 'old', question: 'Old optical prompt'}}); await pause(40);
    assert.match(screen.lastFrame()!, /Stopping safely/); assert.doesNotMatch(screen.lastFrame()!, /Old optical prompt|Hidden fixture failure/);
    await key('\r'); await key('f'); await key('\x1b');
    assert.deepEqual(answers, []); assert.equal(starts, 1); assert.match(screen.lastFrame()!, /Stopping safely/);
    await key('d'); assert.match(screen.lastFrame()!, /Hidden fixture failure/); assert.match(screen.lastFrame()!, /Audit:/);
    finish(); await pause(40); assert.match(screen.lastFrame()!, /Installation stopped/);
    await key('\x1b'); assert.match(screen.lastFrame()!, /READY TO REVIEW/); assert.equal(starts, 1);
  } finally {finish?.(); screen.unmount();}
});
