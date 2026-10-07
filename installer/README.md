# Guided installer

The guided setup uses React and Ink, with a Python backend that verifies each device operation. For a packaged Windows release, extract the ZIP and open **start-open-keylight.cmd**. No global Node or Python installation is needed. Check [Getting started](../docs/getting-started.md) for supported hardware, current release status and recovery limits.

Choose **Find my light**, select your light, then review and install. The installer finds the release files and prepares recovery automatically. Already installed lights link to their dashboard after an identity check. Discovery is read-only; the backend verifies the target before writing. Advanced setup accepts explicit targets, builds and existing plans.

Use **↑/↓** to choose, **Enter** to continue and **Space** for the installation acknowledgement. Prefer a terminal at least 80 columns wide. **D** shows technical details and the audit path during review or installation. The physical observation prompts start at **No / unsure**; select **Yes** only after seeing the expected output.

The six artifacts in `bundle.json` are checked against their declared hashes and confined relative paths. The Python backend then validates the actual package contents, image and embedded dashboard assets. The verified plan digest is passed back when you explicitly start, preventing an unnoticed plan change between review and execution. A hash establishes integrity, not authorship or electrical qualification; use a reviewed release for the supported hardware.

Automatic lookup checks `bundle.json`, `firmware/bundle.json` (the packaged release layout), `../bundle.json`, then `release/bundle.json`, relative to the repository or selected `--root`. A source checkout also checks the immediate `build/release/*/firmware/bundle.json` paths. Local builds are ranked by semantic version, newest manifest timestamp, then path. The selected bundle and every artifact must validate; a corrupt selection does not silently fall back to an older build. No bundle argument is needed for these layouts. Use `npm run start -- --bundle "/absolute/path/to/bundle.json"` only to select a particular build.

The recovery step downloads the pinned official Razer archive through `tools/vendor_restore.py`, checks it and derives the complete reviewed recovery bank. An exact existing cache is reused. This is a **vendor-derived restore image, not a backup of your device**. Advanced users can supply a complete reviewed local bank and its real provenance.

During installation, progress comes from the backend's completed operations. You must observe and answer both physical checks. After verifying the new application, controller and dashboard assets, the installer pairs once, checks a brief 5% white output and Off through authenticated readbacks, then confirms the application once. If pairing is closed, hold the light's button for three seconds when prompted. Setup then continues automatically. Commands are bound to the current revision, and a reboot stops acceptance. Readbacks are protocol checks, not optical measurements.

If a previous installation left the Open Keylight light engine running behind the stock ESP, open its prepared plan and press **D** at review for the advanced **F · Finish a previous installation** option. Review and start it explicitly: it verifies that exact existing controller, its ownership and Off state, uploads only the ESP, then runs the normal dashboard acceptance checks. Its three stages do not repeat or claim fresh darkness/channel qualification. It refuses unknown firmware, a recovery loader, an expired controller trial or an unverified state. A generic connection failure does not offer this as a recovery shortcut. Each attempt creates a new audit; no failed transfer resumes automatically.

Pairing credentials are stored in a new private local directory: owner-only permissions on POSIX, or an explicit current-user ACL on Windows. Tokens never appear in progress events, URLs or audit records. The final screen shows the dashboard URL; press **D** for the credential file path. A lost pairing or confirmation response is never retried automatically.

**Ctrl+C requests a stop after the current safe stage.** It does not kill the Python process during a write or restart. While the screen says **Stopping safely**, keep the terminal and power connected. A failed or ambiguous operation is never retried automatically. Keep the audit for diagnosis and follow [the recovery matrix](../docs/stock-migration.md).

## Offline inspection and development

Source development needs Node **22 or newer** and Python **3.12 or newer**. These commands work in PowerShell and POSIX shells:

```text
cd installer
npm ci
npm run start
```

Set `OKL_PYTHON` to a Python executable if it is not on `PATH`. To open a prepared plan or supply a development artifact folder:

```text
node --import tsx src/cli.tsx --plan "/path/to/migration.json"
node --import tsx src/cli.tsx --artifacts "/path/to/artifacts"
```

The following checks and previews do not install firmware:

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

To create a PNG directly from that ANSI capture, install Pillow in your development environment and run the following from `installer/` on Windows:

```text
python capture-demo.py --ansi dist/demo/install.ansi --output dist/demo/install.png --font C:/Windows/Fonts/CascadiaMono.ttf --bold-font C:/Windows/Fonts/CascadiaMono.ttf
python tests/test_capture_demo.py
```

Supply installed regular/bold monospace font paths; a variable Cascadia font supplies both weights. The renderer checks glyph coverage, preserves character columns, accepts only the captured colour/weight escapes, and refuses an existing output unless `--overwrite` is explicit. It performs no browser, network or device operations; Pillow is not part of the shipped installer.

The live adapter expects the repository's `--events-jsonl` backend. It rejects unsupported, malformed or out-of-order events and an exit without a verified terminal result. It never scrapes human console messages. The seven stage IDs and cooperative cancellation contract are tested alongside the UI; live hardware qualification is a separate operator task.

The UI uses [Ink 8.0.0](https://github.com/vadimdemedes/ink/releases/tag/v8.0.0), [React](https://react.dev/) and [Ink testing library](https://github.com/vadimdemedes/ink-testing-library), pinned in the lockfile. UI dependencies and original firmware ship in the release. Discovery queries the local network; the pinned official recovery archive is downloaded only during the selected preparation flow.
