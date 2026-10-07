# Versions and releases

An ESP application, an NXP application and a Stream Deck plugin can change independently. A release must identify all three; a matching version label alone is not a matching binary.

| Component | Version source | Runtime identity |
| --- | --- | --- |
| ESP and embedded dashboard | Root `VERSION` | `/api/v1/device`: `firmware` and `firmware_elf_sha256` |
| NXP lighting application | `firmware-nxp/src/nxp_app.c` and the checked build manifest | Controller version, part ID and capability/status ABI |
| Stream Deck plugin | Its `package.json` and generated plugin manifest | Installed plugin version |

ESP development builds use `-dev`. Published versions use semantic versioning; protocol compatibility is separate from the product version. API v1 additions must preserve existing fields and behavior. A breaking public API needs a new API version and a migration path. NXP protocol changes must negotiate their own capability or descriptor; older clients cannot be expected to accept unknown status bits.

## Prepare a candidate

1. Commit the complete change and update its version. Document the problem, observable behavior, compatibility and recovery implications.
2. Run the affected tests, the complete CI workflow and compiled-controller regressions. Build the dashboard before the ESP application so the asset manifest describes the embedded files.
3. Package the ESP with `tools/package_esp.py`. Retain its application, matching ELF, full source commit, asset manifest and checksums. Package NXP applications separately with `tools/package_controller.py`; never substitute a raw bank for an update package.
4. Qualify those exact images on the supported board. Record starting firmware, running ELF, controller identity, saved-data checks, transitions, update behavior, restart/recovery outcomes and remaining limitations. Preserve failed attempts alongside successes. Rebuilding an image creates a different candidate even when the source version is unchanged.
5. For a public installation release, exercise the documented factory-to-original path from an untouched supported light, then an original-to-original upgrade. Check that a new user can obtain the files and follow the published instructions without private development tools or credentials.

Tag only the reviewed source commit, and publish the exact qualified files with their checksums and a concise compatibility/recovery record. Never replace files under an existing tag. A correction gets a new version. CI artifacts are development candidates until these observations exist; the current project has no qualified public installation release.

## Keep recovery usable

Retain the preceding known-good files and symbols. Describe persistent-data changes and whether an older application can still read them. Application-only updates preserve the resident bootloaders and partition table. An A/B layout does not imply that the installed bootloader can recover an early crash, and a completed ESP upload does not establish NXP readiness.

Do not put vendor firmware, credentials, private captures or owner-specific network information in a release. A locally supplied recovery artifact stays with its owner.
