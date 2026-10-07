import {execFile} from 'node:child_process';
import {promisify} from 'node:util';
import {mkdir, mkdtemp, open, chmod} from 'node:fs/promises';
import {homedir} from 'node:os';
import {join, win32} from 'node:path';
const execute = promisify(execFile);

/** Restrict a new directory before it contains any credential. Fail closed if ACL setup fails. */
export async function privateDirectory(parent = join(homedir(), '.open-keylight', 'credentials')): Promise<string> {
  await mkdir(parent, {recursive: true, mode: 0o700});
  const directory = await mkdtemp(join(parent, 'install-'));
  if (process.platform === 'win32') {
    const windows = process.env['SystemRoot'];
    if (!windows || !win32.isAbsolute(windows)) throw new Error('Windows system directory is unavailable.');
    const system = win32.join(windows, 'System32');
    const options = {windowsHide: true, timeout: 5000, maxBuffer: 8192};
    const identity = await execute(win32.join(system, 'whoami.exe'), ['/user', '/fo', 'csv', '/nh'], options);
    const sid = identity.stdout.match(/\bS-1-(?:\d+-)+\d+\b/)?.[0];
    if (!sid) throw new Error('Could not establish current-user credential permissions.');
    await execute(win32.join(system, 'icacls.exe'), [directory, '/inheritance:r', '/grant:r', `*${sid}:(OI)(CI)F`], options);
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
