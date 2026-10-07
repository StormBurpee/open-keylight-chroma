import {test} from 'node:test';
import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import {mkdtemp, mkdir, readFile, writeFile, rm, realpath, symlink, utimes} from 'node:fs/promises';
import {join, relative, resolve} from 'node:path';
import {tmpdir} from 'node:os';
import {findBundle} from '../src/bundle.js';

async function bundle(root: string, version = '0.2.0-alpha.1') {
  await mkdir(root, {recursive: true});
  const entry = async (path: string, size: number) => {
    const bytes = Buffer.alloc(size, path.length);
    const file = join(root, path); await mkdir(resolve(file, '..'), {recursive: true});
    await writeFile(file, bytes);
    return {path, bytes: size, sha256: createHash('sha256').update(bytes).digest('hex')};
  };
  const manifest = {format: 1, product: 'open-keylight-chroma', version, source_commit: 'a'.repeat(40),
    stock_profile: 'keylight-chroma-1.0.13', packages: {
      identity: await entry('controller/identity.oklnxp', 28736), OFF1: await entry('controller/OFF1.oklnxp', 28736),
      LOW1: await entry('controller/LOW1.oklnxp', 28736), lighting: await entry('controller/lighting.oklnxp', 28736)},
    esp: await entry('esp32/open_keylight.bin', 400), assets: await entry('esp32/asset-manifest.json', 100)};
  const path = join(root, 'bundle.json'); await writeFile(path, JSON.stringify(manifest)); return path;
}
async function fixture(run: (root: string) => Promise<void>) {
  const temp = await mkdtemp(join(tmpdir(), 'okl bundle lookup '));
  const root = join(temp, 'release'); await mkdir(root);
  try {await run(root);} finally {await rm(temp, {recursive: true, force: true});}
}

