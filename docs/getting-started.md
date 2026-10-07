# Getting started

Open Keylight runs on the light itself. You use a computer for the initial installation; afterward, a browser is enough.

**Installation status:** the first release is still being qualified. Both original applications have run on the reference lights, but the complete public installer path has not yet passed its final live check. Use the [release record](../PROGRESS.md) to distinguish a tested release from a development build.

## Before you begin

You need a Razer Key Light Chroma already connected to Wi-Fi, a computer on the same local network, its original power supply, and a few minutes beside the light. Close Synapse, the Razer app and other controllers during installation.

The initial stock profile is **ESP 1.0.13.0 with NXP 1.3.0.0**, using the reviewed LPC11U35/501 controller and resident loader. The installer checks live identity and the loader before continuing. A similar product name or firmware number does not establish support for a different board.

Initial Wi-Fi provisioning of a factory-new light is outside this installer. Once Open Keylight is installed, its dashboard handles Wi-Fi configuration without Razer software.

If an earlier installation already put Open Keylight on the lighting controller but left the stock ESP, open its prepared plan and press **D** at review to reveal **F · Finish a previous installation**. This verifies and retains the installed light engine while finishing the dashboard application. It cannot recover an unresponsive controller or resume an incomplete flash; a connection timeout alone is not a reason to choose it.

## Install with the guided Windows setup

The release package is designed for **Windows 10/11 x64**. It includes original firmware and the React / Ink installer. The launcher downloads verified portable Node and Python runtimes on first use—about 47 MB—without installing them globally or changing PATH.

1. Download the Windows installer ZIP from the project's [Releases](https://github.com/StormBurpee/open-keylight-chroma/releases) page. Read that release's supported hardware and qualification record.
2. Choose **Extract all**. Open **start-open-keylight.cmd** from the extracted folder.
3. Choose **Find my light**, then select it by name and address. If discovery finds nothing, check power and the local network, then choose **Look again**. Guest-network isolation or a VPN can prevent discovery. **Enter stock address manually** opens advanced setup; your router can supply the address.
4. Let the installer prepare the release files and exact stock recovery image automatically, then review the selected light and release. The recovery image comes from Razer's official HTTPS server and is verified locally. You do not need to find or extract a firmware backup.
5. Close other light controls. Press **Space** to acknowledge that you can watch the light, then **Enter** to start. Keep the terminal and light powered. Follow the two visual checks: complete darkness, then red, green, blue, warm white and cool white with darkness between them. Use the arrow keys to choose **Yes** only when the observation matches, then press **Enter**.
6. The installer replaces the ESP application last, checks the actual image and dashboard assets, pairs its client, briefly checks 5% white and Off, and confirms the new application. If it asks you to pair, hold the light's button continuously for **three seconds**, then release. Setup continues automatically; you do not need to copy a token or confirm in the browser during this step.
7. Open the dashboard address shown at completion. Pair your browser as described below.

The installer saves an audit and a private client credential on your computer. Press **D** for technical details and file paths. Its recovery download stays in your local cache; it is not uploaded to GitHub or redistributed in our releases. That file is a known stock recovery image, **not a backup of your device's settings or complete flash**.

Advanced mode supports prepared plans and explicit artifact paths for developers. Normal setup does not require typing hashes, commits or firmware paths. See [the stock migration reference](stock-migration.md) for those details.

If you need to stop, press **Ctrl+C once** and wait for **Installation stopped**. **Stopping safely** means the backend is still finishing a write or restart; leave the window and power connected. Keep the audit and follow the displayed recovery instruction. A timeout can occur after a write succeeded, so restarting the installer is not a general resume procedure.

If discovery identifies an existing Open Keylight installation, the installer reads its identity and shows its dashboard address. Use **System** there for updates; no stock reinstall is needed.

## Pair your browser

Open the light's local IP address. The dashboard comes directly from the ESP32 on port 80; no desktop control server is needed.

1. Hold the light's physical button for **three seconds**.
2. In the dashboard, open **Connect access**.
3. Give the browser a recognizable name and choose **Pair this browser**.

The window lasts up to 180 seconds and closes after one successful pairing. An existing trusted client can instead use **System → Open pairing for another client**. You can also enter the installer client's saved token in the dashboard or Stream Deck; keep that credential file private.

An accepted three-second hold gives one soft green pulse, then restores the previous lighting, including Off. This acknowledges the button hold and open pairing window; it does not mean a client has connected. Recording Lock, an update, an unavailable controller or a newer lighting command suppresses the pulse without preventing pairing. Feedback is never queued to flash later. Your running effect resumes after the pulse; saved scenes and desired settings stay unchanged.

