import {test} from 'node:test';
import assert from 'node:assert/strict';
import {execFileSync} from 'node:child_process';
import {mkdtemp, mkdir, writeFile, rm} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import {repository} from '../src/backend.js';
import {installationRunner, type Execution} from '../src/execution.js';
import {parseSummary, type Progress} from '../src/model.js';

const python = process.env['OKL_PYTHON'] ?? (process.platform === 'win32' ? 'python' : 'python3');
test('real Python Migration JSONL child composes with TUI prompts, native action and verified terminal result', {timeout: 20000}, async () => {
  const root = await mkdtemp(join(tmpdir(), 'okl actual event child '));
  try {
    const fixture = join(repository, 'tests/migration/jsonl_fixture.py');
    const created = JSON.parse(execFileSync(python, [fixture, '--create', join(root, 'fixture')], {encoding: 'utf8'}));
    const plan = parseSummary(JSON.parse(execFileSync(python, [fixture, 'prepare', '--manifest', created.manifest], {encoding: 'utf8'})));
    await mkdir(join(root, 'tools'));
    // Fixed test-only shim executes actual backend CLI with fake hardware
    // boundaries. The Python fixture forbids socket construction globally.
    await writeFile(join(root, 'tools/stock_migration.py'), `import runpy\nrunpy.run_path(${JSON.stringify(fixture)},run_name='__main__')\n`);
    const answers = new Set<string>(), stages = new Set<number>();
    let nativeCalls = 0, latest: Progress | undefined, job: Execution;
    const runner = installationRunner(python, root, undefined, async (actual, action) => {
      nativeCalls++; assert.equal(actual.manifest_sha256, plan.manifest_sha256);
      assert.equal(action.manifest, plan.manifest_sha256); assert.ok(action.remainingMs > 30000);
      return '/synthetic/private/access.json';
    });
    job = runner(created.manifest, join(root, 'audit.jsonl'), plan, progress => {
      latest = progress;
      for (const stage of progress.finishedStages ?? []) stages.add(stage);
      if (progress.prompt && !answers.has(progress.prompt.id)) {
        const id = progress.prompt.id; answers.add(id);
        queueMicrotask(() => job.answer(id, 'yes')); // Explicit fixture observation, never product auto-answer.
      }
    });
    assert.equal(await job.done, 'installed');
    assert.equal(answers.size, 2); assert.equal(nativeCalls, 1); assert.equal(stages.size, 7);
    assert.equal(latest?.state, 'complete'); assert.equal(latest?.acceptanceState, 'complete');
    assert.equal(latest?.dashboardUrl, `http://${plan.target_ip}/`);
  } finally {await rm(root, {recursive: true, force: true});}
});
