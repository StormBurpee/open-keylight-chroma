# Development record

## 2026-10-07 — independent firmware begins

The preceding private hardware investigation demonstrated application OTA, RGB/white control and bounded smooth transitions in an additive stock application. That is evidence about the interface, not evidence that this independent firmware boots.

This repository starts with original implementation source only. The owner selected the name Open Keylight Chroma and explicitly requested replacement applications for both ESP32 and NXP. ESP bring-up retains protocol compatibility temporarily; an independent NXP implementation is part of the target. No vendor images, device credentials, private network traces or decompiler exports belong in this tree.

Work in progress: portable protocol driver and fault tests; ESP-IDF boot/migration compatibility; authenticated API and state model; embedded dashboard. Public installation is not yet qualified. Release claims will be tied to a reproducible test record.

Decisions: preserve the installed flash layout; pin ESP-IDF 5.5.5; cap an application at 1,572,864 bytes; single SPI owner; no NVS erase fallback; preserve output on connection loss; MQTT discovery for Home Assistant; audio deferred.

## First implementation and review

The independent ESP application builds with the compressed dashboard at approximately 1.15 MB, within the original 1.5 MB slot. Nine host suites pass, including the actual OTA, storage, HTTP and network source compiled against fault-injectable platform boundaries. The dashboard has 36 passing tests. The Stream Deck package has 24 passing tests, including a compiled SDK process talking to a loopback device fixture, and passes Elgato's manifest validation.

Review corrected false confirmation of mismatched controller readback, pending MQTT state publication, transition parking normalization, confirmation/expiry races, upload deadlines, credential-bearing broker URIs, client revocation and setup-network lifetime. These tests do not replace live validation.

Read-only ROM identification on the development board returned `0x0000bc40`, the LPC11U35/501. A separate original NXP SPI-only application was uploaded after verifying all 448 staging blocks. It deliberately contains no enabled PWM path. Its application and timed recovery responses were **not observed** in the first live trial. Recovery investigation is in progress; this trial is not qualified, and the independent ESP application has not yet been deployed. Default NXP builds remain non-deployable.

The public repository is `StormBurpee/open-keylight-chroma`. The identity uses studio lighting and abstract raster controls; generated imagery does not depict the physical product. Source imagery and art direction are retained with the project.

## Controller recovery and SPI correction

A full power cycle made the resident NXP loader reachable. Restoring the previous controller application succeeded after 448 acknowledged blocks and 448 matching staging readbacks; the owner confirmed physical light output returned. An ESP-only restart had not restored controller communication. The recovery loader remains intact, but the failed trial's warm recovery is not yet explained.

Review reproduced a receive-path defect by correcting the hardware model: SSP busy includes queued transmit data, so prefilled bytes could prevent a short transaction from completing even after chip select rose. Completion now follows the separate chip-select monitor. The endpoint also handles the existing bridge's nine-byte connection notifications and zero-length response handshake. Regression tests cover short transfers, ownership notifications, abandoned responses and subsequent normal requests.

These fixes are verified in host tests, not yet on the controller. A separate minimal application will isolate application entry and immediate software recovery before another SPI trial. The independent ESP application remains undeployed. The first complete GitHub CI run passed portable, interface and ESP32 build checks.

## Commit timing isolated on hardware

The uploader opened a new connection about 25 ms after requesting the controller commit. Connection events produce SPI traffic immediately; the loader was still installing flash with interrupts masked. Captured-instruction replay reproduced an overrun path that leaves a receive interrupt pending before the final reset. The vendor updater also waits after commit.

The uploader now holds its existing connection open and silent for at least three seconds after every attempted commit, including uncertain sends and cancellation. A paired live test changed only the minimal application's record identifier: the protected update returned a fresh application-entry record through the recovery loader without a power cycle. The earlier immediate-reconnect test had remained unresponsive until power was cycled. See [controller update sequencing](docs/controller-updates.md).

A subsequent trial of the larger original application also returned to the loader at approximately the 30-second deadline without physical intervention. Its application SPI replies were still absent, so communication remains unqualified. The working controller image was restored successfully. Investigation is now focused on the length-to-body reply handoff; the existing ESP bridge reads the body immediately without waiting for another ready signal.
