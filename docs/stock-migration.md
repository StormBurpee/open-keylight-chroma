# Experimental stock migration

`tools/stock_migration.py` provides a guided installer for the reviewed **Razer Key Light Chroma / ESP 1.0.13.0 / NXP 1.3.0.0** profile. It keeps the stock ESP network bridge until the original NXP controller has been installed and confirmed, then installs the original ESP application. The installer is experimental; passing its offline tests does not qualify another physical light.

This is currently a builder/operator workflow. Preparing reviewed original images and a legitimate owner-local restore bank is still required; a generally distributable stock-migration bundle and restore-source acquisition workflow are not yet provided.

Use one explicitly selected private IPv4 address. Close Razer software, integration clients, light-control browser tabs and every other updater first. Another connection to the stock bridge can generate controller traffic even without a command. In particular, traffic during the controller's flash commit can disrupt recovery.

## Prepare the artifacts

The installer requires four **original-source** controller packages: the SPI-only identity trial, OFF1, LOW1, and the reference lighting build. Build the distinct profiles described in [the controller README](../firmware-nxp/README.md), then package each complete bank with [package_controller.py](../tools/package_controller.py). Diagnostics use `--role diagnostic`; lighting uses the default lighting role. Use the version from each build's manifest. Do not rename a diagnostic package into a lighting package.

It also requires the standalone ESP application, its dashboard `asset-manifest.json`, and an owner-local, independently reviewed 28 KiB restore bank. Vendor binaries are not distributed here. A logical Read83 staging capture is **not** an independent active-application backup. Preserve the restore bank's origin, exact digest, known modifications, and any live restore evidence; do not label a patched bank “factory.”

Create the local manifest with the offline helper. Supply the light's exact current name and the device ID derived from its ESP MAC, plus the reviewed source commit and artifacts:

```sh
python tools/prepare_migration.py --output migration.json \
  --target-ip 192.168.1.50 --target-name "My Key Light" --device-id keylight-abcdef \
  --source-commit <full-reviewed-commit> \
  --identity identity.oklnxp --off1 off.oklnxp --low1 low.oklnxp --lighting lighting.oklnxp \
  --esp open_keylight.bin --assets asset-manifest.json \
  --restore owner-local-restore.bin --restore-version 1.3.0.0 \
  --restore-provenance "Describe the source, preserved tail, modifications and restore evidence."
```

The helper validates all files using the installer's checks before publishing a complete plan. It never contacts the light, copies firmware payloads, or replaces an existing output. It publishes atomically on filesystems supporting hard links; other filesystems fail without leaving a plan. Preparation does not establish hardware qualification or authenticate a publisher.

The resulting format is shown below for inspection or manual preparation. Paths are relative to the manifest (absolute local paths also work). Replace every placeholder with the exact reviewed value; the installer checks every digest before contacting the light.

```json
{
  "format": 1,
  "profile": "keylight-chroma-1.0.13",
  "source_commit": "<40 lowercase hex characters>",
  "target": {
    "ip": "192.168.1.50",
    "name": "My Key Light",
    "device_id": "keylight-abcdef"
  },
  "packages": {
    "identity": {"path": "identity.oklnxp", "sha256": "<sha256>"},
    "OFF1": {"path": "off.oklnxp", "sha256": "<sha256>"},
    "LOW1": {"path": "low.oklnxp", "sha256": "<sha256>"},
    "lighting": {"path": "lighting.oklnxp", "sha256": "<sha256>"}
  },
  "restore": {
    "path": "owner-local-restore.bin",
    "sha256": "<sha256>",
    "version": "1.3.0.0",
    "provenance": "Describe the original source, preserved tail, modifications and restore evidence here."
  },
  "esp": {"path": "open_keylight.bin", "sha256": "<sha256>"},
  "assets": {"path": "asset-manifest.json", "sha256": "<sha256>"}
}
```

The expected native device ID is `keylight-` followed by the final six hexadecimal digits of the light's ESP MAC address. The stock HELLO owner/routing MAC identifies the connection; it is **not** the light's MAC. A name, version or digest is not cryptographic device authentication.

```sh
python tools/stock_migration.py prepare --manifest migration.json
```

Prepare performs no network operations. It checks package layout, vectors, SHA-256, roles, ESP image checksums/memory bounds, and exact embedded dashboard assets. A source-commit field records the operator's reviewed provenance; it does not authenticate an arbitrary binary.

