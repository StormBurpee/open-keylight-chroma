import {opendir, readFile, realpath, stat} from 'node:fs/promises';
import {createHash} from 'node:crypto';
import {dirname, isAbsolute, relative, resolve, sep} from 'node:path';
import type {Draft} from './model.js';
export type Bundle = {path: string; version: string; commit: string; files: Pick<Draft, 'identity' | 'off1' | 'low1' | 'lighting' | 'esp' | 'assets'>};
function need(value: unknown): asserts value {if (!value) throw new Error('The release bundle is malformed, changed, or contains an unsafe path.');}
const digest = (v: Buffer) => createHash('sha256').update(v).digest('hex');
type Version = {release: bigint[]; prerelease: string[]};
function version(value: unknown): Version {
  need(typeof value === 'string' && value.length <= 64);
  const match = /^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(?:-([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?$/.exec(value);
  need(match);
  const prerelease = match[4]?.split('.') ?? [];
  need(prerelease.every(part => !/^\d+$/.test(part) || part === '0' || !part.startsWith('0')));
  return {release: match.slice(1, 4).map(part => BigInt(part!)), prerelease};
}
function compareVersion(a: Version, b: Version): number {
  for (let i = 0; i < 3; i++) if (a.release[i] !== b.release[i]) return a.release[i]! > b.release[i]! ? 1 : -1;
  if (!a.prerelease.length || !b.prerelease.length) return Number(!a.prerelease.length) - Number(!b.prerelease.length);
  for (let i = 0; i < Math.max(a.prerelease.length, b.prerelease.length); i++) {
    const left = a.prerelease[i], right = b.prerelease[i];
    if (left === right) continue;
    if (left === undefined || right === undefined) return left === undefined ? -1 : 1;
    const ln = /^\d+$/.test(left), rn = /^\d+$/.test(right);
    if (ln && rn) return BigInt(left) > BigInt(right) ? 1 : -1;
    if (ln !== rn) return ln ? -1 : 1;
    return left > right ? 1 : -1;
  }
  return 0;
}
const within = (root: string, path: string) => {
  const rel = relative(root, path);
  return Boolean(rel && rel !== '..' && !rel.startsWith(`..${sep}`) && !isAbsolute(rel));
};
async function manifest(path: string) {
  const absolute = await realpath(path), info = await stat(absolute);
  need(info.isFile() && info.size <= 32768);
  const raw = await readFile(absolute); need(raw.length <= 32768);
  const v = JSON.parse(raw.toString('utf8'));
  need(v?.format === 1 && v.product === 'open-keylight-chroma' && v.stock_profile === 'keylight-chroma-1.0.13'
    && /^[a-f0-9]{40}$/.test(v.source_commit) && v.packages);
  return {absolute, v, version: version(v.version), sha256: digest(raw), mtime: info.mtimeMs};
}

/** Hash every bounded artifact before returning paths. Python still validates firmware contents. */
export async function loadBundle(path: string): Promise<Bundle> {
  return checkedBundle(path);
}
async function checkedBundle(path: string, expectedManifest?: string): Promise<Bundle> {
  const {absolute, v, sha256} = await manifest(path), root = await realpath(dirname(absolute));
  need(expectedManifest === undefined || sha256 === expectedManifest);
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

async function localBuild(root: string): Promise<Bundle | undefined> {
  const base = resolve(root, 'build/release');
  try {await stat(base);} catch (error) {if ((error as NodeJS.ErrnoException).code === 'ENOENT') return; throw error;}
  const realBase = await realpath(base); need(within(await realpath(root), realBase));
  const candidates: Awaited<ReturnType<typeof manifest>>[] = [];
  let entries = 0;
  // One directory level only. Ignore unpublished hidden directories and links;
  // never recurse through build trees or choose by a directory's name.
  for await (const entry of await opendir(base, {bufferSize: 32})) {
    if (++entries > 256) throw new Error('Too many local release entries to inspect safely. Choose a bundle with --bundle.');
    if (!entry.isDirectory() || entry.name.startsWith('.')) continue;
    const path = resolve(base, entry.name, 'firmware/bundle.json');
    try {await stat(path);} catch (error) {if ((error as NodeJS.ErrnoException).code === 'ENOENT') continue; throw error;}
    need(within(realBase, await realpath(path)));
    if (candidates.length >= 64) throw new Error('Too many local bundles to inspect safely. Choose a bundle with --bundle.');
    candidates.push(await manifest(path));
  }
  // Local developer default: SemVer first, manifest timestamp second, stable
  // path tie-break last. Hash validation remains mandatory before UI review.
  candidates.sort((a, b) => compareVersion(b.version, a.version) || b.mtime - a.mtime
    || (a.absolute < b.absolute ? -1 : a.absolute > b.absolute ? 1 : 0));
  const selected = candidates[0];
  if (selected) return checkedBundle(selected.absolute, selected.sha256);
}

export async function findBundle(explicit: string | undefined, root: string): Promise<Bundle> {
  if (explicit) return loadBundle(resolve(explicit));
  const candidates = ['bundle.json', 'firmware/bundle.json', '../bundle.json', 'release/bundle.json']
    .map(path => resolve(root, path));
  for (const candidate of candidates) {
    try {await stat(candidate);} catch (error) {if ((error as NodeJS.ErrnoException).code === 'ENOENT') continue; throw error;}
    return loadBundle(candidate); // An existing invalid bundle is never silently skipped.
  }
  const built = await localBuild(root);
  if (built) return built;
  throw new Error(`No release bundle found. Checked: ${candidates.join(', ')}, ${resolve(root, 'build/release/*/firmware/bundle.json')}. Use an extracted release or prepare a local release build; --bundle also accepts an exact bundle.json path.`);
}
