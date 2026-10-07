# Versions and releases

An ESP application, an NXP application and a Stream Deck plugin can change independently. A release must identify all three; a matching version label alone is not a matching binary.

| Component | Version source | Runtime identity |
| --- | --- | --- |
| ESP and embedded dashboard | Root `VERSION` | `/api/v1/device`: `firmware` and `firmware_elf_sha256` |
| NXP lighting application | `firmware-nxp/src/nxp_app.c` and the checked build manifest | Controller version, part ID and capability/status ABI |
| Stream Deck plugin | Its `package.json` and generated plugin manifest | Installed plugin version |

ESP development builds use `-dev`. Published versions use semantic versioning; protocol compatibility is separate from the product version. API v1 additions must preserve existing fields and behavior. A breaking public API needs a new API version and a migration path. NXP protocol changes must negotiate their own capability or descriptor; older clients cannot be expected to accept unknown status bits.

The first guided-installer candidate is **0.2.0-alpha.1**. `alpha.1` is a SemVer prerelease, not a stable support promise. The release ZIP's product version, root `VERSION`, installer package version and ESP descriptor must agree; NXP and Stream Deck keep their independent component versions.

## Assemble the guided installer

Build and test `installer/` with `npm ci`, `npm test`, `npm run check`, `npm run bundle` and `npm run test:bundle`. The bundle includes its runtime JavaScript, Yoga WASM and dependency notices; it must not require `node_modules` at installation time.

`tools/package_installer.py` accepts the four separately reviewed controller packages, standalone ESP application and matching dashboard asset manifest. Pass the full source commit and `VERSION`; it rejects relabelled images, mismatched embedded assets and existing output directories. It produces `bundle.json` and six original firmware artifacts. No stock recovery image belongs in that directory.

`tools/package_release.py --bundle <firmware-bundle> --installer installer/dist --output <new-release.zip>` assembles the Windows x64 download. Its fixed allowlist includes the firmware bundle, built TUI, Python helpers, portable-runtime launcher, license and illustrated getting-started guide. It adds `release.json` and `SHA256SUMS`, uses deterministic ZIP metadata and refuses to overwrite an existing file. Packaging checks consistency; it does not designate an image as hardware-qualified.

Extract the ZIP into a new directory and test `start-open-keylight.cmd` there, outside the checkout and without project dependencies. The launcher downloads pinned Node/Python archives from their official publishers, verifies size and SHA-256, and extracts fresh session runtimes with bounded writes. It preserves the user's PATH and existing runtime installations. Test the bundled Python flow too: embedded Python's isolated import path differs from a normal installation.

Publish the exact tested ZIP, standalone ESP application and matching symbols, controller package, Stream Deck plugin and checksums as appropriate. Keep release notes concise: supported starting profile, install path, component versions, observations and known recovery limits. Mark prereleases as prereleases. Never substitute a fresh CI rebuild for a previously tested image just because its version matches.

## Prepare a candidate

1. Commit the complete change and update its version. Document the problem, observable behavior, compatibility and recovery implications.
2. Run the affected tests, the complete CI workflow and compiled-controller regressions. Build the dashboard before the ESP application so the asset manifest describes the embedded files.
3. Package the ESP with `tools/package_esp.py`. Retain its application, matching ELF, full source commit, asset manifest and checksums. Package NXP applications separately with `tools/package_controller.py`; never substitute a raw bank for an update package.
4. Qualify those exact images on the supported board. Record starting firmware, running ELF, controller identity, saved-data checks, transitions, update behavior, restart/recovery outcomes and remaining limitations. Preserve failed attempts alongside successes. Rebuilding an image creates a different candidate even when the source version is unchanged.
5. For a public installation release, exercise the documented stock-to-original path from the supported stock application pair, then an original-to-original upgrade. Record whether the test light was untouched or restored, including any retained Open Keylight settings and credentials; a restored fixture is not factory-fresh evidence. Check that a new user can obtain the files and follow the published instructions without private development tools or credentials. Returning a working light to stock is a separate, unqualified operation until its boot and saved-data compatibility have been demonstrated.

Tag only the reviewed source commit, and publish the exact qualified files with their checksums and a concise compatibility/recovery record. Never replace files under an existing tag. A correction gets a new version. CI artifacts are development candidates until these observations exist; the current project has no qualified public installation release.

## Keep recovery usable

Retain the preceding known-good files and symbols. Describe persistent-data changes and whether an older application can still read them. Application-only updates preserve the resident bootloaders and partition table. An A/B layout does not imply that the installed bootloader can recover an early crash, and a completed ESP upload does not establish NXP readiness.

Do not put vendor firmware, credentials, private captures or owner-specific network information in a release. The installer obtains the reviewed recovery image directly from its original publisher and keeps it in the operator's local cache. A locally supplied recovery artifact also stays with its owner.
