import {open} from 'node:fs/promises';
import {performance} from 'node:perf_hooks';
import {setTimeout as sleep} from 'node:timers/promises';
import {acceptNative, nativeHttp, type AcceptanceEvidence, type NativeAction} from './native.js';
import {privateDirectory, writeCredential} from './credentials.js';
import type {PlanSummary} from './model.js';

export type Accept = (summary: PlanSummary, action: NativeAction, audit: string, cancelled: () => boolean, status: (text: string) => void) => Promise<string>;
/** New exclusive audit and private token directory; neither is overwritten on another run. */
export const nativeAcceptance: Accept = async (summary, action, audit, cancelled, status) => {
  const started = performance.now();
  const file = await open(`${audit}.native.json`, 'wx', 0o600);
  let recorded = false;
  const save = async (evidence: AcceptanceEvidence) => {
    await file.truncate(0); await file.write(JSON.stringify(evidence, null, 2) + '\n', 0, 'utf8'); await file.sync(); recorded = true;
  };
  try {
    status('Preparing private access on this computer…');
    const directory = await privateDirectory();
    if (cancelled()) throw new Error('Native acceptance cancelled before pairing.');
    const remainingMs = Math.max(0, Math.floor(action.remainingMs - (performance.now() - started)));
    const result = await acceptNative(summary, {...action, remainingMs}, {
      request: nativeHttp(summary.target_ip), now: () => performance.now(), sleep, cancelled, status,
      cacheToken: token => writeCredential(directory, summary.device_id, summary.target_ip, token),
      save,
    });
    return result.credential_path!;
  } catch (error) {
    if (!recorded) await save({format: 1, manifest_sha256: summary.manifest_sha256, device_id: summary.device_id,
      attempts: [], checks: [], accepted: false, optical_verified: false,
      error: error instanceof Error ? error.message : 'Local access preparation failed.'});
    throw error;
  } finally {await file.close();}
};