test('automatic lookup finds the actual extracted release firmware layout', async () => {
  await fixture(async root => {
    await mkdir(join(root, 'installer')); await mkdir(join(root, 'tools'));
    const expected = await bundle(join(root, 'firmware'));
    const found = await findBundle(undefined, root);
    assert.equal(found.path, await realpath(expected));
    assert.equal(found.files.identity, await realpath(join(root, 'firmware/controller/identity.oklnxp')));
    assert.equal(found.files.esp, await realpath(join(root, 'firmware/esp32/open_keylight.bin')));
  });
});
test('lookup precedence is explicit, root, packaged firmware, parent, then release', async () => {
  await fixture(async root => {
    const fallback = await bundle(join(root, 'release'), '0.2.0-alpha.2');
    const parent = await bundle(resolve(root, '..'), '0.2.0-alpha.3');
    const firmware = await bundle(join(root, 'firmware'), '0.2.0-alpha.4');
    const primary = await bundle(root, '0.2.0-alpha.5');
    assert.equal((await findBundle(fallback, root)).path, await realpath(fallback));
    assert.equal((await findBundle(undefined, root)).path, await realpath(primary));
    await rm(primary);
    assert.equal((await findBundle(undefined, root)).path, await realpath(firmware));
    await rm(firmware);
    assert.equal((await findBundle(undefined, root)).path, await realpath(parent));
    await rm(parent);
    assert.equal((await findBundle(undefined, root)).path, await realpath(fallback));
  });
});
test('a present invalid or tampered candidate never falls through to another bundle', async () => {
  await fixture(async root => {
    await bundle(join(root, 'release'));
    const firmware = await bundle(join(root, 'firmware'));
    const primary = join(root, 'bundle.json'); await writeFile(primary, '{}');
    await assert.rejects(findBundle(undefined, root), /malformed/);
    await rm(primary);
    await writeFile(join(root, 'firmware/esp32/open_keylight.bin'), Buffer.alloc(400));
    await assert.rejects(findBundle(undefined, root), /malformed/);
    await assert.rejects(findBundle(join(root, 'missing.json'), root), /ENOENT/);
    await writeFile(firmware, '{broken');
    await assert.rejects(findBundle(undefined, root), SyntaxError);
  });
});
test('source checkout scans only the exact published local release layout', async () => {
  await fixture(async root => {
    await bundle(join(root, 'build/release/9999-latest'));
    await bundle(join(root, 'build/release/.unpublished/firmware'));
    await bundle(join(root, 'build/release/nested/another/firmware'));
    await assert.rejects(findBundle(undefined, root), error => {
      assert.ok(error instanceof Error);
      assert.ok(error.message.includes(join(root, 'firmware/bundle.json')));
      assert.match(error.message, /--bundle.*exact bundle.json path/);
      assert.match(error.message, /prepare a local release build/);
      return true;
    });
    const selected = await bundle(join(root, 'build/release/published/firmware'));
    assert.equal((await findBundle(undefined, root)).path, await realpath(selected));
    const packaged = await bundle(join(root, 'firmware'), '0.1.0');
    assert.equal((await findBundle(undefined, root)).path, await realpath(packaged));
  });
});
test('local selection follows SemVer instead of directory names, timestamps or lexical version order', async () => {
  await fixture(async root => {
    const versions = ['0.2.0-alpha.1', '0.2.0-alpha.2', '0.2.0-alpha.10', '0.2.0-alpha.beta',
      '0.2.0-beta', '0.2.0-beta.2', '0.2.0-beta.11', '0.2.0-rc.1', '0.2.0', '0.10.0', '1.0.0'];
    for (const [index, version] of versions.entries()) {
      const latest = await bundle(join(root, `build/release/name-${versions.length - index}/firmware`), version);
      // Later semantic versions deliberately have older filesystem timestamps.
      await utimes(latest, 1700000000 - index, 1700000000 - index);
      assert.equal((await findBundle(undefined, root)).path, await realpath(latest));
    }
  });
});
test('same-version local builds use manifest mtime then a deterministic path tie-break', async () => {
  await fixture(async root => {
    const old = await bundle(join(root, 'build/release/zzz-claims-latest/firmware'));
    const newer = await bundle(join(root, 'build/release/pairing/firmware'));
    await utimes(old, 1700000000, 1700000000); await utimes(newer, 1700000001, 1700000001);
    // The directory timestamp is irrelevant.
    await utimes(resolve(old, '../..'), 1800000000, 1800000000);
    assert.equal((await findBundle(undefined, root)).path, await realpath(newer));
    const tied = await bundle(join(root, 'build/release/aaa/firmware'));
    await utimes(tied, 1700000001, 1700000001);
    assert.equal((await findBundle(undefined, root)).path, await realpath(tied));
    assert.equal((await findBundle(old, root)).path, await realpath(old));
  });
});
test('the selected local build is fully validated and corruption never chooses an older build', async () => {
  await fixture(async root => {
    await bundle(join(root, 'build/release/old/firmware'), '0.1.0');
    await bundle(join(root, 'build/release/new/firmware'), '0.2.0');
    await writeFile(join(root, 'build/release/new/firmware/esp32/open_keylight.bin'), Buffer.alloc(400));
    await assert.rejects(findBundle(undefined, root), /malformed/);
  });
});
test('malformed local manifest metadata cannot silently fall through or affect version ranking', async () => {
  for (const version of ['01.2.0', '0.2.0-alpha.01', '0.2.0-', '0.2.0-alpha..1', true]) {
    await fixture(async root => {
      await bundle(join(root, 'build/release/valid/firmware'));
      const invalid = await bundle(join(root, 'build/release/invalid/firmware'));
      const raw = JSON.parse(await readFile(invalid, 'utf8'));
      raw.version = version; await writeFile(invalid, JSON.stringify(raw));
      await assert.rejects(findBundle(undefined, root), /malformed/);
    });
  }
});
test('local scan is bounded and rejects firmware path escapes', async () => {
  await fixture(async root => {
    const releases = join(root, 'build/release'); await mkdir(releases, {recursive: true});
    await Promise.all(Array.from({length: 257}, (_, i) => mkdir(join(releases, `empty-${i}`))));
    await assert.rejects(findBundle(undefined, root), /Too many local release entries/);
  });
  await fixture(async root => {
    const outside = await mkdtemp(join(tmpdir(), 'okl outside build '));
    try {
      await bundle(outside); const release = join(root, 'build/release/escape'); await mkdir(release, {recursive: true});
      await symlink(outside, join(release, 'firmware'), process.platform === 'win32' ? 'junction' : 'dir');
      await assert.rejects(findBundle(undefined, root), /malformed/);
    } finally {await rm(outside, {recursive: true, force: true});}
  });
});
test('explicit relative bundle resolves from process cwd, independently of --root or npm INIT_CWD', async () => {
  await fixture(async root => {
    const expected = await bundle(join(root, 'chosen'));
    const prior = process.env['INIT_CWD']; process.env['INIT_CWD'] = join(root, 'does-not-exist');
    try {
      const found = await findBundle(relative(process.cwd(), expected), join(root, 'unrelated-root'));
      assert.equal(found.path, await realpath(expected));
    } finally {
      if (prior === undefined) delete process.env['INIT_CWD']; else process.env['INIT_CWD'] = prior;
    }
  });
});
