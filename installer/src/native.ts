import {request as httpRequest} from 'node:http';
import {isDeepStrictEqual} from 'node:util';
import {performance} from 'node:perf_hooks';
import type {PlanSummary} from './model.js';

type ObjectValue = Record<string, any>;
export type NativeAction = {manifest: string; controllerVersion: string; remainingMs: number};
export type NativeRequest = (method: 'GET' | 'POST' | 'PATCH', path: string, body: unknown, token: string | undefined, deadline: number) => Promise<{status: number; value: unknown}>;
export type AcceptanceEvidence = {format: 1; manifest_sha256: string; device_id: string; attempts: string[]; checks: string[]; accepted: boolean; optical_verified: false; error?: string; credential_path?: string};
export type NativeDependencies = {
  request: NativeRequest; now(): number; sleep(ms: number): Promise<void>;
  save(evidence: AcceptanceEvidence): Promise<void>;
  cacheToken(token: string): Promise<string>;
  cancelled(): boolean;
  status(message: string): void;
};
const object = (v: unknown): v is ObjectValue => !!v && typeof v === 'object' && !Array.isArray(v);
const uint = (v: unknown): v is number => Number.isSafeInteger(v) && Number(v) >= 0 && Number(v) <= 4294967295;
function need(condition: unknown, message: string): asserts condition {if (!condition) throw new Error(message);}

/** Exact IP, fixed routes, no redirects/proxies, bounded JSON and absolute request deadline. */
export function nativeHttp(ip: string, now = () => performance.now()): NativeRequest {
  need(/^(?:\d{1,3}\.){3}\d{1,3}$/.test(ip), 'Invalid native target.');
  return (method, path, body, token, deadline) => new Promise((accept, reject) => {
    const allowed = method === 'GET' ? ['device', 'state', 'controller/update', 'clients'] : method === 'PATCH' ? ['state'] : ['pair', 'confirm'];
    if (!allowed.includes(path) || deadline <= now()) return reject(new Error('Native request is outside its allowed route or deadline.'));
    const data = body === undefined ? undefined : Buffer.from(JSON.stringify(body));
    const headers: Record<string, string | number> = {Connection: 'close', 'Cache-Control': 'no-cache', 'Accept-Encoding': 'identity'};
    if (data) {headers['Content-Type'] = 'application/json'; headers['Content-Length'] = data.length;}
    if (token) headers.Authorization = `Bearer ${token}`;
    if (method === 'PATCH') headers['X-Keylight-Actor'] = 'automation';
    const request = httpRequest({host: ip, port: 80, path: `/api/v1/${path}`, method, headers, agent: false}, response => {
      const chunks: Buffer[] = []; let bytes = 0;
      if (response.headers['content-encoding'] && response.headers['content-encoding'] !== 'identity') {request.destroy(new Error('Unexpected native response encoding.')); return;}
      response.on('data', (chunk: Buffer) => {bytes += chunk.length; if (bytes > 32768) request.destroy(new Error('Native response exceeded its bound.')); else chunks.push(chunk);});
      response.on('end', () => {
        clearTimeout(timer);
        if (now() >= deadline) return reject(new Error('Native response arrived after its deadline.'));
        try {accept({status: response.statusCode ?? 0, value: JSON.parse(Buffer.concat(chunks).toString('utf8'))});}
        catch {reject(new Error('Native response is not valid JSON.'));}
      });
      response.on('error', () => {clearTimeout(timer); reject(new Error('Native response was interrupted; mutation was not retried.'));});
    });
    const timer = setTimeout(() => request.destroy(new Error('Native request deadline expired; mutation was not retried.')), Math.max(1, Math.min(3000, deadline - now())));
    request.on('error', () => {clearTimeout(timer); reject(new Error('Native request failed or became uncertain; no mutation retry.'));});
    if (data) request.write(data);
    request.end();
  });
}

