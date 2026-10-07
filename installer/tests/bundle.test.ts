import {test} from 'node:test';
import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import {mkdtemp, mkdir, writeFile, rm, realpath} from 'node:fs/promises';
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
test('source checkout does not scan build output and the missing-bundle error is actionable', async () => {
  await fixture(async root => {
    await bundle(join(root, 'build/release/9999-latest'));
    await assert.rejects(findBundle(undefined, root), error => {
      assert.ok(error instanceof Error);
      assert.ok(error.message.includes(join(root, 'firmware/bundle.json')));
      assert.match(error.message, /--bundle.*absolute path/);
      assert.match(error.message, /build directories are not searched/);
      return true;
    });
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
