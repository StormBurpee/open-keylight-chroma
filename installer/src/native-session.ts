import {open} from 'node:fs/promises';
import {performance} from 'node:perf_hooks';
import {setTimeout as sleep} from 'node:timers/promises';
import {acceptNative, nativeHttp, type NativeAction} from './native.js';
import {privateDirectory, writeCredential} from './credentials.js';
import type {PlanSummary} from './model.js';

export type Accept = (summary: PlanSummary, action: NativeAction, audit: string, cancelled: () => boolean, status: (text: string) => void) => Promise<string>;
/** New exclusive audit and private token directory; neither is overwritten on another run. */
export const nativeAcceptance: Accept = async (summary, action, audit, cancelled, status) => {
  const file = await open(`${audit}.native.json`, 'wx', 0o600);
  try {
    const directory = await privateDirectory();
    const result = await acceptNative(summary, action, {
      request: nativeHttp(summary.target_ip), now: () => performance.now(), sleep, cancelled, status,
      cacheToken: token => writeCredential(directory, summary.device_id, summary.target_ip, token),
      save: async evidence => {await file.truncate(0); await file.write(JSON.stringify(evidence, null, 2) + '\n', 0, 'utf8'); await file.sync();},
    });
    return result.credential_path!;
  } finally {await file.close();}
};
