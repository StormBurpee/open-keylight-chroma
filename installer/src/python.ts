import {resolve} from 'node:path';

const scripts = new Set(['prepare_migration.py', 'stock_migration.py', 'vendor_restore.py']);
// Embedded Python deliberately ignores PYTHONPATH. Add only the selected,
// allowlisted backend's directory, without modifying its shared runtime cache.
const bootstrap = "import runpy,sys; from pathlib import Path; p=Path(sys.argv[1]).resolve(); sys.path.insert(0,str(p.parent)); sys.argv=sys.argv[1:]; runpy.run_path(str(p),run_name='__main__')";

export function pythonArguments(root: string, script: string, args: string[]): string[] {
  if (!scripts.has(script)) throw new Error('Unknown backend.');
  return ['-X', 'utf8', '-c', bootstrap, resolve(root, 'tools', script), ...args];
}