## Run the guided installation

```sh
python tools/stock_migration.py install --manifest migration.json \
  --audit migration-attempt-1.jsonl --execute --exclusive-control
```

The audit path must be new. The installer records durable intent before sending each mutation, hashes rather than firmware payloads, and never records browser tokens or Wi-Fi passwords.

The stages are:

1. Match the selected stock name, ESP version and controller version. Claim the connection and verify native Off settings.
2. Enter the resident controller loader once, inspect its exact information and code fingerprint, and install the reviewed SPI-only identity trial. Read its actual ROM-IAP part ID through FE and its typed FC status. No PWM diagnostic or production image is admitted before `0x0000bc40` is observed.
3. Wait for that trial's reviewed 30-second recovery path. Install OFF1, trigger its single bounded all-low experiment, verify the fixed register record and wait for resident recovery. Only then ask whether the light stayed dark.
4. Install LOW1, trigger its five fixed low pulses, leave the bus silent during the sequence, verify the fixed record and wait for resident recovery. Only then ask about the observed red, green, blue, warm-white and cool-white pulses. A rejected, cancelled or unanswered prompt stops the installer.
5. Install the lighting package, require fresh original identity and Off readback, issue one controller confirmation and verify it with another FC read. Diagnostics never receive FD.
6. Install the original ESP application through the retained stock bridge. A final 100% notification and clean socket close mean that the stock application accepted the update; they do not prove the new application booted.
7. Observe the exact native device ID, ELF digest, version, original-controller readiness and every served dashboard asset. Open the printed URL and pair the browser while its initial pairing window is open. If that window is closed, hold the physical button for three seconds to reopen it. Check controls at low brightness, return to Off, and explicitly confirm the trial in the dashboard. The CLI only observes public status. It neither obtains a bearer token nor confirms for the user.

The original ESP has an application-level trial lasting 180 seconds. Its fallback requires the new application to boot and run its recovery logic; this is not a guarantee that the old hardware bootloader recovers an image that cannot start. Do not leave the acceptance stage unattended.

### Why a raw ESP partition capture is optional

The reviewed stock updater selects the next existing OTA partition using its installed table, verifies that partition object, refuses the running partition, bounds writes against the selected partition's size, and validates the image before changing boot selection. The host sends no flash address. The installer caps images at the reference 1.5 MiB envelope; a smaller device-selected slot must reject an oversized image before selection.

The profile match is an explicit assumption about the installed stock build, supported by static inspection and reference-board experiments. It does not measure each light's flash size or attest its bootloader/eFuses. Unknown first-boot compatibility remains part of this experimental migration. An available owner-bound partition capture can supply additional evidence, but serial access or an otherwise inaccessible raw-table read is not invented as a prerequisite.

## If a stage stops

Keep the audit and stop other controller traffic. No mutation is retried, no old journal is resumed, and no restore image is sent automatically. After an uncertain controller End, the connection remains open and silent for at least three seconds before it may close. Before End, only a synchronized session may issue one Abort; ambiguous transport outcomes fail closed.

For an explicit controller restore while the stock ESP bridge remains installed, unplug and reconnect the **whole light**, wait at least 35 seconds, and use a new audit:

```sh
python tools/stock_migration.py restore --manifest migration.json \
  --audit explicit-restore-1.jsonl --execute --exclusive-control --power-cycled
```

The power-cycle acknowledgment supplies a physical reset premise that a TCP connection or Info80 cannot prove. Restore requires the exact resident loader to be responding and its fingerprint to match; it does not reset an arbitrary active application or guess from a timeout. It writes the owner-reviewed bank once and reports the observed application version. Independently check normal operation afterwards.

This restore command does not target the original ESP HTTP API after ESP replacement. Use the native dashboard's documented OTA/trial/recovery flow there. A controller image that fails before its recovery routine runs can still need physical intervention; preserving the resident loader does not guarantee every failed image is remotely recoverable.

## Offline verification

```sh
python -m unittest discover -s tests/migration -p "test_*.py"
```

Tests forbid real sockets. They exercise truncated/coalesced frames, captured loader-only HELLO, deadlines including delayed audit writes, all controller transfer reply-loss positions, uncertain End handling, diagnostic profile/timing gates, explicit restoration and concurrent ESP notifications. Profile tests compare Python against the actual C predicates, including every single-bit mutation of valid OFF1/LOW1 records. These checks validate software behavior, not light output or electrical limits.
