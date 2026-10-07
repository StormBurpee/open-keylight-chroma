import {spawn} from 'node:child_process';
import {resolve} from 'node:path';
import {dirname} from 'node:path';
import {existsSync} from 'node:fs';
import {StringDecoder} from 'node:string_decoder';
import {fileURLToPath} from 'node:url';
import {draftErrors, parseSummary, type Draft, type PlanSummary} from './model.js';
import {pythonArguments} from './python.js';

export function findRepository(start = dirname(fileURLToPath(import.meta.url))): string {
  let current = resolve(start);
  for (let level = 0; level < 6; level++) {
    if (existsSync(resolve(current, 'tools/stock_migration.py')) && (existsSync(resolve(current, 'firmware')) || existsSync(resolve(current, 'bundle.json')))) return current;
    current = dirname(current);
  }
  throw new Error('Run the installer from a complete Open Keylight checkout.');
}
export const repository = findRepository();
export type Run = (script: string, args: string[], signal?: AbortSignal) => Promise<unknown>;

/** A fixed Python entrypoint with an argv array. Never invoke a shell. */
export function pythonRunner(python = process.env['OKL_PYTHON'] ?? (process.platform === 'win32' ? 'python' : 'python3'), root = repository): Run {
  return (script, args, signal) => new Promise((accept, reject) => {
    if (!['prepare_migration.py', 'stock_migration.py', 'vendor_restore.py'].includes(script)) return reject(new Error('Unknown backend.'));
    if (script === 'stock_migration.py' && args[0] !== 'prepare') return reject(new Error('Live events backend has not been integrated.'));
    if (signal?.aborted) return reject(new Error('Cancelled before starting validation.'));
    const child = spawn(python, pythonArguments(root, script, args), {
      cwd: root, shell: false, windowsHide: true, stdio: ['ignore', 'pipe', 'pipe'],
    });
    let stdout = '', stderr = '', stopped: Error | undefined;
    const outputDecoder = new StringDecoder('utf8'), errorDecoder = new StringDecoder('utf8');
    const fail = (message: string) => { if (stopped) return; stopped = new Error(message); child.kill(); };
    const cancel = () => fail('Local preparation cancelled. Check the selected output path before trying again.');
    const timer = setTimeout(() => fail('Preparation exceeded its deadline. Check the output path before retrying.'), script === 'vendor_restore.py' ? 85000 : 60000);
    signal?.addEventListener('abort', cancel, {once: true});
    child.stdout.on('data', (chunk: Buffer) => {
      stdout += outputDecoder.write(chunk);
      if (Buffer.byteLength(stdout) > 262144) fail('Backend output exceeded its bound.');
    });
    child.stderr.on('data', (chunk: Buffer) => {
      stderr += errorDecoder.write(chunk);
      if (Buffer.byteLength(stderr) > 65536) fail('Backend diagnostics exceeded their bound.');
    });
    const cleanup = () => {clearTimeout(timer); signal?.removeEventListener('abort', cancel);};
    child.on('error', error => {cleanup(); reject(error);});
    child.on('close', code => {
      cleanup();
      if (stopped) return reject(stopped);
      stdout += outputDecoder.end(); stderr += errorDecoder.end();
      if (code !== 0) return reject(new Error(stderr.trim() || `Backend stopped (${code}).`));
      try {accept(JSON.parse(stdout));} catch {reject(new Error('Backend returned incomplete or invalid JSON.'));}
    });
  });
}

export async function acquireRestore(path: string, run: Run, signal?: AbortSignal): Promise<{path: string; provenance: string; cached: boolean}> {
  const v = await run('vendor_restore.py', ['--output', resolve(path)], signal) as Record<string, unknown>;
  if (!v || v.format !== 1 || v.profile !== 'keylight-chroma-1.0.13' || v.path !== resolve(path)
    || v.bytes !== 28672 || v.sha256 !== 'f46d19f50bba9fd97c6c3e207557b15470ad05d1877402a0cd15e0adc3a87a96'
    || v.version !== '1.3.0.0' || v.device_backup !== false || v.device_operations !== 0 || typeof v.cached !== 'boolean'
    || v.source_url !== 'https://mobileapp-assets.razerzone.com/iOS/Jade/T1/02.03.13.00.zip'
    || typeof v.provenance !== 'string' || !v.provenance || !/^[a-f0-9]{64}$/.test(String(v.archive_sha256))) {
    throw new Error('The acquisition helper returned an unexpected recovery artifact.');
  }
  return {path: v.path as string, provenance: v.provenance, cached: v.cached};
}

export async function validatePlan(path: string, run: Run, signal?: AbortSignal): Promise<PlanSummary> {
  return parseSummary(await run('stock_migration.py', ['prepare', '--manifest', resolve(path)], signal));
}
export async function createPlan(draft: Draft, run: Run, signal?: AbortSignal): Promise<PlanSummary> {
  const errors = draftErrors(draft);
  if (errors.length) throw new Error(errors[0]);
  const fields = {
    output: resolve(draft.output), 'target-ip': draft.ip, 'target-name': draft.name,
    'device-id': draft.deviceId, 'source-commit': draft.commit,
    identity: resolve(draft.identity), off1: resolve(draft.off1), low1: resolve(draft.low1),
    lighting: resolve(draft.lighting), esp: resolve(draft.esp), assets: resolve(draft.assets),
    restore: resolve(draft.restore), 'restore-version': draft.restoreVersion, 'restore-provenance': draft.provenance,
  };
  await run('prepare_migration.py', Object.entries(fields).flatMap(([k, v]) => [`--${k}`, v]), signal);
  return validatePlan(draft.output, run, signal);
}
