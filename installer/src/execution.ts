import {spawn, type ChildProcessWithoutNullStreams} from 'node:child_process';
import {resolve} from 'node:path';
import {EventLines, EventState} from './events.js';
import {repository} from './backend.js';
import type {PlanSummary, Progress} from './model.js';
import type {Accept} from './native-session.js';
import {pythonArguments} from './python.js';

export type Execution = {answer(id: string, answer: 'yes' | 'no'): void; cancel(): void; done: Promise<'installed' | 'stopped'>};
export type Launch = (args: string[]) => ChildProcessWithoutNullStreams;
export type Start = (plan: string, audit: string, summary: PlanSummary, update: (p: Progress) => void) => Execution;

export function installationRunner(python = process.env['OKL_PYTHON'] ?? (process.platform === 'win32' ? 'python' : 'python3'), root = repository, launch?: Launch, acceptNative?: Accept): Start {
  return (plan, audit, summary, update) => {
    if (!audit.trim() || resolve(audit) === resolve(plan) || !/^[a-f0-9]{64}$/.test(summary.manifest_sha256)) throw new Error('Choose a new audit path separate from the verified plan.');
    const args = pythonArguments(root, 'stock_migration.py', ['install', '--manifest', resolve(plan), '--audit', resolve(audit),
      '--execute', '--exclusive-control', '--expected-manifest-sha256', summary.manifest_sha256, '--events-jsonl']);
    const child = launch ? launch(args) : spawn(python, args, {cwd: root, shell: false, windowsHide: true, stdio: ['pipe', 'pipe', 'pipe']});
    const state = new EventState({ip: summary.target_ip, deviceId: summary.device_id, version: summary.esp.version, elf: summary.esp.elf_sha256, manifest: summary.manifest_sha256, controllerVersion: summary.controller_version});
    let fault: Error | undefined, stderrBytes = 0;
    let nativeStarted = false, nativeDone: Promise<void> | undefined;
    const publish = () => update({...state.progress});
    const write = (line: string | undefined) => {if (line && child.stdin.writable) child.stdin.write(line);};
    const cancel = () => {write(state.cancel()); publish();};
    const fail = (error: unknown) => {
      if (fault) return;
      fault = error instanceof Error ? error : new Error('Installer event stream failed.');
      // Never kill a flashing child. Ask it to stop only after its safe stage.
      cancel(); state.progress = {...state.progress, state: 'stopped', label: `${fault.message} Waiting for the current safe stage; keep this window open.`}; publish();
    };
    child.stdin.on('error', fail);
    const lines = new EventLines(value => {if (!fault) {
      state.accept(value); publish();
      if (state.progress.action?.pairingOpen && acceptNative && !nativeStarted) {
        nativeStarted = true; const action = state.progress.action;
        state.progress = {...state.progress, acceptanceState: 'running'}; publish();
        nativeDone = acceptNative(summary, {manifest: action.manifest, controllerVersion: action.controllerVersion, remainingMs: action.remainingSeconds * 1000}, audit,
          () => !!state.progress.cancelRequested || !!fault,
          message => {state.progress = {...state.progress, label: message}; publish();})
          .then(path => {state.progress = {...state.progress, acceptanceState: 'complete', credentialPath: path, label: 'Exact application confirmed. Credentials saved privately.'}; publish();})
          .catch(fail);
      }
    }});
    child.stdout.on('data', (chunk: Buffer) => {try {lines.write(chunk);} catch (error) {fail(error);}});
    child.stderr.on('data', (chunk: Buffer) => {stderrBytes += chunk.length; if (stderrBytes > 65536) fail(new Error('Backend diagnostics exceeded their bound.'));});
    const done = new Promise<'installed' | 'stopped'>((accept, reject) => {
      child.on('error', error => {fail(error); reject(error);});
      child.on('close', async code => {
        try {lines.end();} catch (error) {fail(error);}
        await nativeDone;
        if (fault) return reject(fault);
        if (state.terminal === 'installed' && acceptNative && !nativeStarted) return reject(new Error('The backend ended without the required native acceptance action.'));
        if (code === 0 && state.terminal === 'installed') return accept('installed');
        if (state.terminal === 'stopped') return accept('stopped');
        reject(new Error('The backend ended without a verified terminal result. Preserve the audit; do not retry automatically.'));
      });
    });
    publish();
    return {done, cancel, answer: (id, answer) => {write(state.answer(id, answer)); publish();}};
  };
}
