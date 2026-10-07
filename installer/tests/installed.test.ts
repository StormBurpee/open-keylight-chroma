import {test} from 'node:test';
import assert from 'node:assert/strict';
import {readInstalled} from '../src/installed.js';
import type {NativeRequest} from '../src/native.js';

const light = {name: 'Desk', ip: '192.168.1.26', deviceId: 'keylight-ddeeff', installed: true};
const device = {id: light.deviceId, model: 'Open Keylight Chroma', api_version: 1, firmware: '0.2.0-alpha.1', trial_pending: false, network: {ip: light.ip}};

test('installed detail check makes one fixed unauthenticated GET with a three-second deadline', async () => {
  const calls: Parameters<NativeRequest>[] = [];
  const info = await readInstalled(light, new AbortController().signal, async (...args) => {calls.push(args); return {status: 200, value: device};}, () => 500);
  assert.deepEqual(calls, [['GET', 'device', undefined, undefined, 3500]]);
  assert.deepEqual(info, {deviceId: light.deviceId, firmware: '0.2.0-alpha.1', trialPending: false});
});

test('installed details reject mismatched identity, address, schema, unsafe version and status without retry', async () => {
  for (const change of [{id: 'keylight-aabbcc'}, {model: 'unrelated'}, {api_version: 2}, {network: {ip: '192.168.1.27'}}, {firmware: '\x1b[31mred'}, {firmware: 'x'.repeat(65)}, {trial_pending: 1}]) {
    let calls = 0;
    await assert.rejects(readInstalled(light, new AbortController().signal, async () => {calls++; return {status: 200, value: {...device, ...change}};}));
    assert.equal(calls, 1);
  }
  for (const status of [301, 404, 500]) await assert.rejects(readInstalled(light, new AbortController().signal, async () => ({status, value: device})));
});

test('invalid or pre-cancelled targets make no request; late or cancelled results are not displayed as verified', async () => {
  let calls = 0;
  const request: NativeRequest = async () => {calls++; return {status: 200, value: device};};
  for (const target of [{...light, ip: '8.8.8.8'}, {...light, installed: false}, {...light, deviceId: 'wrong'}]) await assert.rejects(readInstalled(target, new AbortController().signal, request));
  await assert.rejects(readInstalled(light, AbortSignal.abort(), request)); assert.equal(calls, 0);
  let now = 0;
  await assert.rejects(readInstalled(light, new AbortController().signal, async () => {now = 3000; return {status: 200, value: device};}, () => now));
  const abort = new AbortController();
  await assert.rejects(readInstalled(light, abort.signal, async () => {abort.abort(); return {status: 200, value: device};}));
});

test('pending trial is metadata only and transport failure is never retried', async () => {
  const info = await readInstalled(light, new AbortController().signal, async () => ({status: 200, value: {...device, trial_pending: true}}));
  assert.equal(info.trialPending, true);
  let calls = 0;
  await assert.rejects(readInstalled(light, new AbortController().signal, async () => {calls++; throw new Error('Offline');}), /Offline/);
  assert.equal(calls, 1);
});
