import {execFileSync} from 'node:child_process';
import {mkdir, writeFile} from 'node:fs/promises';
import {createHash} from 'node:crypto';

// Capture the bundled product's real Ink output, rather than recreate its UI.
const scenes = ['discover', 'review', 'install', 'observe', 'complete'];
const escape = value => value.replaceAll('&', '&amp;').replaceAll('<', '&lt;').replaceAll('>', '&gt;').replaceAll('"', '&quot;');
function ansiHtml(ansi) {
  let color = '', bold = false, dim = false, result = '', cursor = 0;
  for (const match of ansi.matchAll(/\x1b\[([0-9;]*)m/g)) {
    const part = ansi.slice(cursor, match.index);
    result += `<span style="${color ? `color:${color};` : ''}${bold ? 'font-weight:700;' : ''}${dim ? 'opacity:.65;' : ''}">${escape(part)}</span>`;
    const args = (match[1] || '0').split(';').map(Number);
    for (let i = 0; i < args.length; i++) {
      const code = args[i];
      if (code === 0) {color = ''; bold = false; dim = false;}
      else if (code === 1) bold = true;
      else if (code === 2) dim = true;
      else if (code === 22) {bold = false; dim = false;}
      else if (code === 39) color = '';
      else if (code === 38 && args[i + 1] === 2) {color = `rgb(${args.slice(i + 2, i + 5).join(',')})`; i += 4;}
      else throw new Error(`Unexpected terminal style ${code}; update the exact renderer before capture.`);
    }
    cursor = match.index + match[0].length;
  }
  const tail = ansi.slice(cursor);
  if (/\x1b/.test(ansi.replace(/\x1b\[[0-9;]*m/g, ''))) throw new Error('Unexpected non-style terminal command');
  return result + escape(tail);
}
await mkdir('dist/demo', {recursive: true});
const captures = [];
for (const scene of scenes) {
  const env = {...process.env, FORCE_COLOR: '3'}; delete env.NO_COLOR;
  const ansi = execFileSync(process.execPath, ['dist/cli.js', '--demo', scene, '--columns', '96'], {encoding: 'utf8', env});
  await writeFile(`dist/demo/${scene}.ansi`, ansi);
  const html = `<!doctype html><meta charset="utf-8"><meta name="viewport" content="width=device-width"><title>Open Keylight installer · ${scene} demo</title><style>*{box-sizing:border-box}body{margin:0;min-height:100vh;background:#eae8e0;color:#17292c;display:grid;place-items:center;padding:48px;font-family:system-ui,sans-serif}.caption{font-size:12px;letter-spacing:.16em;text-transform:uppercase;margin-bottom:18px;color:#586565}.terminal{background:#142328;color:#ede9df;border:1px solid #31464a;border-radius:15px;box-shadow:0 30px 70px #24383b30;overflow:hidden}.bar{height:48px;border-bottom:1px solid #ffffff10;display:flex;align-items:center;gap:8px;padding:0 20px;background:#182a30}.dot{width:9px;height:9px;background:#526367;border-radius:50%}.title{margin:auto;color:#99abaa;font-size:12px;letter-spacing:.04em;padding-right:40px}pre{font-family:"Cascadia Code","DejaVu Sans Mono",Consolas,monospace;font-size:14px;line-height:1.55;margin:0;padding:18px 24px 24px;white-space:pre}.foot{font-size:12px;color:#61716e;margin-top:18px;display:flex;justify-content:space-between}a{color:inherit}</style><main><div class="caption">Open Keylight Chroma / Guided setup</div><section class="terminal"><div class="bar"><i class="dot"></i><i class="dot"></i><i class="dot"></i><span class="title">OPEN KEYLIGHT INSTALLER</span></div><pre>${ansiHtml(ansi)}</pre></section><div class="foot"><span>Actual Ink rendering · illustrative ${scene} screen</span><span>No device activity</span></div></main>`;
  await writeFile(`dist/demo/${scene}.html`, html);
  captures.push({scene, ansi_sha256: createHash('sha256').update(ansi).digest('hex'), device_operations: 0});
}
await writeFile('dist/demo/captures.json', JSON.stringify({format: 1, origin: 'Bundled CLI --demo: actual Ink ANSI output, illustrative fixture', captures}, null, 2));
console.log('Five real Ink demo captures written to dist/demo. No device activity.');
