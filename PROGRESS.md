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

The revised adapter prequeues both reply phases and passes tests that deliberately omit interrupt and main-loop service between them; the same tests fail the previous adapter. Hardware verification is pending. The independent ESP worker now claims and releases startup ownership, begins fades from exact available controller readback, and prepares the first scaled frame with the RGB master dark before exposing it. Failure paths preserve the last acknowledged frame and release only synchronized ownership. All ten host suites pass, including the actual worker source; the ESP-IDF application builds within its existing slot. Neither change is a claim of live lighting qualification.

## Original controller replies observed

The next bounded trial returned the original application's version and the expected silicon identifier repeatedly. Of 51 application getter attempts, 45 returned valid correlated reports and six returned zero-filled bodies. The application returned to the recovery loader at its deadline, and the previous working image was restored with all 448 staging blocks verified. The owner confirmed blue light output returned.

This is partial communication evidence, not reliable control. The existing ESP bridge can forward a zero-filled body after reading a nonzero reply length; it does not validate the controller report. A private trace-only trial will record controller-local reply phases and expiration without enabling PWM or changing the timeout policy. Explicit claim/readback/release will test ownership separately from asynchronous connection notifications.

The implemented HTTP surface now has an OpenAPI 3.1 contract covering 15 operations, checked against handlers and 98 schema/example/boundary checks. The Home Assistant guide documents the implemented MQTT interface and its reporting limits. No live Home Assistant test or independent ESP deployment is claimed. GitHub CI passed all jobs for the worker and reply-handoff changes.

## Receive-boundary race measured

A private trace-only image retained its last 116 events and aggregate counters through the 30-second return to the loader. Five retained failures cancelled the length phase with only one byte counted, then treated the master's subsequent body clocks as a new invalid request. The five command selectors match the corresponding empty network replies in order. The complete trace recorded zero reply expirations and zero receive overruns.

A host reproduction injects the final length byte and chip-select rising edge after an empty RX status sample but before the later select check. The master receives the expected length while the adapter clears the body. The fix must drain again after observing deselect and defer finalization if a new transfer starts. The known working controller image was restored and its static blue output verified through getters. Explicit claim/readback succeeded in the diagnostic, but the release guard failed, so the observer sent no release and made no full ownership-success claim.

## Corrected controller trial passes

The corrected traced image completed 73 application getters with valid correlated replies and no empty reports. Explicit claim, exact owner/name readback, guarded release and unclaimed readback all passed. Controller counters recorded 77 requests, 77 prepared replies, 77 completed length reads and 77 completed bodies, with no errors, expiry or receive overrun. Its fresh retained record ended at the 30,000 ms recovery deadline. The separate final empty version reply from the loader is excluded from application success counts. All 448 staged blocks were acknowledged and read back before the single commit; the working image was then restored successfully.

This qualifies the bounded communication trial on the reference board, not replacement lighting output. Command-triggered recovery and confirmed operation beyond the startup deadline are the next gates before a distinct low-output PWM trial.

The native ESP transport now completes zero-length replies without inventing a body transfer. Review against the installed ESP-IDF source also found that polling start rejects finite wait arguments before wire activity. It now uses the supported API under exclusive host ownership, retains deadline checks, and refuses further clocks after an ambiguous DMA completion error. Actual transport tests exercise those contracts and deliberately fail the prior implementations. All eleven host suites pass with sanitizers; the independent ESP application builds at 0x119b00 bytes with 27% slot space free. Its hardware deployment remains pending.

## Controller lifecycle and sustained communication

A separate live trial verified the owned recovery command at 2,776 ms, before the automatic startup deadline. The same frozen, PWM-absent image then accepted one explicit bench confirmation and completed 153 of 153 health getters over 60.125 seconds. Its retained trace recorded 163 complete request/reply exchanges, zero errors, zero expiry and zero overrun. One owned recovery command returned it to the loader at 63,154 ms. Each installation verified all 448 staging blocks before its single commit. This is a bounded communication test on one board; it does not qualify optical output or long-term reliability.

The original controller now reports an explicit status ABI containing its runtime role, initialized capabilities, observed part ID, trial state, uptime and reset-cause snapshot. The ESP checks this status before automatic confirmation and requires fresh confirmation readback. Diagnostic images cannot enable lighting or receive automatic confirmation. Exact legacy version 1.3.0.0 remains a separately identified compatibility path. Once-per-second health checks stop rendering after a detected controller failure or reset and discard earlier output revisions before reopening control. Uptime and sticky reset cause are not unique boot identities; a same-version legacy reset may go undetected.

All eleven host suites pass under AddressSanitizer and UndefinedBehaviorSanitizer, including the actual worker, startup, transport, HTTP and storage sources. Joint ESP driver/NXP application tests verify the new status and confirmation protocol. The default non-deployable ARM build passes, and the independent ESP application builds at 0x11a510 bytes with 26% slot space free. The native controller updater and live PWM qualification remain prerequisites for deploying the complete replacement.
