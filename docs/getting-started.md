# Getting started

Open Keylight Chroma is a development preview for a qualified reference board. Both original MCU applications have run on that light, including native update and recovery tests. This is not yet a general installation release. High-output colour caused ESP brownouts during qualification; the cause and sustained-output reliability remain under investigation. An ESP update with the original controller also exposed a flash/SPI coordination fault; its correction must pass live qualification before release.

## Choose the appropriate path

| Starting point | Current path |
| --- | --- |
| Explore the interface without a light | Run the explicitly labelled [dashboard demo](dashboard.md#build-and-preview). It sends no device requests. |
| Build or contribute | Follow the [README build steps](../README.md#build-and-test) and automated tests. Building does not establish hardware compatibility. |
| A factory-firmware light | Stock migration is pending. There is no qualified public one-command installer. Preserve the factory installation until a procedure for the actual board is published. |
| An already qualified Open Keylight light | Use its embedded dashboard and authenticated application-only update flow below. Keep its known-good image and qualification record. |

The ESP image, NXP application bank and controller update package are different formats. Never upload a bootloader, partition table, merged flash image, raw NXP bank or diagnostic image through the ESP firmware control. `idf.py flash` is not the existing-light migration procedure. The NXP default build is deliberately inert and must not be installed; [reference lighting](../firmware-nxp/PRODUCTION.md) and diagnostic profiles have separate acceptance requirements.

## Connect to an existing installation

Open the light's current local IP address in a browser. The dashboard is served by the light on port 80; no companion desktop server is needed. Find the address in your router if local hostname discovery is unavailable.

To authorize a browser, hold the physical button for three seconds, open **Connect access**, give the client a recognizable name and choose **Pair this browser**. The pairing window lasts up to 180 seconds and closes after one client pairs. An already trusted client can instead use **System → Open pairing for another client**. Tokens are scoped to clients and the browser keeps its token in tab session storage; closing that session may require pairing again. Keep the device on a trusted LAN: HTTP bearer tokens do not encrypt traffic.

System provides Wi-Fi setup, client revocation and integration settings. A Wi-Fi change is an explicit **Save Wi-Fi & restart light** action; it may change the address. Passwords are never returned by the settings API. An installation without configured Wi-Fi exposes `Keylight-Setup-<suffix>` at `http://192.168.4.1`. If configured Wi-Fi is unavailable, a three-second button hold also requests a temporary setup network. Pairing is still required to change settings.

If all access tokens are lost, an uninterrupted button hold beginning at boot and lasting ten seconds revokes existing clients and reopens pairing. It preserves Wi-Fi and scenes; it is not a factory erase or controller-journal reset. Use it deliberately, since other clients will lose access.

## Development artifacts

Download artifacts only from the intended commit's successful **Build and verify** run. The workflow does not create GitHub Releases or designate a hardware-qualified version. The ESP archive contains:

- `open_keylight.bin`: standalone ESP application, including the dashboard.
- `open_keylight.elf`: matching symbols and build identity, for diagnosis; do not upload it.
- `manifest.json`: source commit, descriptor version, image and ELF digests, slot size and explicit unqualified status.
- `project_description.json` and `asset-manifest.json`: build and embedded-asset metadata.
- `SHA256SUMS`: checksums for the files above.

Run `sha256sum --check SHA256SUMS` after extraction on Linux, or compare `Get-FileHash .\open_keylight.bin -Algorithm SHA256` with its entry on Windows. Verify the workflow commit and expected image digest independently of the selected local file. These hashes detect mismatches; they are not firmware signatures or proof that a build is safe for another board. Firmware `VERSION`, NXP application version and Stream Deck plugin version are independent.

The separately packaged Stream Deck plugin is described in [its installation guide](../integrations/streamdeck/README.md); [Home Assistant setup](home-assistant.md) uses MQTT. Neither integration installs firmware on a factory light.

## Update an already qualified ESP installation

1. Keep the previous known-good application and its digest. Check System's current firmware/controller health, paired access and stable network. Resolve any pending ESP trial first. Stop other controllers and set the light Off.
2. In System, select the standalone `open_keylight.bin` and enter its independently checked SHA-256. Choose **Verify & update** once. The device writes the inactive application slot; it preserves the installed bootloader, partition table, settings, tokens and scenes.
3. Reconnect after restart. Check the expected firmware version **and** `firmware_elf_sha256` in `GET /api/v1/device`, since version labels can repeat. Check the dashboard, settings/scenes, controller health, low-level controls and fresh reported fields, then return to Off. A successful upload response is not a successful reboot or optical verification.
4. Within the 180-second application trial, explicitly choose **Confirm this firmware** only after those checks. Confirming the ESP does not independently qualify or install the NXP application.

If a request times out, inspect the current device and preserve the audit before another action. Do not blindly repeat an upload or confirmation: a response can be lost after a successful operation. A later upload can replace the previous recovery image.

## Fallback and controller recovery

Leave an unsuccessful ESP trial unconfirmed. If the new application is running sufficiently to service its trial task, it selects the other application slot and restarts after 180 seconds. This is **application-level fallback**. The retained ESP bootloader has no automatic crash rollback: an invalid startup, repeated early crash or lost power can prevent that task from running. Do not assume a power cycle repairs an unbootable image. Recovering that case can require independently prepared serial access and a verified image/layout; no general serial recovery recipe is qualified here.

Confirmation ends the automatic trial. There is no general dashboard rollback button. Reinstalling an older known-good image is a separate explicit OTA operation and requires compatible stored data; it is not a reason to erase NVS.

The NXP controller has its own updater and durable failure journal. A failed controller update can leave the ESP dashboard reachable while normal lighting stays disabled. ESP rollback or reboot does not clear that journal. Preserve the cached job and transport diagnostics. The authenticated recovery operation requires the exact job ID, a confirmed ESP application, an acknowledged **whole-light physical power cycle**, and the documented fresh-boot conditions; it does not erase the journal or resume an interrupted transfer. A resident recovery result permits a new explicit package upload. See [the exact recovery contract](controller-update-journal.md#explicit-recovery-after-physical-power-cycle).

Public stock migration, broad board compatibility, sustained maximum-output/current/thermal testing and recovery from arbitrary early-boot failure remain release gates. Host tests and development artifacts are evidence for software behavior, not substitutes for those observations.
