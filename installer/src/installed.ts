import {performance} from 'node:perf_hooks';
import {nativeHttp, type NativeRequest} from './native.js';
import {privateIPv4} from './model.js';
import type {Light} from './discovery.js';

export type InstalledDetails = {deviceId: string; firmware: string; trialPending: boolean};
export type ReadInstalled = (light: Light, signal: AbortSignal) => Promise<InstalledDetails>;

/** One bounded, unauthenticated device GET. Discovery never authorizes a write. */
export async function readInstalled(light: Light, signal: AbortSignal,
  request: NativeRequest = nativeHttp(light.ip), now = () => performance.now()): Promise<InstalledDetails> {
  signal.throwIfAborted();
  if (!light.installed || !privateIPv4(light.ip) || !/^keylight-[0-9a-f]{6}$/.test(light.deviceId)) throw new Error('Invalid discovered native light.');
  const deadline = now() + 3000;
  const {status, value} = await request('GET', 'device', undefined, undefined, deadline);
  signal.throwIfAborted();
  if (now() >= deadline || status !== 200 || !value || typeof value !== 'object' || Array.isArray(value)) throw new Error('Device details could not be verified. Open the dashboard or find the light again.');
  const v = value as Record<string, unknown>, network = v['network'];
  if (v['id'] !== light.deviceId || v['model'] !== 'Open Keylight Chroma' || v['api_version'] !== 1
    || typeof v['firmware'] !== 'string' || !/^[0-9A-Za-z][0-9A-Za-z.+-]{0,63}$/.test(v['firmware'])
    || typeof v['trial_pending'] !== 'boolean' || !network || typeof network !== 'object'
    || Array.isArray(network) || (network as Record<string, unknown>)['ip'] !== light.ip) {
    throw new Error('Device identity differs from discovery. Find the light again before using this address.');
  }
  return {deviceId: light.deviceId, firmware: v['firmware'], trialPending: v['trial_pending']};
}