Browser access is stored in the current tab session. Closing that session may require pairing again. System lets you view and revoke clients independently. There is no fixed four-client limit; new pairings are limited by available device storage.

If every token is lost, a continuous button hold starting at power-on and lasting ten seconds revokes existing clients and reopens pairing. It preserves Wi-Fi and scenes.

## Make your first scene

Choose White for warmth and brightness, or Colour for RGB. sRGB decoding is the default. In Colour mode, disable smooth transitions for an immediate change, or enable them and choose a fade duration. White transitions are not supported by the current application.

Scenes has four defaults and space for eight saved scenes. Save deliberately; turning a control does not overwrite a scene. A single button press toggles power, and a double press advances the saved scenes. Recording Lock protects output changes during a take; Off is always available.

Connect [Home Assistant through MQTT](home-assistant.md), or install the [Stream Deck plugin](../integrations/streamdeck/README.md). The plugin needs Stream Deck 7.0 or newer, the light's address and a paired token.

## Change Wi-Fi

Use **System → Save Wi-Fi & restart light**. Reconnecting may assign a different IP address. Passwords are never returned by the settings API.

An installation without configured Wi-Fi exposes `Keylight-Setup-<suffix>` at `http://192.168.4.1`. If configured Wi-Fi is unavailable, a three-second button hold also requests a temporary setup network. Pairing is still required to change settings.

## Update an existing installation

Firmware releases identify the ESP application, controller package and Stream Deck plugin separately. Updating the ESP does not automatically replace the NXP application.

1. Keep the previous known-good application and its checksum. Check System for healthy controller status and no pending update. Close other controllers and set the light Off.
2. Select the standalone ESP application and its published SHA-256 in System's firmware controls. Choose **Verify & update** once.
3. Reconnect after restart. Check the expected firmware version and `firmware_elf_sha256` in `GET /api/v1/device`, the dashboard, settings/scenes and controller health. Check low-level controls, then return to Off.
4. Within the **180-second application trial**, choose **Confirm this firmware** after those checks. The timer begins at application startup; opening or refreshing the dashboard does not restart it.

Native updates breathe gently from blue through cyan to green. Green means image verification completed. Failure uses two red pulses. The indicator needs a healthy controller and is suppressed by Recording Lock; verify the API and next boot's identity even if no indicator is visible.

Use only the standalone application image. A bootloader, partition table, merged flash dump, raw NXP bank and controller package are different formats. **Do not use `idf.py flash` on an installed light.** It is not the application-only update path.

Controller updates have a separate [package and recovery procedure](controller-updates.md).

## When something goes wrong

An unconfirmed ESP trial selects the previous application after 180 seconds **if the new application can run its trial task**. The retained bootloader has no automatic crash rollback. An early crash or unbootable application can require physical serial recovery; an A/B partition layout alone cannot prevent that.

Once confirmed, there is no general rollback button. Installing an older known-good application is another explicit update and requires compatible saved data. Do not erase NVS as a recovery shortcut.

Controller updates keep a durable failure journal in ESP storage. The dashboard can remain reachable while lighting is disabled. Restarting or rolling back the ESP does not clear that journal. Preserve the job details and follow the [controller recovery contract](controller-update-journal.md#explicit-recovery-after-physical-power-cycle); some failures need a whole-light power cycle.

Current evidence covers bounded functional checks on the reference hardware. Broad board compatibility, sustained maximum-output/current/thermal measurements and arbitrary early-boot recovery remain unqualified. The [development record](../PROGRESS.md) distinguishes observed behaviour from software tests.

## Build or explore without installing

The [README](../README.md#build-and-test) covers the toolchain. For a dashboard demo, run `npm run dev` inside `dashboard` and add `?demo=1` to the displayed address. It is visibly labelled and sends no device requests.

The [installer guide](../installer/README.md#offline-inspection-and-development) covers its labelled interface preview and source development. Preview mode does not discover or change lights.

GitHub Actions development artifacts are tied to their workflow commit and are not hardware-qualified releases. The ESP archive includes the application, matching ELF, source and asset metadata, and `SHA256SUMS`. Check those hashes and retain the ELF for diagnosis; never upload the ELF.

Use the dashboard on a trusted LAN. HTTP tokens authenticate requests but do not encrypt traffic. MQTT supports broker TLS with certificate validation. Do not expose the device directly to the public internet.
