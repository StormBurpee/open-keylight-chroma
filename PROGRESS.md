# Development record

## 2026-10-07 — first independent ESP boot

The first independent ESP application reached 100% OTA acceptance, booted, imported the existing Wi-Fi credentials and served its embedded API on the original network address. Its observed free heap was 179,452 bytes. The NXP version getter returned 1.3.0.0, but a subsequent capability query was rejected by its retained ownership gate. Lighting readiness stayed closed; no control qualification or trial confirmation was attempted. At the application trial deadline, the lamp returned to the previously installed ESP image in the original fallback slot without a power cycle. This verifies this healthy-app fallback path, not recovery from an early crash.

The preceding all-off NXP experiment stopped on a staging readback timeout before commit. It never ran. The working NXP image was restored with all 448 blocks acknowledged and read back. The timeout cause remains unresolved; no original PWM output qualification is claimed.

## Dashboard art direction and implementation

The dashboard now follows a generated concept refined in two passes: graphite surfaces, ivory typography, copper controls and an atmospheric colour preview. The source concept and photographic scene atlas are retained in `assets/design` with their prompts. Runtime photography is a 14,160-byte WebP; no physical lamp is depicted. Controls remain semantic HTML, React and Radix, including the keyboard-accessible hue/saturation/value picker, exact RGB and hex entry, scene recall and adjustable fades. Scenes in production come from the device; populated preview scenes are isolated demo data.

Browser review covered the desktop Light, Scenes and System views, and narrow layouts at 300 and 868 CSS pixels without horizontal overflow. The production asset package is 152,536 compressed bytes, below the 235,520-byte budget. This design is implemented locally; it was not in the first ESP trial image described above.

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

## Native controller update path

The ESP now accepts a complete, hashed controller package in RAM and gives its single SPI worker an explicit installation job. The portable loader verifies all 448 staged blocks before attempting commit once, enforces the measured quiet interval, and requires fresh matching application identity afterward. Startup confirmation and durable journal clearing remain separate requirements before output is reopened. A power interruption or uncertain result blocks automatic SPI work after restart; this version does not yet expose a recovery endpoint.

The HTTP handler never drives SPI. Cached progress distinguishes reception, writing, readback, commit and application checks; the dashboard shows completion only after controller confirmation. The package tool validates the fixed image layout and never uploads. Legacy entry additionally requires an actual-part getter present in the private development controller image, so ordinary stock migration is not yet a public installation path. Native Off getter readback proves requested settings, not instantaneous optical darkness or completion of a legacy fade.

All fifteen host suites pass, including independent tests compiling the real loader and worker adapter together and failures at every write/readback boundary. The dashboard has 46 passing tests and packages to 136,208 compressed bytes. The ESP-IDF build passes with about 26% application-slot space free. Integration review corrected SDK header ordering, compiler discovery and a public progress-enum mismatch. These results do not qualify native SPI, controller flashing, or PWM on hardware; the independent ESP application still awaits its first live trial.

## Read-only controller update rejection

A fully correlated unsupported target can now reject an update before ownership claim or lighting mutation without leaving a permanent recovery journal. The adapter records a typed read-only outcome and known-idle transport; the manager checks both saved and returned audit evidence before durably clearing the journal. Failed clearing, attempted mutation, malformed responses, unknown transport and rebooted pending records remain blocked. The terminal result is `failed` / `invalid`, not completed installation.

Fresh ordinary bootstrap must reopen readiness, and old effects or scenes are not replayed. A new Off accepted during package reception retains its exact revision and is attempted once after a safe rejection; a failed or already attempted Off is never replayed. Its regression failed before the worker correction and passed afterward.

Targeted verification passed 4,041,255 real adapter/core assertions, 10,347 journal-manager assertions, 17,979 worker assertions, 47 startup assertions and 1,394 adapter-harness assertions. Windows and WSL sanitizer runs and the ESP-IDF build passed. These are offline results only; this change does not claim live deployment or controller-update qualification.

## Dashboard behavior review

Independent control-flow tests exposed two stale-draft regressions: opening Effects during a colour preview prevented later state updates, and recalling a scene from Light could leave the old colour draft visible. Both cases failed before the shared draft-cancellation fix and pass afterward. Seven new integration tests also cover pointer cancellation against newer controller state, main-tab cancellation with no late write, actual scene save/recall, retained fade duration, and a rejected scene request without retry. The targeted suite and TypeScript build pass; these checks use the explicitly isolated transport and make no hardware claims.
