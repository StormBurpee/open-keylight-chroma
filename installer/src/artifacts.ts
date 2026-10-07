import {readdir, stat} from 'node:fs/promises';
import {resolve} from 'node:path';
import type {ArtifactKey} from './model.js';

const names: Record<ArtifactKey, RegExp> = {
  identity: /^identity\.oklnxp$/i, off1: /^off1?\.oklnxp$/i, low1: /^low1?\.oklnxp$/i,
  lighting: /^lighting\.oklnxp$/i, esp: /^(?:open_keylight|open-keylight[^/]*)\.bin$/i,
  assets: /^asset-manifest\.json$/i,
};
export type Discovery = {files: {path: string; bytes: number}[]; suggested: Partial<Record<ArtifactKey, string>>; ambiguous: ArtifactKey[]};

/** Local files only. Names are suggestions; Python performs authoritative admission. */
export async function discoverArtifacts(folder: string): Promise<Discovery> {
  const directory = resolve(folder);
  const entries = await readdir(directory, {withFileTypes: true});
  if (entries.length > 512) throw new Error('Choose a smaller artifact folder (at most 512 entries).');
  const files: Discovery['files'] = [];
  for (const entry of entries.sort((a, b) => a.name.localeCompare(b.name))) {
    if (!entry.isFile() || !/\.(?:oklnxp|bin|json)$/i.test(entry.name)) continue;
    const path = resolve(directory, entry.name);
    const metadata = await stat(path);
    if (metadata.size > 2097152) continue;
    files.push({path, bytes: metadata.size});
  }
  const suggested: Discovery['suggested'] = {}, ambiguous: ArtifactKey[] = [];
  for (const key of Object.keys(names) as ArtifactKey[]) {
    const candidates = files.filter(f => names[key].test(f.path.slice(directory.length + 1)));
    if (candidates.length === 1) suggested[key] = candidates[0]!.path;
    else if (candidates.length > 1) ambiguous.push(key);
  }
  return {files, suggested, ambiguous};
}
