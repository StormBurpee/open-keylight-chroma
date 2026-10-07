#!/usr/bin/env node
import React from 'react';
import {parseArgs} from 'node:util';
import {render, renderToString} from 'ink';
import {App} from './App.js';
import {ProgressView} from './components.js';
import {previewProgress} from './model.js';
import {pythonRunner, validatePlan, repository} from './backend.js';
import {installationRunner} from './execution.js';
import {nativeAcceptance} from './native-session.js';
import {Demo, demoScenes, type DemoScene} from './demo.js';

async function main() {
const {values} = parseArgs({options: {
  plan: {type: 'string'}, artifacts: {type: 'string'}, bundle: {type: 'string'}, preview: {type: 'boolean'},
  plain: {type: 'boolean'}, help: {type: 'boolean'}, python: {type: 'string'}, root: {type: 'string'}, demo: {type: 'string'}, columns: {type: 'string'},
}, allowPositionals: false, strict: true});
if (values.help) {
  console.log('Open Keylight installer\n\n  node cli.js [--bundle path] [--root release-folder] [--python executable]\n  node cli.js --plan path [--plain]\n  node cli.js --demo discover|review|install|observe|complete [--columns 96]\n  node cli.js --preview [--plain]\n\nNode 22+, Python 3. Live installs require an interactive review and explicit start.\nDemo and preview never discover, download, spawn a backend or contact a light.\nAdvanced: --artifacts folder. OKL_PYTHON selects the Python executable.');
} else if (values.demo) {
  if (!(demoScenes as readonly string[]).includes(values.demo)) throw new Error('Unknown demo scene. Use discover, review, install, observe or complete.');
  const columns = Number(values.columns ?? 96);
  if (!Number.isInteger(columns) || columns < 60 || columns > 120) throw new Error('Demo columns must be between 60 and 120.');
  console.log(renderToString(<Demo scene={values.demo as DemoScene} width={columns} />, {columns}));
} else if (values.plain) {
  if (values.preview) console.log(renderToString(<ProgressView progress={previewProgress} preview />, {columns: 96}));
  else if (values.plan) console.log(JSON.stringify(await validatePlan(values.plan, pythonRunner(values.python, values.root ?? repository)), null, 2));
  else throw new Error('--plain requires --plan or --preview.');
} else if (!process.stdin.isTTY || !process.stdout.isTTY) {
  throw new Error('Interactive mode needs a terminal. Use --plan path --plain or --preview --plain.');
} else {
  await render(<App run={pythonRunner(values.python, values.root ?? repository)} start={installationRunner(values.python, values.root ?? repository, undefined, nativeAcceptance)} initialPlan={values.plan} initialFolder={values.artifacts} initialBundle={values.bundle} root={values.root} preview={values.preview} />,
    {exitOnCtrlC: false}).waitUntilExit();
}
}
main().catch(error => {console.error(error instanceof Error ? error.message : 'Installer failed.'); process.exitCode = 1;});
