import {stripVTControlCharacters} from 'node:util';

/** Display untrusted filenames/backend messages as text, never terminal commands. */
export function display(value: string, maximum = 160): string {
  return stripVTControlCharacters(value).replace(/[\x00-\x1f\x7f-\x9f]/g, ' ').slice(0, maximum);
}

export const artifactKeys = ['identity', 'off1', 'low1', 'lighting', 'esp', 'assets'] as const;
export type ArtifactKey = typeof artifactKeys[number];
export type Draft = Record<ArtifactKey, string> & {
  ip: string; name: string; deviceId: string; commit: string;
  folder: string; restore: string; restoreVersion: string; provenance: string; output: string;
};
export const emptyDraft = (): Draft => ({
  ip: '', name: '', deviceId: '', commit: '', folder: '', identity: '', off1: '', low1: '',
  lighting: '', esp: '', assets: '', restore: '', restoreVersion: '1.3.0.0', provenance: '', output: '',
});
export const artifactLabels: Record<ArtifactKey, string> = {
  identity: 'Identity trial', off1: 'Darkness test · OFF1', low1: 'Five low pulses · LOW1',
  lighting: 'Light engine', esp: 'Dashboard + network', assets: 'Dashboard manifest',
};
export function privateIPv4(value: string): boolean {
  if (!/^(?:\d{1,3}\.){3}\d{1,3}$/.test(value)) return false;
  const octets = value.split('.').map(Number);
  if (octets.some((v, i) => v > 255 || String(v) !== value.split('.')[i])) return false;
  return octets[0] === 10 || (octets[0] === 172 && octets[1]! >= 16 && octets[1]! <= 31)
    || (octets[0] === 192 && octets[1] === 168);
}
export function draftErrors(draft: Draft): string[] {
  const errors: string[] = [];
  if (!privateIPv4(draft.ip)) errors.push('Enter the light’s private IPv4 address.');
  if (!draft.name.trim() || Buffer.byteLength(draft.name) > 64) errors.push('Enter its exact stock name (up to 64 UTF-8 bytes).');
  if (!/^keylight-[a-f0-9]{6}$/.test(draft.deviceId)) errors.push('Device ID: keylight- plus the last six ESP MAC digits.');
  if (!/^[a-f0-9]{40}$/.test(draft.commit)) errors.push('Use the complete reviewed source commit (40 lowercase hex digits).');
  for (const key of artifactKeys) if (!draft[key].trim()) errors.push(`Select ${artifactLabels[key]}.`);
  if (!draft.restore.trim()) errors.push('Select the reviewed owner-local restore bank.');
  if (!/^(?:\d{1,3}\.){3}\d{1,3}$/.test(draft.restoreVersion)
    || draft.restoreVersion.split('.').some(v => +v > 255)) errors.push('Restore version needs four bytes, for example 1.3.0.0.');
  if (draft.provenance.trim().length < 16 || draft.provenance.length > 1000) errors.push('Record restore origin, preserved tail, modifications and evidence (16–1000 characters).');
  if (!draft.output.trim()) errors.push('Choose a new local plan path.');
  if (Object.values(draft).some(v => /[\x00-\x1f\x7f]/.test(v))) errors.push('Fields must not contain control characters.');
  return errors;
}

export type PlanSummary = {
  profile: string; target_ip: string; target_name: string; device_id: string;
  manifest_sha256: string; source_commit?: string; packages: Record<string, string>;
  controller_version?: string;
  restore_sha256: string; restore_provenance: string;
  esp: {version: string; sha256: string; elf_sha256: string; bytes: number};
  device_operations: 0;
};
export function parseSummary(value: unknown): PlanSummary {
  const s = value as PlanSummary;
  const sha = (v: unknown) => typeof v === 'string' && /^[a-f0-9]{64}$/.test(v);
  if (!s || s.profile !== 'keylight-chroma-1.0.13' || !privateIPv4(s.target_ip ?? '')
    || typeof s.target_name !== 'string' || !/^keylight-[a-f0-9]{6}$/.test(s.device_id ?? '')
    || !sha(s.manifest_sha256) || !sha(s.restore_sha256) || typeof s.restore_provenance !== 'string'
    || !s.packages || !['identity', 'OFF1', 'LOW1', 'lighting'].every(k => sha(s.packages[k]))
    || !s.esp || typeof s.esp.version !== 'string' || !sha(s.esp.sha256) || !sha(s.esp.elf_sha256)
    || !Number.isSafeInteger(s.esp.bytes) || s.esp.bytes <= 0 || s.esp.bytes > 1572864
    || s.device_operations !== 0) throw new Error('The backend returned an invalid offline plan summary.');
  return s;
}

export const stages = [
  ['stock', 'Meet your light', 'Match stock identity and verify Off'],
  ['identity', 'Prove the connection', 'Read the actual controller part'],
  ['off1', 'Check darkness', 'One bounded all-low experiment'],
  ['low1', 'Check five channels', 'Red · green · blue · warm · cool'],
  ['lighting', 'Install the light engine', 'Check the lighting firmware, then start it'],
  ['esp', 'Install the dashboard', 'Keep the network bridge until last'],
  ['native', 'Make it yours', 'Pair, check controls and confirm'],
] as const;
export type InstallationMode = 'install' | 'finish';
export const finishStages = [
  ['existing', 'Check the light engine', 'Verify the installed Open Keylight controller'],
  ['esp', 'Install the dashboard', 'Update the ESP application only'],
  ['native', 'Make it yours', 'Pair, check controls and confirm'],
] as const;
export const stagesFor = (mode: InstallationMode = 'install') => mode === 'finish' ? finishStages : stages;
export type Progress = {
  failureCode?: 'stock_loader_entry_unconfirmed';
  workflow?: InstallationMode;
  unit?: 'blocks' | 'bytes' | 'seconds' | 'percent';
  stage: number; state: 'waiting' | 'running' | 'quiet' | 'prompt' | 'stopped' | 'complete';
  label: string; completed?: number; total?: number; remainingSeconds?: number;
  prompt?: {id: string; question: string};
  action?: {url: string; pairingOpen: boolean; remainingSeconds: number; manifest: string; controllerVersion: string};
  finishedStages?: number[];
  cancelRequested?: boolean;
  credentialPath?: string;
  dashboardUrl?: string;
  acceptanceState?: 'running' | 'complete';
};
export const previewProgress: Progress = {stage: 3, state: 'running', label: 'Verifying the LOW1 bank', completed: 312, total: 448};
