import {test} from 'node:test';
import assert from 'node:assert/strict';
import {mkdtemp, readFile, rm, stat} from 'node:fs/promises';
import {join} from 'node:path';
import {tmpdir} from 'node:os';
import {acceptNative, type AcceptanceEvidence, type NativeDependencies} from '../src/native.js';
import {privateDirectory, writeCredential} from '../src/credentials.js';
import {summary} from './fixtures.js';
const secret = 'c'.repeat(64);
class Harness {
  time = 50000; revision = 1; trial = true; pairing = true; boot = 0; reset = 1;
  desired: Record<string, any> = {power: false, mode: 'white', brightness: 50, temperature_k: 5000, rgb: {r: 255, g: 255, b: 255}, effect: 'none', transition_ms: 0, recording_lock: false};
  calls: string[] = []; evidence: AcceptanceEvidence[] = []; notices: string[] = []; credentials: string[] = [];
  fault = ''; cacheFail = false; slowSave = false; concurrent = false; cancel = false;
  state(): Record<string, any> {return {revision: this.revision, desired: {...this.desired}, operation: {status: 'idle', error: null}, reported: {...this.desired, valid: true, confirmed_fields: ['power', 'mode', 'brightness', 'temperature_k', 'effect']}};}
  deps(): NativeDependencies {return {
    now: () => this.time, sleep: async ms => {this.time += ms;}, cancelled: () => this.cancel,
    status: text => this.notices.push(text),
    cacheToken: async token => {if (this.cacheFail) throw new Error('Private cache failed'); this.credentials.push(token); return '/private/credential.json';},
    save: async e => {this.evidence.push(structuredClone(e)); if (this.slowSave && e.attempts.includes('confirm_once')) this.time += 200000;},
    request: async (method, path, body, token, deadline) => {
      assert.ok(this.time < deadline); this.calls.push(`${method} ${path}`); this.time += 10;
      if (method === 'GET' && path === 'device') {
        if (this.fault === 'reboot' && this.revision > 1) {this.boot = 40000; this.reset = 3;}
        return {status: 200, value: {id: this.fault === 'identity' ? 'keylight-ffffff' : summary.device_id, firmware: summary.esp.version, firmware_elf_sha256: summary.esp.elf_sha256, api_version: 1,
          uptime_ms: this.time - this.boot, reset_reason: this.reset, trial_pending: this.trial, pairing_open: this.pairing,
          network: {ip: summary.target_ip, connected: true}, controller: {backend: 'original', version: '0.1.1.0', part_id: 0xbc40, connected: true, ready: this.fault !== 'controller', status: 'ready', trial_confirmed: true, last_health_ms: this.time - this.boot - 100}}};
      }
      if (method === 'GET' && path === 'state') {
        if (this.concurrent && this.revision > 1) {this.revision++; this.desired.brightness = 30;}
        const s = this.state(); if (this.fault === 'readback' && this.revision > 1) s.reported.power = false;
        return {status: 200, value: s};
      }
      if (method === 'GET' && path === 'controller/update') return {status: 200, value: {state: 'idle', job_id: null, recovery_required: this.fault === 'journal'}};
      if (method === 'POST' && path === 'pair') {this.pairing = false; if (this.fault === 'pair') throw new Error('Pair response lost'); return {status: 201, value: {token: secret}};}
      assert.equal(token, secret);
      if (method === 'GET' && path === 'clients') return {status: 200, value: {clients: [{id: 'one', label: 'Open Keylight installer'}]}};
      if (method === 'PATCH' && path === 'state') {
        const b = body as Record<string, any>; assert.equal(b.expected_revision, this.revision);
        const {expected_revision: _r, ...delta} = b; this.desired = {...this.desired, ...delta}; this.revision++;
        if ((this.fault === 'white' && this.revision === 2) || (this.fault === 'off' && this.revision === 3)) throw new Error('Mutation response lost');
        return {status: 202, value: this.state()};
      }
      if (method === 'POST' && path === 'confirm') {this.trial = false; if (this.fault === 'confirm') throw new Error('Confirm response lost'); return {status: 200, value: {confirmed: true}};}
      throw new Error('Unexpected route');
    },
  };}
  run(remainingMs = 120000) {return acceptNative(summary, {manifest: summary.manifest_sha256, controllerVersion: '0.1.1.0', remainingMs}, this.deps());}
}
test('native acceptance pairs once, proves low white then Off, confirms exact app once without logging token', async () => {
  const h = new Harness(), result = await h.run();
  assert.equal(result.accepted, true); assert.equal(result.optical_verified, false); assert.equal(h.desired.power, false); assert.equal(h.trial, false);
  assert.deepEqual(result.attempts, ['pair_once', 'white_5_percent_once', 'off_once', 'confirm_once']);
  assert.equal(h.calls.filter(c => c === 'POST confirm').length, 1); assert.deepEqual(h.credentials, [secret]);
  assert.equal(JSON.stringify(h.evidence).includes(secret), false); assert.equal(h.notices.join(' ').includes(secret), false);
});
test('identity, controller, journal, closed pairing and low time stop before any mutation', async () => {
  for (const fault of ['identity', 'controller', 'journal', 'closed']) {
    const h = new Harness(); h.fault = fault; if (fault === 'closed') h.pairing = false;
    await assert.rejects(h.run()); assert.equal(h.calls.some(c => c.startsWith('POST') || c.startsWith('PATCH')), false);
  }
  const h = new Harness(); await assert.rejects(h.run(29999)); assert.deepEqual(h.calls, []);
});
test('lost pair or credential persistence failure never retries or controls light', async () => {
  for (const cacheFail of [false, true]) {const h = new Harness(); h.cacheFail = cacheFail; h.fault = cacheFail ? '' : 'pair';
    await assert.rejects(h.run()); assert.equal(h.calls.filter(c => c === 'POST pair').length, 1); assert.equal(h.calls.some(c => c.startsWith('PATCH')), false);
  }
});
test('lost white response permits one exact owned cleanup Off; lost Off never repeated', async () => {
  const white = new Harness(); white.fault = 'white'; await assert.rejects(white.run());
  assert.equal(white.calls.filter(c => c === 'PATCH state').length, 2); assert.equal(white.desired.power, false); assert.equal(white.calls.includes('POST confirm'), false);
  const off = new Harness(); off.fault = 'off'; await assert.rejects(off.run());
  assert.equal(off.calls.filter(c => c === 'PATCH state').length, 2); assert.equal(off.calls.includes('POST confirm'), false);
});
test('reboot or concurrent revision refuses cleanup and confirmation', async () => {
  for (const failure of ['reboot', 'concurrent']) {const h = new Harness(); h.fault = failure; h.concurrent = failure === 'concurrent';
    await assert.rejects(h.run()); assert.equal(h.calls.filter(c => c === 'PATCH state').length, 1); assert.equal(h.calls.includes('POST confirm'), false);
  }
});
test('readback mismatch cannot qualify; ambiguous confirmation is not repeated', async () => {
  const failed = new Harness(); failed.fault = 'readback'; await assert.rejects(failed.run()); assert.equal(failed.calls.includes('POST confirm'), false);
  const uncertain = new Harness(); uncertain.fault = 'confirm'; await assert.rejects(uncertain.run()); assert.equal(uncertain.calls.filter(c => c === 'POST confirm').length, 1); assert.equal(uncertain.evidence.at(-1)?.accepted, false);
});
test('slow final audit and cancellation cannot dispatch confirmation after budget', async () => {
  const h = new Harness(); h.slowSave = true; await assert.rejects(h.run()); assert.equal(h.calls.includes('POST confirm'), false);
  const cancelled = new Harness(); cancelled.cancel = true; await assert.rejects(cancelled.run()); assert.deepEqual(cancelled.calls, []);
});
test('real credential directory restricts access and preserves exclusive existing file', async () => {
  const root = await mkdtemp(join(tmpdir(), 'okl private '));
  try {
    const directory = await privateDirectory(root), path = await writeCredential(directory, summary.device_id, summary.target_ip, secret);
    assert.equal(JSON.parse(await readFile(path, 'utf8')).token, secret);
    if (process.platform !== 'win32') {assert.equal((await stat(directory)).mode & 0o777, 0o700); assert.equal((await stat(path)).mode & 0o777, 0o600);}
    await assert.rejects(writeCredential(directory, summary.device_id, summary.target_ip, 'd'.repeat(64)), /EEXIST/);
  } finally {await rm(root, {recursive: true, force: true});}
});
