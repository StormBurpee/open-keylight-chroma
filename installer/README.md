# Guided installer

A React Ink interface for the reviewed Python migration engine. In a packaged Windows release, open `start-open-keylight.cmd`. The launcher supplies private portable runtimes; no global Node or Python installation is needed. For source development, use Node 22 or newer and Python 3:

```text
cd installer
npm ci
node --import tsx src/cli.tsx
```

These commands work in PowerShell and POSIX shells. Set `OKL_PYTHON` to a Python executable if it is not on `PATH`. Prefer a terminal at least 80 columns wide.

Start with an existing plan or a folder of built artifacts:

```text
node --import tsx src/cli.tsx --plan "/path/to/migration.json"
node --import tsx src/cli.tsx --artifacts "/path/to/artifacts"
```

The default flow finds lights with a bounded, read-only local mDNS query. Choose a name, review the bundled release, and install. Already installed lights link to their dashboard. Discovery supplies selection hints; the backend independently verifies the exact target before writing. Advanced setup accepts explicit targets, builds and existing plans.

The six artifacts in `bundle.json` are checked against their declared hashes and confined relative paths. The Python backend then validates the actual package contents, image and embedded dashboard assets. The verified plan digest is passed back when you explicitly start, preventing an unnoticed plan change between review and execution. A hash establishes integrity, not authorship or electrical qualification; use a reviewed release for the supported hardware.

Automatic lookup checks `bundle.json`, `firmware/bundle.json` (the packaged release layout), `../bundle.json`, then `release/bundle.json`, relative to the repository or selected `--root`. The first existing bundle must validate; a malformed candidate is never skipped. Build directories are not scanned or selected by age. In a source checkout, select the intended prepared bundle explicitly when none is in those locations. Explicit paths are relative to the running process's working directory. Since npm runs scripts from the package directory, prefer an absolute path with `npm --prefix installer start -- --bundle "/absolute/path/to/bundle.json"`.

The recovery step downloads the pinned official Razer archive through `tools/vendor_restore.py`, checks it and derives the complete reviewed recovery bank. An exact existing cache is reused. This is a **vendor-derived restore image, not a backup of your device**. Advanced users can supply a complete reviewed local bank and its real provenance.

During installation, progress comes only from versioned JSONL events. You must observe and answer both physical checks. No answer defaults to Yes. After the backend verifies the exact new application, controller and dashboard assets, the installer pairs once, checks a brief 5% white output and Off through authenticated readbacks, then confirms the exact application once. It binds every command to the current revision and checks that the ESP has not restarted. These are protocol checks, not optical measurements.

Pairing credentials are stored in a new private local directory: owner-only permissions on POSIX, or an explicit current-user ACL on Windows. Tokens never appear in progress events, URLs or audit records. The final screen shows the credential file path and dashboard URL, so you can connect the dashboard after installation without rushing the first-boot trial. A lost pairing or confirmation response is never retried automatically.

**Ctrl+C requests a stop after the current safe stage.** It does not kill the Python process during a write, commit, quiet interval or diagnostic return. Keep the terminal and power connected until the backend reports its result. A failed or ambiguous operation is never retried automatically. Keep the exclusive audit file for diagnosis and follow [the recovery matrix](../docs/stock-migration.md).

## Offline inspection and development

```text
node --import tsx src/cli.tsx --preview --plain
node --import tsx src/cli.tsx --demo install
node --import tsx src/cli.tsx --plan "/path/to/migration.json" --plain
npm run check
npm test
npm run build
node dist/src/cli.js --preview --plain
npm run bundle
npm run test:bundle
npm run capture:demo
```

Preview and the five deterministic demo scenes (`discover`, `review`, `install`, `observe`, `complete`) perform no discovery, downloads, subprocess work or device operations. `--columns 80` selects a narrower demo. Tests use local files, fake event streams and subprocess fixtures; they do not connect to lights. `dist/` is generated, not committed.

`npm run bundle` creates a self-contained Node 22 ESM executable at `dist/cli.js`, its `package.json`, a build digest and complete dependency license notices. The isolated smoke test runs without `node_modules` or Python. A release places these under `installer/`, Python helpers under `tools/`, and the firmware bundle under `firmware/`. Launch it with:

```text
node installer/cli.js --root "/path/to/release" --bundle "/path/to/release/firmware/bundle.json" --python "/path/to/python"
```

Both Python entrypoints use one fixed `runpy` bootstrap that adds the allowlisted script directory. This supports Windows embedded Python without modifying its shared `._pth` file or depending on `PYTHONPATH`.

`capture:demo` saves the actual bundled Ink ANSI output and a faithful HTML text/style rendering under `dist/demo/`. Screenshots must be captioned as illustrative interface captures, never installation evidence.

The live adapter expects the repository's `--events-jsonl` backend. It rejects unsupported, malformed or out-of-order events and an exit without a verified terminal result. It never scrapes human console messages. The seven stage IDs and cooperative cancellation contract are tested alongside the UI; live hardware qualification is a separate operator task.

The UI uses [Ink 8.0.0](https://github.com/vadimdemedes/ink/releases/tag/v8.0.0), [React](https://react.dev/) and [Ink testing library](https://github.com/vadimdemedes/ink-testing-library), pinned in the lockfile. UI dependencies and original firmware ship in the release. Discovery queries the local network; the pinned official recovery archive is downloaded only during the selected preparation flow.
