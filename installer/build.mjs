import {build} from 'esbuild';
import {builtinModules} from 'node:module';
import {readFile, writeFile} from 'node:fs/promises';
import {createHash} from 'node:crypto';

const version = JSON.parse(await readFile('package.json', 'utf8')).version;
const productVersion = (await readFile('../VERSION', 'utf8')).trim();
if (typeof version !== 'string' || !/^(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)(?:-[0-9A-Za-z]+(?:[.-][0-9A-Za-z]+)*)?$/.test(version)
  || version.length > 31 || version !== productVersion) throw new Error('Installer package version must match the root VERSION before building.');

const result = await build({entryPoints: ['src/cli.tsx'], outfile: 'dist/cli.js', bundle: true, platform: 'node', format: 'esm', target: 'node22', minify: true,
  define: {'process.env.NODE_ENV': '"production"', 'process.env.DEV': '"false"'}, legalComments: 'linked', metafile: true,
  // Ink's optional developer-tools dynamic import is not part of this product.
  plugins: [{name: 'no-developer-server', setup(api) {
    api.onResolve({filter: /^\.\/devtools\.js$/}, args => args.importer.replaceAll('\\', '/').endsWith('/ink/build/reconciler.js') ? {path: 'disabled', namespace: 'devtools-disabled'} : undefined);
    api.onLoad({filter: /.*/, namespace: 'devtools-disabled'}, () => ({contents: 'export {};', loader: 'js'}));
  }}],
  banner: {js: "import {createRequire as __oklCreateRequire} from 'node:module';const require=__oklCreateRequire(import.meta.url);process.env.DEV='false';"}});
const builtins = new Set(builtinModules.map(name => name.replace(/^node:/, '')));
for (const output of Object.values(result.metafile.outputs)) for (const entry of output.imports) {
  if (entry.external && !builtins.has(entry.path.replace(/^node:/, ''))) throw new Error(`Unbundled runtime dependency: ${entry.path}`);
}
const packages = new Set(Object.keys(result.metafile.inputs).filter(path => path.startsWith('node_modules/')).map(path => {
  const parts = path.split('/'); return parts[1].startsWith('@') ? parts.slice(0, 3).join('/') : parts.slice(0, 2).join('/');
}));
const notices = [];
for (const path of [...packages].sort()) {
  const pkg = JSON.parse(await readFile(`${path}/package.json`, 'utf8')); let license;
  for (const name of ['LICENSE', 'license', 'LICENSE.md', 'license.md', 'LICENSE.txt', 'COPYING']) {
    try {license = await readFile(`${path}/${name}`, 'utf8'); break;} catch (error) {if (error.code !== 'ENOENT') throw error;}
  }
  // The npm tarball omits Yoga's license; retain the exact upstream v3.2.1 text.
  if (!license && pkg.name === 'yoga-layout' && pkg.version === '3.2.1') license = await readFile('licenses/yoga-layout.txt', 'utf8');
  if (!license) throw new Error(`License text missing for ${pkg.name}; do not publish incomplete notices.`);
  notices.push(`${pkg.name} ${pkg.version} (${pkg.license})\n${'='.repeat(72)}\n${license.trim()}\n`);
}
await writeFile('dist/THIRD_PARTY_NOTICES.txt', notices.join('\n'));
await writeFile('dist/package.json', JSON.stringify({type: 'module', engines: {node: '>=22'}, version}) + '\n');
const bytes = await readFile('dist/cli.js');
await writeFile('dist/build.json', JSON.stringify({format: 1, node: '>=22', version, bytes: bytes.length, sha256: createHash('sha256').update(bytes).digest('hex'), bundled_packages: packages.size, external_runtime_packages: 0}, null, 2) + '\n');
console.log(`Bundled ${bytes.length} bytes; ${packages.size} package notices; no external runtime packages.`);
