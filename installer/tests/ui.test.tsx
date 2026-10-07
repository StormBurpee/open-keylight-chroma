import React from 'react';
import {test} from 'node:test';
import assert from 'node:assert/strict';
import {setTimeout as pause} from 'node:timers/promises';
import {render} from 'ink-testing-library';
import {renderToString} from 'ink';
import {App} from '../src/App.js';
import {ProgressView, PlanCard} from '../src/components.js';
import {previewProgress} from '../src/model.js';
import {summary} from './fixtures.js';

test('preview is visibly illustrative and completed ticks are evidence-based', () => {
  const output = renderToString(<ProgressView progress={previewProgress} preview />, {columns: 96});
  assert.match(output, /NO DEVICE ACTIVITY/); assert.match(output, /312 \/ 448/); assert.match(output, /Illustrative progress/);
  assert.doesNotMatch(output, /✓/);
  assert.match(renderToString(<PlanCard summary={summary} />), /has not been contacted/);
});

test('closed pairing asks for one physical hold only before native acceptance starts', () => {
  const p = {stage: 6, state: 'running' as const, label: 'Completing setup', action: {url: 'http://192.168.1.25/', pairingOpen: false, remainingSeconds: 90, manifest: 'a'.repeat(64), controllerVersion: '0.1.1.0'}};
  assert.match(renderToString(<ProgressView progress={p} />), /3 seconds/);
  for (const acceptanceState of ['running', 'complete'] as const) assert.doesNotMatch(renderToString(<ProgressView progress={{...p, acceptanceState}} />), /3 seconds/);
});
test('open plan validation cannot silently start installation; start requires explicit exclusive control', async () => {
  let validations = 0, starts = 0;
  const screen = render(<App initialPlan="fixture.json" run={async () => {validations++; return summary;}} start={() => {starts++; throw new Error('fixture start');}} />);
  await pause(50); screen.stdin.write('\r'); await pause(60);
  assert.equal(validations, 1); assert.equal(starts, 0); assert.match(screen.lastFrame()!, /READY TO REVIEW/);
  assert.doesNotMatch(screen.lastFrame()!, /A fixture only/);
  screen.stdin.write('d'); await pause(30); assert.match(screen.lastFrame()!, /A fixture only/);
  screen.stdin.write('\r'); await pause(40); screen.stdin.write('\r'); await pause(40);
  assert.equal(starts, 0); assert.match(screen.lastFrame()!, /exclusive control/);
  screen.stdin.write(' '); await pause(40); screen.stdin.write('\r'); await pause(40);
  assert.equal(starts, 1); assert.match(screen.lastFrame()!, /fixture start/); screen.unmount();
});

test('default flow discovers only after selection, derives target and loads the bundled release', async () => {
  let queries = 0, bundles = 0, backends = 0;
  const screen = render(<App run={async () => {backends++; throw new Error('No preparation yet');}}
    discover={async () => {queries++; return [{name: 'Studio light', ip: '192.168.1.25', deviceId: 'keylight-aabbcc', mac: 'AA:BB:CC:AA:BB:CC', installed: false}];}}
    bundleLoader={async () => {bundles++; return {path: '/release/bundle.json', version: '0.2.0-alpha.1', commit: 'a'.repeat(40), files: {identity: 'i', off1: 'o', low1: 'l', lighting: 'p', esp: 'e', assets: 'a'}};}} />);
  await pause(30); assert.equal(queries, 0); assert.equal(backends, 0);
  assert.match(screen.lastFrame()!, /› Find my light/); assert.doesNotMatch(screen.lastFrame()!, /40-character/);
  screen.stdin.write('\r'); await pause(50); assert.equal(queries, 1); assert.match(screen.lastFrame()!, /Studio light/);
  screen.stdin.write('\r'); await pause(50); assert.equal(bundles, 1); assert.equal(backends, 0);
  assert.match(screen.lastFrame()!, /0.2.0-alpha.1/); assert.match(screen.lastFrame()!, /Selected for Studio light/);
  screen.unmount();
});

test('already installed discovery result directs to dashboard without selecting firmware or starting backend', async () => {
  let calls = 0;
  const unexpected = async () => {calls++; throw new Error('Unexpected operation');};
  const screen = render(<App run={unexpected} bundleLoader={unexpected} discover={async () => [{name: 'Desk', ip: '192.168.1.26', deviceId: 'keylight-ddeeff', installed: true}]} />);
  await pause(30); screen.stdin.write('\r'); await pause(50); screen.stdin.write('\r'); await pause(50);
  assert.equal(calls, 0); assert.match(screen.lastFrame()!, /Already running Open Keylight/); screen.unmount();
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
  screen.stdin.write('\x03'); await pause(30); assert.equal(cancelled, 1); assert.match(screen.lastFrame()!, /GUIDED INSTALLATION/);
  screen.unmount();
});
