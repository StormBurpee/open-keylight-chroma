import {readFile, realpath, stat} from 'node:fs/promises';
import {createHash} from 'node:crypto';
import {dirname, isAbsolute, relative, resolve, sep} from 'node:path';
import type {Draft} from './model.js';
export type Bundle = {path: string; version: string; commit: string; files: Pick<Draft, 'identity' | 'off1' | 'low1' | 'lighting' | 'esp' | 'assets'>};
function need(value: unknown): asserts value {if (!value) throw new Error('The release bundle is malformed, changed, or contains an unsafe path.');}
const digest = (v: Buffer) => createHash('sha256').update(v).digest('hex');

/** Hash every bounded artifact before returning paths. Python still validates firmware contents. */
export async function loadBundle(path: string): Promise<Bundle> {
  const absolute = await realpath(path), root = await realpath(dirname(absolute)), info = await stat(absolute);
  need(info.isFile() && info.size <= 32768);
  const v = JSON.parse(await readFile(absolute, 'utf8'));
  need(v?.format === 1 && v.product === 'open-keylight-chroma' && v.stock_profile === 'keylight-chroma-1.0.13'
    && typeof v.version === 'string' && /^\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?$/.test(v.version)
    && /^[a-f0-9]{40}$/.test(v.source_commit) && v.packages);
  const file = async (entry: any, minimum: number, maximum: number) => {
    need(entry && typeof entry.path === 'string' && entry.path.length <= 240 && !isAbsolute(entry.path)
      && !entry.path.includes('\\') && !entry.path.includes(':') && !/[\x00-\x1f\x7f]/.test(entry.path)
      && entry.path.split('/').every((p: string) => p && p !== '.' && p !== '..')
      && /^[a-f0-9]{64}$/.test(entry.sha256) && Number.isSafeInteger(entry.bytes) && entry.bytes >= minimum && entry.bytes <= maximum);
    const selected = await realpath(resolve(root, entry.path)), rel = relative(root, selected);
    need(rel && rel !== '..' && !rel.startsWith(`..${sep}`) && !isAbsolute(rel));
    const metadata = await stat(selected); need(metadata.isFile() && metadata.size === entry.bytes);
    const bytes = await readFile(selected); need(bytes.length === entry.bytes && digest(bytes) === entry.sha256);
    return selected;
  };
  const files = {identity: await file(v.packages.identity, 28736, 28736), off1: await file(v.packages.OFF1, 28736, 28736), low1: await file(v.packages.LOW1, 28736, 28736), lighting: await file(v.packages.lighting, 28736, 28736), esp: await file(v.esp, 288, 1572864), assets: await file(v.assets, 2, 131072)};
  need(new Set(Object.values(files)).size === 6);
  return {path: absolute, version: v.version, commit: v.source_commit, files};
}

export async function findBundle(explicit: string | undefined, root: string): Promise<Bundle> {
  if (explicit) return loadBundle(resolve(explicit));
  const candidates = ['bundle.json', 'firmware/bundle.json', '../bundle.json', 'release/bundle.json']
    .map(path => resolve(root, path));
  for (const candidate of candidates) {
    try {await stat(candidate);} catch (error) {if ((error as NodeJS.ErrnoException).code === 'ENOENT') continue; throw error;}
    return loadBundle(candidate); // An existing invalid bundle is never silently skipped.
  }
  throw new Error(`No release bundle found. Checked: ${candidates.join(', ')}. Use an extracted release or pass --bundle with the absolute path to a prepared bundle.json; build directories are not searched.`);
}
