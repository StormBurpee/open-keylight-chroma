# Experimental stock migration

`tools/stock_migration.py` provides a guided installer for the reviewed **Razer Key Light Chroma / ESP 1.0.13.0 / NXP 1.3.0.0** profile. It keeps the stock ESP network bridge until the original NXP controller has been installed and confirmed, then installs the original ESP application. The installer is experimental; passing its offline tests does not qualify another physical light.

This page documents the low-level Python workflow for builders and recovery operators. The [guided Windows setup](getting-started.md#install-with-the-guided-windows-setup) supplies the release images and device discovery; the manual commands below require prepared images and an explicit target. Both paths can acquire the exact reviewed stock controller recovery image directly from Razer. Vendor binaries are not distributed in this repository.

Use one explicitly selected private IPv4 address. Close Razer software, integration clients, light-control browser tabs and every other updater first. Another connection to the stock bridge can generate controller traffic even without a command. In particular, traffic during the controller's flash commit can disrupt recovery.

## Identify the target

For the manual Python workflow, record the exact name currently shown in the stock application, then use your router's client/lease list to match that light to its current IP address and ESP MAC. With multiple lights, resolve the mapping before installation; do not select by proximity in a discovery list. The Python backend does not scan for a target. Close the stock application after recording the name.

The expected native device ID is `keylight-` followed by the final six hexadecimal digits of the light's ESP MAC, lowercase and without separators. The stock HELLO owner/routing MAC identifies the connection; it is **not** the light's MAC. A name, version or digest is not cryptographic device authentication.

## Build and package once, then prepare the plan

Use one reviewed source checkout for all artifacts. Run the following from the repository root with Python 3, ARM-capable Clang, lld and llvm-objcopy available. The [controller README](../firmware-nxp/README.md#reproduce-the-software-checks) documents tool selection; the [ESP build instructions](../README.md#build-and-test) require ESP-IDF 5.5.5 and Node.js 22 or newer. These commands build local files and perform no device operations.

```text
python firmware-nxp/tests/run_tests.py
python firmware-nxp/build.py --spi-only-trial --spi-mode 3
python firmware-nxp/build.py --pwm-off-trial --spi-mode 3
python firmware-nxp/build.py --pwm-low-trial --spi-mode 3
python firmware-nxp/build.py --reference-lighting
```

Each directory below also contains `manifest.json`. Check its `firmware_version`, source pins and bank digest before packaging. The versions shown are the current source's values; if they change, use the reviewed build's manifest rather than relabelling an old bank.

| Installer stage | Complete bank under `firmware-nxp/build/` | Package role | Current version |
| --- | --- | --- | --- |
| Identity | `spi-trial-mode3/spi-trial-bank.bin` | `diagnostic` | `0.1.0.0` |
| OFF1 | `pwm-off-trial/pwm-off-trial-bank.bin` | `diagnostic` | `0.1.0.0` |
| LOW1 | `pwm-low-trial/pwm-low-trial-bank.bin` | `diagnostic` | `0.1.0.0` |
| Lighting | `lighting/lighting-bank.bin` | `lighting` | `0.1.1.0` |

Create a local working directory and package all four banks. `private/` is ignored by this repository. Package outputs must be new; these commands deliberately do not use `--overwrite`.

```text
python -c "from pathlib import Path; Path('private/migration').mkdir(parents=True, exist_ok=True)"
python tools/package_controller.py firmware-nxp/build/spi-trial-mode3/spi-trial-bank.bin private/migration/identity.oklnxp --version 0.1.0.0 --role diagnostic
python tools/package_controller.py firmware-nxp/build/pwm-off-trial/pwm-off-trial-bank.bin private/migration/off.oklnxp --version 0.1.0.0 --role diagnostic
python tools/package_controller.py firmware-nxp/build/pwm-low-trial/pwm-low-trial-bank.bin private/migration/low.oklnxp --version 0.1.0.0 --role diagnostic
python tools/package_controller.py firmware-nxp/build/lighting/lighting-bank.bin private/migration/lighting.oklnxp --version 0.1.1.0 --role lighting
```

Do not package the default inert controller image or rename a diagnostic package into a lighting package. For the compiled diagnostic/lifecycle checks, use the emulator commands in the [OFF1](../firmware-nxp/OFF-TRIAL.md), [LOW1](../firmware-nxp/LOW-TRIAL.md#reproducible-offline-checks) and [lighting](../firmware-nxp/PRODUCTION.md) guides.

Build the dashboard before the ESP application, in the configured ESP-IDF environment:

```text
cd dashboard
npm ci
npm test -- --run
npm run build
cd ../firmware
idf.py build
cd ..
```

The matching migration inputs are `firmware/build/open_keylight.bin` and `dashboard/dist/asset-manifest.json`. A [development artifact archive](getting-started.md#build-or-explore-without-installing) from the intended commit can supply that pair instead. Use only the application image, never `idf.py flash`, a merged image, bootloader or partition table. Keep the matching ELF and metadata for diagnosis.

Acquire the reviewed stock recovery image:

```text
python tools/vendor_restore.py --output private/migration/stock-nxp-1.3.0.bin
```

The helper downloads one pinned HTTPS archive from Razer, validates its version and Intel HEX checksums, and reconstructs the reviewed 28 KiB bank. Its erased tail and complete digest match the reference staging capture exactly. An existing matching cache is reused; a different file is never overwritten. Use the returned `path`, `version` and `provenance` in the plan below. This is a recovery image, **not a backup of your light** or proof that restoration works on another board. No light is contacted by this helper.

An independently reviewed owner-local bank remains usable instead. Preserve its origin, exact digest, modifications and live restore evidence. A logical Read83 staging capture is not an independent active-application backup; a patched bank must not be labelled “factory.”

Create the plan with the offline helper. Replace the uppercase placeholders below; quote paths containing spaces. This PowerShell example uses an argument array, so it does not depend on fragile line-continuation characters:

```powershell
$migrationArgs = @(
  '--output', 'private/migration/migration.json',
  '--target-ip', 'LIGHT_IP', '--target-name', 'EXACT_STOCK_NAME',
  '--device-id', 'keylight-MAC_SUFFIX', '--source-commit', 'FULL_REVIEWED_COMMIT',
  '--identity', 'private/migration/identity.oklnxp', '--off1', 'private/migration/off.oklnxp',
  '--low1', 'private/migration/low.oklnxp', '--lighting', 'private/migration/lighting.oklnxp',
  '--esp', 'firmware/build/open_keylight.bin', '--assets', 'dashboard/dist/asset-manifest.json',
  '--restore', 'OWNER_LOCAL_RESTORE_PATH', '--restore-version', '1.3.0.0',
  '--restore-provenance', 'DESCRIBE_ORIGIN_TAIL_MODIFICATIONS_AND_RESTORE_EVIDENCE'
)
python tools/prepare_migration.py @migrationArgs
```

For a POSIX shell, use the same flags on one command:

```sh
python tools/prepare_migration.py --output private/migration/migration.json --target-ip LIGHT_IP --target-name "EXACT_STOCK_NAME" --device-id keylight-MAC_SUFFIX --source-commit FULL_REVIEWED_COMMIT --identity private/migration/identity.oklnxp --off1 private/migration/off.oklnxp --low1 private/migration/low.oklnxp --lighting private/migration/lighting.oklnxp --esp firmware/build/open_keylight.bin --assets dashboard/dist/asset-manifest.json --restore "OWNER_LOCAL_RESTORE_PATH" --restore-version 1.3.0.0 --restore-provenance "DESCRIBE_ORIGIN_TAIL_MODIFICATIONS_AND_RESTORE_EVIDENCE"
```

The helper validates every file before publishing a complete plan. It never contacts the light, copies firmware payloads or replaces an existing output. It publishes atomically on filesystems supporting hard links; other filesystems fail without leaving a plan. Paths in the plan are relative to its directory where possible. Keep the plan and audit private because they contain local paths and target details. Preparation does not establish hardware qualification or authenticate a publisher.

Inspect the printed target and artifact summary before proceeding. Recheck an existing plan with this single-line command, valid in PowerShell and POSIX shells:

```text
python tools/stock_migration.py prepare --manifest private/migration/migration.json
```

Prepare performs no network operations. It checks package layout, vectors, SHA-256, roles, ESP image checksums/memory bounds, and exact embedded dashboard assets. A source-commit field records the operator's reviewed provenance; it does not authenticate an arbitrary binary.

## Run the guided installation

```text
python tools/stock_migration.py install --manifest private/migration/migration.json --audit private/migration/migration-attempt-1.jsonl --execute --exclusive-control
```

The audit path must be new. The installer records durable intent before sending each mutation, hashes rather than firmware payloads, and never records browser tokens or Wi-Fi passwords. Stay within sight of the light for OFF1 and LOW1, with a browser ready for the later acceptance step. The current CLI does not print transfer progress: a quiet console is expected while it writes and verifies banks and waits for 30-second diagnostic recovery. Do not launch another controller to check progress; the local audit continues recording events.

The stages are:

1. Match the selected stock name, ESP version and controller version. Claim the connection and verify native Off settings.
2. Enter the resident controller loader using the stock updater's fixed two-frame sequence, inspect its exact information and code fingerprint, and install the reviewed SPI-only identity trial. Read its actual ROM-IAP part ID through FE and its typed FC status. No PWM diagnostic or production image is admitted before `0x0000bc40` is observed.
3. Wait for that trial's reviewed 30-second recovery path. Install OFF1, trigger its single bounded all-low experiment, verify the fixed register record and wait for resident recovery. Only then ask whether the light stayed dark.
4. Install LOW1, trigger its five fixed low pulses, leave the bus silent during the sequence, verify the fixed record and wait for resident recovery. Only then ask about the observed red, green, blue, warm-white and cool-white pulses. A rejected, cancelled or unanswered prompt stops the installer.
5. Install the lighting package, require fresh original identity and Off readback, issue one controller confirmation and verify it with another FC read. Diagnostics never receive FD.
6. Install the original ESP application through the retained stock bridge. A final 100% notification and clean socket close mean that the stock application accepted the update; they do not prove the new application booted.
7. Observe the exact native device ID, ELF digest, version, original-controller readiness and every served dashboard asset. Open the printed URL and pair the browser while its initial pairing window is open. If that window is closed, hold the physical button for three seconds to reopen it. Check controls at low brightness, return to Off, and explicitly confirm the trial in the dashboard. The CLI only observes public status. It neither obtains a bearer token nor confirms for the user.

The original ESP has an application-level trial lasting 180 seconds **from application startup**, not from the printed browser prompt. Boot and asset verification consume some of that window. Its fallback requires the new application to boot and run its recovery logic; this is not a guarantee that the old hardware bootloader recovers an image that cannot start. Do not leave the acceptance stage unattended.

### Stock recovery entry

On the existing TCP connection, the installer sends `00/04 = 01 00`, waits 100 ms, sends the same frame, then waits another 100 ms. This is the fixed sequence recovered from the stock updater, not a retry selected after a timeout. The stock application resets on the first processed frame; class 00 is a no-op in the resident loader. An ESP-local version getter then establishes that the synchronous bridge queue has drained. Only exact loader information and the complete resident code fingerprint admit the first image operation.

The earlier single-frame entry timed out at its first loader-information request on the restored reference light. Replaying the captured loader instructions shows that two residual SPI bytes can produce a zero-length response to that request, and that the stock two-frame sequence consumes this condition before the information request. The physical reset timing was not measured, so this remains a supported explanation, not a proven diagnosis of the live timeout. Any unresolved entry still stops before image erase, program or commit and records its failing phase.

### Why a raw ESP partition capture is optional

The reviewed stock updater selects the next existing OTA partition using its installed table, verifies that partition object, refuses the running partition, bounds writes against the selected partition's size, and validates the image before changing boot selection. The host sends no flash address. The installer caps images at the reference 1.5 MiB envelope; a smaller device-selected slot must reject an oversized image before selection.

The profile match is an explicit assumption about the installed stock build, supported by static inspection and reference-board experiments. It does not measure each light's flash size or attest its bootloader/eFuses. Unknown first-boot compatibility remains part of this experimental migration. An available owner-bound partition capture can supply additional evidence, but serial access or an otherwise inaccessible raw-table read is not invented as a prerequisite.

## If a stage stops

Keep the audit and stop other controller traffic. No mutation is retried, no old journal is resumed, and no restore image is sent automatically. After an uncertain controller End, the connection remains open and silent for at least three seconds before it may close. Before End, only a synchronized session may issue one Abort; ambiguous transport outcomes fail closed.

Use the last recorded stage and its outcome to choose the next step. A stage's intent record alone does not prove completion.

| Last established outcome | What to do next |
| --- | --- |
| Preparation or identity check failed before a mutation | Correct the artifact/target mismatch. A later attempt needs a new audit. Do not infer this case from a timeout after a mutation was sent. |
| A controller transfer or diagnostic stopped while the stock ESP is still installed | Preserve the full audit. The explicit cold-recovery restore below is available only if the exact resident loader responds; it is not a generic restart/resume command. |
| Original lighting controller installed, ESP replacement not yet accepted | Choose **Finish a previous installation** in the TUI, or the explicit `finish` command below. It verifies a running original controller, ownership and Off state, then uploads only the ESP. It refuses a resident loader or unknown state. The ordinary `install` command still expects the stock controller. |
| New original ESP observed with a pending trial | While the TUI is running, follow its button-hold prompt and let automatic acceptance finish; do not pair a browser concurrently. With the direct Python CLI, or after the TUI has fully stopped, use the dashboard to pair, check low-brightness controls, return to Off and confirm within the remaining trial time. If checks fail, leave the trial unconfirmed and inspect the actual fallback result. |
| ESP upload/boot outcome uncertain, or an unconfirmed ESP fell back | Identify which ESP application is actually running before choosing any tool. The original NXP controller may remain installed after ESP fallback. Neither the original stock starting state nor a responding resident loader can be assumed. |
| Original ESP confirmed, controller journal blocks lighting | Use the authenticated native recovery contract linked below. A reboot does not clear the journal, and the stock TCP restore tool is no longer the appropriate interface. |

There is no automatic path from every intermediate state back to factory firmware. If the required resident or application identity cannot be established, stop with the audit intact rather than trying another entry, commit or restore command.

To finish an installation with the stock ESP and an already installed original lighting application, use a new audit:

```text
python tools/stock_migration.py finish --manifest private/migration/migration.json --audit private/migration/finish-1.jsonl --execute --exclusive-control
```

This separate three-stage path verifies the target's MAC suffix, stock ESP name/version and original controller version, ROM part and typed status. It claims the real SPI owner using a fresh nonce and verifies ownership and Off through independent getters. An already confirmed controller is not confirmed again; an unconfirmed one must be younger than 15 seconds, with confirmation and readback before a conservative 20-second deadline. A change of controller boot or ambiguous response stops the operation. The installed controller bank is retained, with no new claim of optical or electrical qualification. ESP upload and independent native acceptance use the same checks as initial installation.

For an explicit controller restore while the stock ESP bridge remains installed, unplug and reconnect the **whole light**, wait at least 35 seconds, and use a new audit:

```text
python tools/stock_migration.py restore --manifest private/migration/migration.json --audit private/migration/explicit-restore-1.jsonl --execute --exclusive-control --power-cycled
```

The power-cycle acknowledgment supplies a physical reset premise that a TCP connection or Info80 cannot prove. Restore requires the exact resident loader to be responding and its fingerprint to match; it does not reset an arbitrary active application or guess from a timeout. It writes the owner-reviewed bank once and reports the observed application version. Independently check normal operation afterwards.

This restore command does not target the original ESP HTTP API after ESP replacement. Use the native dashboard's documented OTA/trial/recovery flow there. A controller image that fails before its recovery routine runs can still need physical intervention; preserving the resident loader does not guarantee every failed image is remotely recoverable.

## Offline verification

```sh
python -m unittest discover -s tests/migration -p "test_*.py"
```

Tests forbid real sockets. They exercise truncated/coalesced frames, captured loader-only HELLO, deadlines including delayed audit writes, all controller transfer reply-loss positions, uncertain End handling, diagnostic profile/timing gates, explicit restoration and concurrent ESP notifications. Profile tests compare Python against the actual C predicates, including every single-bit mutation of valid OFF1/LOW1 records. These checks validate software behavior, not light output or electrical limits.