/** One explicit first-install acceptance. The Python observer independently sees its result. */
export async function acceptNative(summary: PlanSummary, action: NativeAction, d: NativeDependencies): Promise<AcceptanceEvidence> {
  need(action.manifest === summary.manifest_sha256 && /^(?:\d{1,3}\.){3}\d{1,3}$/.test(action.controllerVersion)
    && Number.isSafeInteger(action.remainingMs) && action.remainingMs >= 30000 && action.remainingMs <= 180000, 'Not enough verified trial time or action identity differs.');
  let deadline = d.now() + action.remainingMs - 1000;
  let bootLow = -Infinity, bootHigh = Infinity, reset: number | undefined, lastUptime = -1;
  let token: string | undefined, owned: {revision: number; desired: ObjectValue} | undefined, offAttempted = false, cleanup = false;
  const evidence: AcceptanceEvidence = {format: 1, manifest_sha256: summary.manifest_sha256, device_id: summary.device_id, attempts: [], checks: [], accepted: false, optical_verified: false};
  const persist = () => d.save(structuredClone(evidence));
  const guard = () => {need(cleanup || !d.cancelled(), 'Native acceptance cancelled.'); need(d.now() < deadline, 'Native acceptance trial deadline expired.');};
  const request = async (method: 'GET' | 'POST' | 'PATCH', path: string, body?: unknown) => {
    guard(); const result = await d.request(method, path, body, token, Math.min(deadline, d.now() + 3000)); guard(); return result;
  };
  const get = async (path: string) => {const r = await request('GET', path); need(r.status === 200 && object(r.value), `Native ${path} read failed.`); return r.value;};
  const device = async (trial: boolean) => {
    const started = d.now(), v = await get('device'), ended = d.now(), c = v.controller;
    need(v.id === summary.device_id && v.api_version === 1 && v.firmware === summary.esp.version && v.firmware_elf_sha256 === summary.esp.elf_sha256
      && object(v.network) && v.network.ip === summary.target_ip && v.network.connected === true && v.trial_pending === trial && uint(v.uptime_ms) && uint(v.reset_reason), 'Native identity or trial state changed.');
    need(object(c) && c.backend === 'original' && c.version === action.controllerVersion && c.part_id === 0xbc40 && c.connected === true
      && c.ready === true && c.status === 'ready' && c.trial_confirmed === true && uint(c.last_health_ms)
      && v.uptime_ms >= c.last_health_ms && v.uptime_ms - c.last_health_ms <= 5000, 'Original controller is not freshly ready.');
    const low = Math.max(bootLow, started - v.uptime_ms - 50), high = Math.min(bootHigh, ended - v.uptime_ms + 50);
    need(low <= high && v.uptime_ms >= lastUptime && (reset === undefined || reset === v.reset_reason), 'ESP restarted during acceptance.');
    bootLow = low; bootHigh = high; reset = v.reset_reason; lastUptime = v.uptime_ms;
    deadline = Math.min(deadline, bootLow + 175000); guard(); return v;
  };
  const state = async () => {const s = await get('state'); need(uint(s.revision) && object(s.desired) && object(s.reported) && object(s.operation) && s.operation.status !== 'error', 'Native state is invalid or failed.'); return s;};
  const matches = (s: ObjectValue, wanted: {revision: number; desired: ObjectValue}) => s.revision === wanted.revision && isDeepStrictEqual(s.desired, wanted.desired);
  const settled = (s: ObjectValue, wanted: {revision: number; desired: ObjectValue}, fields: string[]) => {
    need(matches(s, wanted), 'Another control changed the light.');
    if (s.operation.status !== 'idle') return false;
    need(s.reported.valid === true && Array.isArray(s.reported.confirmed_fields), 'Controller readback is not valid.');
    for (const field of fields) need(s.reported.confirmed_fields.includes(field) && isDeepStrictEqual(s.reported[field], wanted.desired[field]), `Controller ${field} readback differs.`);
    return true;
  };
  const mutate = async (label: string, method: 'POST' | 'PATCH', path: string, body?: unknown) => {
    need(!evidence.attempts.includes(label), 'Mutation was already attempted.');
    evidence.attempts.push(label); await persist(); guard(); return request(method, path, body);
  };
  const patch = async (label: string, before: {revision: number; desired: ObjectValue}, delta: ObjectValue, fields: string[]) => {
    await device(true); const fresh = await state(); need(matches(fresh, before) && fresh.operation.status === 'idle', 'State changed before native control check.');
    need(before.revision < 4294967295, 'Revision cannot advance safely.');
    owned = {revision: before.revision + 1, desired: {...before.desired, ...delta}};
    if (delta.power === false) offAttempted = true;
    const response = await mutate(label, 'PATCH', 'state', {...delta, expected_revision: before.revision});
    need(response.status === 202 && object(response.value) && matches(response.value, owned), 'Native control acceptance differs or is uncertain.');
    const end = Math.min(deadline, d.now() + 6000);
    while (d.now() < end) {
      await d.sleep(150); await device(true); const current = await state();
      if (settled(current, owned, fields)) {evidence.checks.push(label); await persist(); return owned;}
    }
    throw new Error('Controller readback did not settle; command was not retried.');
  };
  try {
    await persist(); d.status('Verifying the new application and controller…');
    const first = await device(true), baseline = await state(), job = await get('controller/update');
    need(first.pairing_open === true && baseline.desired.power === false && baseline.desired.recording_lock === false
      && baseline.operation.status === 'idle' && baseline.reported.valid === true && baseline.reported.power === false
      && baseline.reported.confirmed_fields?.includes('power') && baseline.revision <= 4294967292, 'Initial pairing window or verified Off is absent.');
    need(job.state === 'idle' && job.job_id === null && job.recovery_required === false, 'Controller update journal is not clear.');
    evidence.checks.push('identity_and_initial_off'); await persist();
    const paired = await mutate('pair_once', 'POST', 'pair', {label: 'Open Keylight installer'});
    need(paired.status === 201 && object(paired.value) && /^[a-f0-9]{64}$/.test(paired.value.token), 'Pairing did not return a valid token; do not repeat automatically.');
    token = paired.value.token;
    evidence.credential_path = await d.cacheToken(token!); await persist();
    const clients = await get('clients'); need(Array.isArray(clients.clients) && clients.clients.some((c: unknown) => object(c) && c.label === 'Open Keylight installer'), 'Authenticated client readback failed.');
    d.status('Checking a brief 5% white output, then returning to Off…');
    const white = await patch('white_5_percent_once', {revision: baseline.revision, desired: baseline.desired}, {power: true, mode: 'white', brightness: 5, temperature_k: 3000, effect: 'none', transition_ms: 0}, ['power', 'mode', 'brightness', 'temperature_k', 'effect']);
    await d.sleep(300); guard();
    const off = await patch('off_once', white, {power: false, transition_ms: 0}, ['power']);
    await device(true); need(settled(await state(), off, ['power']), 'Off changed before confirmation.');
    d.status('Readback checks passed. Confirming this exact application once…');
    const confirmed = await mutate('confirm_once', 'POST', 'confirm');
    need(confirmed.status === 200 && object(confirmed.value) && confirmed.value.confirmed === true, 'Confirmation response is uncertain; it was not retried.');
    await device(false); need(settled(await state(), off, ['power']), 'State changed across confirmation.');
    evidence.checks.push('confirmed_and_off'); evidence.accepted = true; await persist(); return evidence;
  } catch (error) {
    // At most one cleanup Off, only for our exact accepted/planned white intent on the same boot.
    // It never confirms an uncertain install or overwrites a newer user command.
    if (token && owned && !offAttempted && d.now() < deadline) {
      try {
        cleanup = true;
        await device(true); const current = await state();
        if (matches(current, owned)) {
          offAttempted = true;
          const response = await mutate('recovery_off_once', 'PATCH', 'state', {power: false, transition_ms: 0, expected_revision: owned.revision});
          evidence.checks.push(response.status === 202 ? 'recovery_off_accepted_unverified' : 'recovery_off_rejected');
        }
      } catch { /* Original failure remains authoritative; never retry cleanup. */ }
    }
    evidence.error = error instanceof Error ? error.message : 'Native acceptance failed.';
    await persist(); throw new Error(evidence.error);
  }
}
