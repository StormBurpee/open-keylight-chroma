import {execFile} from 'node:child_process';
import {promisify} from 'node:util';
import {mkdir, mkdtemp, open, chmod} from 'node:fs/promises';
import {homedir} from 'node:os';
import {join} from 'node:path';
const execute = promisify(execFile);

/** Restrict a new directory before it contains any credential. Fail closed if ACL setup fails. */
export async function privateDirectory(parent = join(homedir(), '.open-keylight', 'credentials')): Promise<string> {
  await mkdir(parent, {recursive: true, mode: 0o700});
  const directory = await mkdtemp(join(parent, 'install-'));
  if (process.platform === 'win32') {
    const identity = await execute('whoami.exe', ['/user', '/fo', 'csv', '/nh'], {windowsHide: true});
    const sid = identity.stdout.match(/\bS-1-(?:\d+-)+\d+\b/)?.[0];
    if (!sid) throw new Error('Could not establish current-user credential permissions.');
    await execute('icacls.exe', [directory, '/inheritance:r', '/grant:r', `*${sid}:(OI)(CI)F`], {windowsHide: true});
  } else {await chmod(directory, 0o700);}
  return directory;
}

export async function writeCredential(directory: string, deviceId: string, ip: string, token: string): Promise<string> {
  if (!/^keylight-[a-f0-9]{6}$/.test(deviceId) || !/^[a-f0-9]{64}$/.test(token)) throw new Error('Invalid credential identity.');
  const path = join(directory, `${deviceId}.json`), file = await open(path, 'wx', 0o600);
  try {await file.writeFile(JSON.stringify({format: 1, device_id: deviceId, ip, token}) + '\n'); await file.sync();}
  finally {await file.close();}
  return path;
}
