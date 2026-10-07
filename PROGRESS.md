# Development record

## Complete controller replacement in progress

The remaining target is the original NXP LED application running alongside the original ESP application, followed by usable installation and integration checks. The compatibility ESP deployment is a milestone, not completion of the project.

The next hardware sequence is an explicitly selected all-off controller diagnostic, bounded low-output channel checks, then the original lighting application. Diagnostic images cannot receive production confirmation. A current-boot recovery proof can be passed to one subsequent explicit package only after the diagnostic's fixed recovery deadline and a fresh resident-loader response; any intervening SPI attempt, uncertain bus state or ESP restart invalidates it. The transport guard passes 9,574 assertions across 59 cases on Windows and Linux. These are software checks; the new diagnostic and PWM handoff have not yet run on hardware.

## Colour rendering, stored scenes and upload indication

The 0.1.1-dev candidate makes sRGB the default and places its persistent linear override in System. Colour transitions now seed a custom frame before entering custom mode, keep a constant native master and park the exact final acknowledged colour. Zero-duration changes use that same direct frame path to bypass the legacy Static-to-Static fade. Retarget and slow-ACK regressions check that an intermediate frame cannot be mistaken for completion. After reboot, a native colour can only be reconstructed approximately; its original chosen hex and brightness decomposition are not claimed as confirmed. Eight-bit output still limits very dim fades.

Focus, Blue hour, Ember and Afterglow are seeded once into free scene slots without activation or overwriting existing scenes. A single versioned NVS record keeps the collection and seeding marker together. The encoding setting uses a separate key without changing the existing credential/configuration blob.

The existing SPI worker now owns a bounded blue-to-purple ESP upload indication, with two red failure pulses and guarded restoration of the prior output. Off, Recording Lock, transport uncertainty and newer commands take precedence. The indication becomes available only after this application is installed; the previous live application cannot display it during this first upgrade. Controller flashing remains separate and excludes cosmetic output work.

The dashboard passes 75 tests and embeds in 152,944 compressed bytes. All 17 host suites and the ESP-IDF build pass; the updated actual worker passes 66,172 assertions plus startup/adapter checks. Independent source reviews cover output ordering, storage migration, encoding and indication. GitHub CI passed all jobs for deployed source commit `af24fc7`.

The 1,195,136-byte application was uploaded once through authenticated OTA and booted into a fresh trial. Its SHA-256 is `e875775e4e0890c287caa777681e3a024b4ec4c736c0e3ad0bed9b7bdcaf51bb`. Qualification verified the default sRGB setting, all four stored scenes and matching controller readback for white, Off, instant colour, a 300 ms colour transition and Off again. All four served dashboard assets matched their build hashes, and the physical lamp's dashboard displayed Connected with the new settings and scenes. The trial was explicitly confirmed on that evidence, and the owner's prior colour, brightness and transition setting were restored with matching readback at revision 6.

Optical smoothness and the newly installed upload indication remain unverified. The ESP application is original; the working NXP controller is still legacy version 1.3.0.0. No independent NXP PWM qualification is claimed.

## Stream Deck installation and settings corrections

The initial package required Stream Deck 7.1 and Node 24 while the installed application was 7.0.3. Version 0.1.2 targets Stream Deck 7.0 and Node 20, including the SDK's explicit legacy settings mode. Its compiled-plugin test now exercises settings requests and replies without message identifiers. The owner's app was upgraded to 7.6 during this work; all 25 tests pass using that installation's Node 20.20.0 runtime, and Elgato validation and packaging pass.

The owner's first installed test exposed a settings-routing defect. Adding the missing action type in 0.1.2 did not resolve it: the actual application log explicitly rejected `setSettings` and `getSettings` as coming from the wrong context. Inspection of the SDPI client confirmed that outgoing property-inspector commands require its registered inspector UUID, while replies identify the action instance. Version 0.1.3 separates those identifiers. The DOM harness now rejects action-instance contexts on outgoing inspector commands and verifies returned values before displaying success. Subsequent read-only observation of the lamp showed `last_actor: streamdeck`, idle operation and matching controller readback at revision 34. This establishes a live API/control result; it does not measure light output.

## ESP control qualification and browser correction

The corrected ESP application booted with controller readiness and passed four fresh native-readback checks: white at 5% and 5300 K, Off, static RGB at 5%, and Off. Each check matched its accepted revision and confirmed fields. The operator explicitly confirmed the trial based on these API/controller checks; optical behavior remains unverified. This is an original ESP application with the working legacy NXP controller.

The first live browser visit then exposed a frontend defect: the transport stored the native `fetch` function as an object method, producing an illegal receiver in Chromium. API qualification and demo-mode UI tests had not exercised this browser requirement. The default transport now delegates through `globalThis.fetch`. A receiver-sensitive regression fails the prior code and passes the correction; all 69 dashboard tests and the production asset build pass.

The correction was delivered through the original ESP application's authenticated OTA endpoint, which accepted the application and restarted into a fresh trial. All four served dashboard assets matched the pinned build hashes. White, colour and Off again passed controller readback checks; the trial was explicitly confirmed. Chromium then showed Connected while loading the dashboard directly from the physical lamp. No host service is needed to use it. The update indicator is still being implemented; this trial did not exercise it. GitHub CI passed for the deployed source commit `1212dc4`.

## 2026-10-07 — first independent ESP boot

The first independent ESP application reached 100% OTA acceptance, booted, imported the existing Wi-Fi credentials and served its embedded API on the original network address. Its observed free heap was 179,452 bytes. The NXP version getter returned 1.3.0.0, but a subsequent capability query was rejected by its retained ownership gate. Lighting readiness stayed closed; no control qualification or trial confirmation was attempted. At the application trial deadline, the lamp returned to the previously installed ESP image in the original fallback slot without a power cycle. This verifies this healthy-app fallback path, not recovery from an early crash.

The preceding all-off NXP experiment stopped on a staging readback timeout before commit. It never ran. The working NXP image was restored with all 448 blocks acknowledged and read back. The timeout cause remains unresolved; no original PWM output qualification is claimed.

The startup correction only permits a fully correlated ownership-denied reply after exact legacy version 1.3.0.0 to trigger one verified ownership claim. A fresh capability query must then establish the expected legacy response or pass every original-controller identity and readiness check. Cleanup remains conditional on synchronized ownership, and old output commands are discarded. Captured-instruction replay reproduced the legacy ownership exception; actual-worker tests pass 18,576 assertions plus startup and transport-adapter checks. Independent reviews passed. The corrected build awaits its next hardware trial.

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

## Native off-only controller qualification path

Explicit diagnostic uploads now preserve the role1 recovery deadline, issue one fixed OFF1 action, validate all five captured GPIO/timer phases through fourteen bounded pages, and never send FD. The journal and lighting gate remain closed. After the fixed recovery deadline and protected silence, an exact resident response can create a volatile one-use capability for another explicit package; no generic journal clear or automatic retry was added. The native adapter also handles the exact legacy ownership denial before its required part proof.

Offline validation passed 4,258,487 actual adapter/core assertions across 1,009 cases, 11,265 manager assertions, 175,244 HTTP assertions, and the worker/transport suites. Independent reviewers checked the profile ABI, token invalidation, no-FD behavior and retained journal. These results prepare a hardware trial; they do not establish physical PWM behavior or native controller installation reliability.

## Native handoff failure and loader envelope correction

ESP application `0.1.2-dev` was installed, its served assets and five control checks verified, and its trial explicitly confirmed. The following native NXP diagnostic attempt stopped before any acknowledged program or readback block, without attempting commit. The ESP remains reachable and its controller journal keeps ordinary output blocked.

Independent instruction replay of the resident loader reproduced a response-envelope mismatch: it returns a zero device tag with kind zero, while the native adapter required its own tag. The correction accepts that observed envelope only within the typed loader transport; normal application replies retain their identity checks. This is a demonstrated compatibility defect and a likely explanation for the live failure, whose original audit did not retain the precise failing exchange. Recovery and a new live trial are pending.

The authenticated recovery action now requires an acknowledged whole-light power cycle, a fresh ESP power-on reset and an untouched controller transport. It classifies the peer once after silence, retaining the journal when it establishes a fresh loader for a subsequent explicit upload. Only an entry-only failure with independently verified unchanged legacy firmware and Off state can clear its journal. Cached transport diagnostics retain the response envelope and failing stage. The 19 existing host suites, the new fixed-diagnostic codec suite, recovery request schema checks and ESP-IDF build pass. Deployment verification will identify the running ELF as well as its version.

## Explicit controller recovery and failure evidence

An authenticated recovery request now requires a confirmed ESP image, matching valid journal, explicit whole-light power-cycle acknowledgment, fresh ESP POWERON and pristine SPI. The worker waits silently, probes once, and can retain a fresh resident capability for a new explicit package without clearing the journal or replaying output. A narrowly proven unchanged legacy application after an entry-only failure may instead be checked Off and durably reconciled. Warm OTA boots, corrupt journals, stale requests, ambiguous bus state and failed journal clearing remain blocked.

The cached audit now includes the precise operation stage, transport result, complete raw envelope tag/kind and report digest, separately labelled from parsed/correlated replies. Evidence is captured before cleanup can replace the failed response. Targeted actual-source tests cover physical-reset admission, one-shot consumption, no initial SPI, no loader writes/FD, all recovery exchange failures, strict legacy identity/Off/release, journal persistence errors and no old-scene replay. Manager and worker sanitizer suites pass; the independent actual recovery-adapter harness covers 315 deterministic cases. This slice is prepared for hardware recovery, not a claim that recovery has already succeeded.

## Native LOW1 diagnostic admission

The controller upload API now separately admits the fixed LOW1 profile. It preserves the role 1 trial and journal, checks the unused profile, triggers once, and keeps SPI silent throughout the five-pulse sequence. All sixteen result pages and a repeated header must validate before the protected resident-return sequence can create a one-use next-upload capability. OFF1 keeps its distinct command and validator. Neither profile receives FD confirmation or production readiness.

The actual adapter/core suite passes 1,049 cases, including every LOW1 request failure, uncertain ACK, wrong profile, incomplete capture and both silence windows. The manager tests verify profile persistence, old journal compatibility, cross-profile rejection and no clearing or confirmation; HTTP tests verify explicit mode selection and malformed headers. These are offline results. They do not qualify emitted light, electrical polarity or live LOW1 installation.

## Native OFF1 passes on Storm Rim

ESP 0.1.4-dev was installed and explicitly confirmed after verifying its running ELF, embedded dashboard assets and preserved settings. The corrected recovery probe recognized the observed legacy unsupported reply, verified the unchanged controller and Off readback, and cleared the earlier entry-only failure.

The subsequent original NXP OFF1 installation acknowledged and read back all 448 blocks before its single commit. The application identity, fixed all-off command and all 224 register words passed. The owner reported that the lamp stayed completely dark throughout the test. After protected silence, the native updater verified the resident loader again and retained a one-use next-upload capability. The journal remains blocked intentionally; this proves the bounded all-off procedure and native recovery, not production lighting.

The next build adds the separate LOW1 procedure to verify red, green, blue, warm-white and cool-white output with fixed brief pulses. Full lighting remains gated on those observations and subsequent qualification.

The complete 21-suite host run passes with AddressSanitizer and UndefinedBehaviorSanitizer, including the new LOW1 suite. The fixed diagnostic schema boundary tests pass, and ESP 0.1.5-dev builds with 23% application-slot space free. Independent compiled-controller tests reproduce the exact OFF1 and LOW1 banks; live LOW1 qualification is next.

ESP 0.1.5-dev has now been installed and explicitly confirmed on Storm Rim. The running ELF, all four served dashboard assets and saved settings/scenes match the pinned candidate. The completed OFF1 journal survived the restart, retaining its target and 448 verified blocks while discarding volatile recovery proof as designed. An independent manager test also reproduces the full OFF1 → ESP restart → cold recovery → LOW1 → production-upload admission sequence without granting diagnostic images production confirmation.

## Five original lighting channels observed

After an acknowledged whole-light power cycle, native recovery established the resident loader from the retained OFF1 job. The LOW1 installation then verified all 448 blocks, booted the expected original application, and completed its single fixed sequence. Both the ESP validator and a separate host validator accepted all 256 recorded words, including five 100 ms pulses, dark handoffs, the complete channel mask and zero reported errors. The protected return to the resident loader also passed without another physical reset.

The owner observed red, green, blue, warm-white and cool-white flashes in order and explicitly confirmed darkness between them. This supports channel mapping and low-duty output on the reference board. It does not measure current, temperature or calibrated colour. Full lighting will retain the existing duty envelope; its first installation and API control checks are next.

## Original lighting application installed; load-change fault under investigation

The full original NXP lighting application was installed through the native ESP updater, with all 448 staging blocks acknowledged and read back. Typed role-2 startup confirmation succeeded, the journal cleared durably, and the device reported the original controller ready. Seven live API checks passed with fresh getter-confirmed fields: blue, an instant purple change, a timed blue fade, warm and cool white, Off, and a final blue setting. These checks used 5–15% brightness; the owner confirmed the visible sequence and steady blue.

Subsequent owner testing at 100% colour while changing controls exposed repeated ESP brownout resets. Reset reason 9 was verified against ESP-IDF, and Off restored stable network access and confirmed dark output. This is an unresolved load-change failure, so the successful bounded checks are not a production-readiness claim. Offline comparison finds identical stock/original RGB output arithmetic at the canonical master of 255 and the same timer periods; intermediate electrical behaviour and the ESP power configuration are being investigated. Brownout protection remains enabled.


## Truthful MQTT availability and physical scene selection

MQTT availability now follows controller readiness, connectivity and update/recovery gates. Lifecycle changes post coalesced notifications to the existing MQTT event loop; only that callback publishes. This also removes a lock inversion between the application mutex and the MQTT client's callback/API lock. Reconnect and Home Assistant birth republish current availability, enqueue failures remain retryable, and stale confirmed state is suppressed.

Physical double-click scene selection now advances only after an accepted activation. Recording Lock and busy refusals retain the selection, while empty slots remain skippable. GPIO configuration and task creation errors are returned and recorded without preventing dashboard or MQTT startup.

Actual-source sanitizer tests cover broker events, queue/enqueue failure, publication races, sampled button gestures and startup failures. The worker, controller-job and ESP-update suites verify representative lifecycle notifications outside the application mutex. These are offline changes; MQTT delivery and physical-button behavior have not been requalified on the lamp in this slice.


## Brownout startup recovery

A separately scoped startup path now responds to ESP brownout resets by validating controller identity and ownership, sending native Off once, and requiring fresh Off readback before controls become available. It does not adopt or replay the retained high-output state. An uncertain setter, mismatched getter, ownership failure or failed release leaves output unavailable for that boot without a repeated Off attempt; HTTP recovery remains available. Ordinary reset reasons retain the existing startup behavior.

Targeted actual-worker sanitizer cases cover both supported backends, both Off setters and uncertain replies, readback and release failures, unsupported identities, no repeated mutation across later health deadlines, ordinary-restart adoption and fresh original-controller trial confirmation. This is recovery from the demonstrated reset loop, not a root-cause fix. Brownout protection and Wi-Fi power settings remain unchanged; no hardware validation was performed by this implementation slice.

## ESP update interruption with the original controller

The first attempt to install ESP 0.1.6 from 0.1.5 with the original lighting controller did not complete. Off was getter-confirmed before the upload; the HTTP client timed out waiting for its response. The ESP later returned with its original version and monotonically increasing uptime, while its NXP exchange was faulted. This was not another ESP reset. A whole-light power cycle restored both original applications, controller readiness and confirmed Off.

ESP-IDF's bulk application erase can suspend the host longer than the original controller's 100 ms pending-reply window. The update indicator currently shares that interval without flash/SPI exclusion. This establishes a missing coordination mechanism; the exact upload duration was not captured, so it does not by itself explain every part of the timeout. Sequential erase and transaction exclusion are being developed before another attempt. Repeated identical controller faults now keep the first history entry rather than evicting the initiating event; actual-source tests verify that a changed failure or new operation still records a new event.

## Flash and controller transaction coordination

ESP 0.1.7 introduces a shared admission gate for complete SPI exchanges and flash mutations. OTA now erases sequentially during reception. Configuration, scenes, update journals and synchronous Wi-Fi initialization/calibration use the same gate. Startup fails closed if the gate cannot be allocated. An idle controller transaction may exclude a bounded, measured admission wait from its response deadline; pending or ambiguous replies cannot receive that allowance or an automatic replay.

The update indicator consumes admission credit once per driver operation. An immutable, lock-free reboot cutoff also reaches the native transport, so a stale receiving snapshot or a queued flash writer cannot extend lighting work beyond the verified update's restart grace. The SPI holder never acquires the application mutex. Tests cover publication while waiting, every wire boundary, guard failures, journal persistence, startup ordering and uncertain replies.

The integrated 27-suite sanitizer run and ESP-IDF build passed. ESP 0.1.7 then installed from 0.1.5 in an 18-second end-to-end trial. The exact running ELF, all four served dashboard assets, saved settings/scenes, clear controller journal and fresh Off readback passed before explicit image confirmation. No further physical reset was needed. Recording Lock suppressed cosmetic output in the older uploader during this bootstrap; the new coordination and breathing indicator still need a subsequent live OTA trial. The high-output brownout remains a separate qualification item.

## Updated controller passes progressive output checks

NXP 0.1.1 installed through ESP 0.1.7 with all 448 blocks acknowledged and read back, fresh original-controller confirmation, durable journal clearing and confirmed Off. Its timer handoff waits for duty reductions to latch before applying increases; this is a bounded overlap correction, not a proven explanation of the earlier brownouts.

Two complete runs passed 10%, 25%, 50%, 75% and 100% colour levels. Each stage checked steady red, an instant blue change, a 500 ms fade, a deliberate retarget during a two-second fade, and Off. Both retained the same ESP boot and fresh controller health. The owner watched the second run and confirmed quick blue changes, smooth red/pink fades and darkness between stages, with no visible issue. A preceding run stopped because the test harness missed its shorter retarget window; it recorded no device fault and verified Off. Its audit was preserved before correcting the test timing.

These are bounded functional observations. Sustained operation, update-indicator visibility, current/thermal measurements and stock migration are separate evidence; the project is still a development preview. CI now also executes the compiled Cortex-M0 lighting application under pinned Unicorn 2.1.4, and the first hosted run passed all twelve regression cases.

## Update indicator observed on both original applications

A same-image OTA using the installed ESP 0.1.7 uploader completed with NXP 0.1.1 online. The old boot's history retained upload start, verified image and getter-checked restoration in order; the new software boot reported the exact expected ELF, ready controller, preserved settings/scenes and confirmed Off. No extra image confirmation was needed because this was the already accepted image, so this test does not exercise a new trial or fallback. The owner confirmed that the breathing was visible and good, but that purple dominated the appearance.

ESP 0.1.8 changes progress to blue through cyan to green, with the same gentle breathing and low master level. Verification selects the green endpoint; failed uploads keep the separate red pulse. Combined blue/green amplitude is bounded throughout the blend. State/math, coordinator and actual worker tests cover endpoints, monotonic progress, breathing continuity and unchanged failure/restoration behavior.

The 0.1.7 → 0.1.8 trial installed and was explicitly confirmed after exact ELF, served-asset, controller-health and preserved-configuration checks. A subsequent same-image OTA exercised the new uploader: its ordered history recorded reception, verification and getter-checked restoration, and the next boot returned with the exact image and ready NXP 0.1.1. The owner missed this run, so the new palette's appearance is not yet observed. Failure-red behavior remains covered by software tests, not a deliberate failed live update. That same-image test left both application slots containing 0.1.8; earlier known-good images remained local artifacts, not an older live fallback slot.

## Sustained output and rapid control qualification

ESP 0.1.7 with NXP 0.1.1 passed a 120-second hold of `#FF0020` at 100% brightness, followed by instant and faded RGB changes, an interrupted transition, low warm/cool white and final Off. The bounded run took 145.8 seconds and retained the same ESP boot and healthy controller. No current, rail voltage or temperature was measured.

After installing ESP 0.1.8, a separate test accepted 24 colour retargets at 100% brightness with 500 ms transitions. It paused 50 ms between accepted commands and checked each exact revision and desired state. Twenty-three retargets were conservatively proven to interrupt an in-progress transition. Periodic health checks retained the same boot; the final endpoint and Off were getter-confirmed. No recovery command was needed. This exercises rapid control input similar to the original failure report without establishing that the earlier brownout's cause has been found.

The public guided stock installer is being prepared for the second light. Its intended order preserves the stock ESP bridge through controller identity, dark and low-channel qualification, then installs and confirms the original lighting controller before replacing the ESP. No second-light migration result is claimed yet.

## Guided stock migration prepared

The public installer now provides offline preparation, guided installation and separate explicit controller restoration. It validates original image packages, exact stock identity, the resident loader fingerprint and ROM-IAP part identity before admitting PWM diagnostics. Each complete controller bank receives 448 program acknowledgements and 448 readback comparisons before one commit and a protected quiet interval. Optical prompts follow diagnostic recovery; the original ESP is installed last, followed by exact native image/asset checks and browser pairing and confirmation. Ambiguous mutations are never retried automatically.

The plan helper validates every local artifact before publishing a complete, exclusive manifest. It neither contacts a device nor copies proprietary restore bytes. The owner's restore provenance remains explicit; a logical staging capture is not relabelled as an active-application backup. This is an experimental builder workflow, not a general release bundle.

All 64 migration test groups pass on Windows and WSL, including 17,388 comparisons against the actual C diagnostic predicates. The integrated 28-suite sanitizer host run also passes. The second-light plan and artifacts are pinned privately. Installation is awaiting the owner's availability to observe that light; an identity preflight timed out before connecting to its last known address and issued no device command. No stock migration success is claimed.

## Dashboard reconnection feedback

The installed browser retained an old timeout banner and an earlier command-accepted heading while its current state already showed Connected and confirmed Off. Two regression tests reproduced the stale message and a related loss of an uncertain-write warning across a later read outage. The store now clears recovered read errors while separately retaining a failed mutation until the owner dismisses it or starts a new operation. It does not retry commands or treat a fresh read as proof that an uncertain write succeeded.

All 85 dashboard tests and the production build pass; an independent review checked generation races and error persistence. ESP 0.1.9 includes this interface correction; controller code and the blue-to-green update palette are unchanged.

The 0.1.8 → 0.1.9 application-only OTA passed on the reference light. The exact ELF, all four served assets, preserved settings/scenes, ready original NXP 0.1.1, clear update journal and fresh Off state passed before one explicit confirmation. Old-boot history retained upload, verification and getter-checked output restoration in order. A browser reload showed Connected, all four scenes and both firmware versions without the old error banner. Optical observation of this update was not requested while the owner was unavailable. The running slot is 0.1.9; the preceding 0.1.8 image remains in the other slot, subject to the documented application-level fallback limits.

GitHub's complete Build and verify workflow and compiled NXP workflow passed for firmware commit `fbf2552`. Stock migration remains pending: the second light did not answer its read-only identity preflight, and the owner cannot currently watch its channel qualification. The prepared installer has not flashed that light.

## First stock installation preflight

A whole-light power cycle restored the second light's stock network service. The public installer verified its name, ESP 1.0.13.0 and NXP 1.3.0.0, then stopped before loader entry or any firmware write because the RGB Off acknowledgement differed. The complete received-frame digest matches an earlier stock capture exactly: stock firmware normalizes its second effect argument to five rather than echoing zero.

The owner then observed bright white. Independent getters confirmed RGB Off but white brightness 255. Stock controller disassembly explains the ordering: switching RGB Off restores the remembered white-only brightness, undoing a preceding white-zero command. Stock migration now selects RGB Off before zeroing white, and still requires both exact independent Off getters before entering the loader. The failed attempt and subsequent read-only observations remain separate private audits; no ambiguous write was retried.

All 67 migration test groups pass on Windows and WSL. Independent execution of the captured stock handlers reproduced the failure and the corrected ordering across cached brightness values. No proprietary executable bytes are included in the repository.

The next guided attempt completed all four controller transfers with 448 program acknowledgements and 448 readback comparisons apiece. The original identity image reported ROM part `0xbc40`; OFF1 and LOW1 register records passed and the owner confirmed darkness and all five ordered pulses. Original NXP 0.1.1 was then confirmed. The stock ESP updater accepted the complete original ESP 0.1.8 image.

The installer failed to recognize native HTTP within its 90-second startup allowance. An independent read at uptime 101.8 seconds reported the exact expected ELF and a ready, confirmed original controller. Browser pairing, white at 5% and Off worked, but the application trial expired before independent asset verification and confirmation. The next stock-bridge read proved ESP 1.0.13.0 with the original NXP 0.1.1 still confirmed and both outputs Off. This exercised application-level fallback, not bootloader crash recovery. This attempt is not recorded as a completed installation.

Further investigation found an installer HTTP-reader defect rather than evidence of delayed Wi-Fi startup: after reading the final bytes of a Content-Length response, Python closes the response socket, but the next loop iteration attempted to set that socket's timeout. The observer treated the resulting socket error as a network outage. Real loopback HTTP tests reproduce the error with plain, gzip and fragmented responses; they also expose a missing truncated-body check. A later device trial independently passed the exact ELF and all four served-asset hashes while the faulty observer still reported waiting. The earlier inference of slow Wi-Fi is withdrawn.

## Stock recovery image acquisition

A fresh HTTPS download of Razer's 02.03.13.00 archive matches the preserved reference archive. Decoding its NXP Intel HEX and supplying the reference profile's erased tail reproduces the complete reviewed recovery bank byte for byte. The public acquisition helper pins the archive, member sizes, version, HEX, decoded application and complete bank; it rejects redirects, changed files, malformed records and out-of-range or overlapping data. It publishes a fully written file without overwriting an existing destination. Proprietary bytes remain in the operator's local cache.

Nine offline test groups use synthetic image contents and cover parsing, metadata, cache behavior, publication failures and redirect rejection. The helper also completed a real download and reproduced the expected bank digest. This removes the need for users to locate a private capture; the downloaded artifact is described as a stock recovery image, never a device backup.


## Guided installer and release packaging

The React / Ink installer now discovers named lights, reads a release bundle and acquires the pinned official recovery image before review. Advanced artifact paths remain available without placing them in the normal setup flow. An original-only bundle packager validates controller roles, ESP descriptor version and exact embedded dashboard assets, then publishes a new directory without replacement. Thirteen packaging tests pass on Windows and WSL; the full migration suite passes 107 tests plus the existing 17,388 differential cases.

The next ESP-only attempt stopped on a stock HELLO timeout before any firmware write. Fill remains awaiting a new physical reset and the reviewed recovery/automatic acceptance sequence. That sequence reuses the proven controller transfer and fixes the HTTP observer; the native client saves its credential privately and checks exact identity, low white and Off before one confirmation. No completed second-light installation is claimed yet.


The guided interface now passes 41 tests on Windows and WSL, including a real Python JSONL subprocess, both observation prompts and the native acceptance handoff. Its self-contained Node bundle has no external runtime npm packages and carries dependency license notices. Actual portable Node 22.23.3 and embedded Python 3.13.16 downloads passed pinned size/hash verification, extraction and an offline validation of the existing migration plan. The Windows PowerShell launcher passes 95 assertions across 18 offline cases, including malformed compressed entries, cache corruption, process failure and cleanup.

Read-only discovery found the installed reference light on the live LAN; parsing the preserved stock mDNS capture also recognized both original stock records. The README installer image is an actual Ink demonstration capture and is labelled accordingly, not a claim of live installation success. Rebuilding all four NXP profiles reproduced exactly the previously reviewed bank hashes. The release ZIP packager passes 14 tests on Windows and WSL and excludes vendor files, credentials and unlisted artifacts.

The final structured-event backend passes 120 migration tests on Windows and WSL, with resource warnings treated as errors. A real full stdout pipe cannot interrupt controller programming, readback or the required quiet interval: progress is bounded and asynchronous, and stage boundaries stop within two seconds if presentation remains unavailable. Initial status and stage events use the same bounded writer. Independent review reran 21 focused groups on both platforms. The integrated 30-suite Linux sanitizer run also passes; the Windows-only launcher suite runs separately on Windows.

Release review added explicit installer version metadata and matching checks against the product version and ESP descriptor. Seventeen archive tests now pass, including an extracted JSONL backend under isolated Python and a checked closure of the offline guide's relative links. Dashboard validation passes all 85 tests and its production asset build. The candidate remains `0.2.0-alpha.1`; these software checks do not complete the pending second-light acceptance.

The expanded compiled-controller workflow passes all 24 emulator cases, covering production, dark and low-output builds; the separate identity profile also compiles. A clean hosted installer run exposed three test assumptions: the checkout's directory name and Windows short-path expansion in two expected paths. Those assertions now compare actual file identities while preserving exact non-path arguments. No installer or device behavior changed. The first alpha ESP build succeeds at 1,212,416 bytes, with 23% of its application slot free.

## Second light recovered and explicitly confirmed

The preserved ninth recovery attempt ended with `ConnectionResetError` during the initial stock connection, before any mutation intent or firmware write. It did not retry or restore automatically. After the owner replaced the cord and restarted the light, a separately authorized tenth attempt completed. The cord change and restart preceded success; this does not establish an electrical cause for the earlier disconnects or brownouts.

The recovery matched the reviewed resident loader, programmed and compared all 448 blocks of the original lighting application, committed once and observed the required quiet interval. Fresh controller status then confirmed role 2 and readiness. ESP 0.1.8 was accepted, and the corrected HTTP observer verified its exact running ELF and all four served dashboard assets. The separate native acceptance client paired once, checked initial Off, checked white at 5%, returned to verified Off, and issued one ESP confirmation. Its final readback confirmed acceptance and Off with original NXP 0.1.1.0. The recovery and acceptance audit spans approximately 71 seconds; the native client began with over 134 seconds remaining in the ESP trial.

This run used a private, pinned recovery adapter and the separately tested native acceptance client. It retained the earlier physical OFF1/LOW1 observations; the new API checks record optical verification as false. It completes recovery and acceptance on the second light, but does not qualify a fresh, uninterrupted installation through the public React / Ink interface. That interface's complete migration flow remains tested with modeled transport. The proven HTTP-reader defect and preserved failed attempts remain part of the evidence; success does not turn those failures into a slow-startup diagnosis or establish general electrical reliability. The alpha release's own installation and upgrade qualification remains separate.

## Restored-stock rehearsal stopped at an unverified reboot

To rehearse the packaged stock installer, the confirmed second light received the exact official ESP 1.0.13 application through the native OTA API. The endpoint returned HTTP 202 with acceptance and reboot flags. The first planned stock observation then timed out while establishing TCP, before sending any stock command. No controller firmware was written, no recovery was started, and the upload was not repeated. Acceptance proves that the uploader selected the image; it does not prove that stock firmware booted successfully. The original NXP application remains the last verified controller image.

Subsequent read-only checks also lost the first reference light, which had received no firmware write during this operation. Neither known MAC appeared at another LAN address or in discovery, while the gateway and other hosts responded. This is an unresolved reachability failure, not evidence establishing a shared power fault, two firmware crashes, or a cause attributable to the second light's OTA. The upload audit and separate network observations are retained privately. Both current boot states require fresh verification before further installation work.

Reverse migration is not part of ordinary Open Keylight upgrades. Its successful boot has not been qualified, and it must not be described as a proven recovery path. The public installer qualification and alpha release remain pending; the draft release has not been published.

## Recover setup access after losing an IP address

An offline review found that the ESP subscribed to address acquisition but not address loss. ESP-IDF can report a lost DHCP address while the station remains associated. The application then retained a stale IP and a connected flag, which could discard the owner's request for its temporary setup network.

Network handling now tracks association separately from a usable IP address, receives address-loss events, clears stale address/status, and permits the existing button-triggered setup network. DHCP continues without a forced disconnect or reboot; a genuine deassociation still uses the existing spaced reconnect attempts. Lighting and saved configuration are unchanged. Tests deliver events through the actual registration filter and cover initial DHCP, address loss while associated, setup access, a new address, and subsequent deassociation. Independent review against the installed ESP-IDF event handlers and the focused sanitizer tests passed. This is a reproduced software defect, not an established cause of the current reachability failure, and it has not been deployed to either light.

The complete 30-suite host sanitizer run also passes. The installer now gives explicit retry/manual-entry guidance when discovery is empty, and a dedicated dashboard/update view for existing installations. That view performs one bounded, unauthenticated device read, checks identity against discovery, and displays unverified results without asserting that installation is complete. It never pairs, changes settings, confirms a trial or uploads firmware. The added navigation and identity tests pass on Windows and Linux.

## Windows launcher checked through its actual entry point

Extracted-release review initially treated an unavailable `Get-FileHash` command as a harness environment issue. Adversarial review reproduced it through the supported entry point: PowerShell 7 launching `start-open-keylight.cmd`, which starts Windows PowerShell 5.1 with an inherited Core module path. Directly launching the PowerShell script normalizes that environment and had hidden the failure. The earlier direct-script pass did not qualify this invocation path.

Archive verification now uses a disposed .NET SHA256 instance and file stream, retaining exact size/hash checks without rewriting the user's module path. A regression launches the actual CMD file from PowerShell 7, uses an isolated local runtime cache and real Node, verifies literal argument delivery and unchanged environment, and checks session cleanup. All three Windows launcher test groups and independent review pass. No firmware or lighting behavior changes in this correction; the failed QA logs remain preserved.

## Stock boot verified; controller restore interrupted

Fresh physical resets allowed the second light's stock ESP to answer its version, hardware address and name queries. This establishes that the official ESP application booted; it supersedes the earlier unverified-reboot observation. The first reference light also returned with its previously verified original ESP and NXP applications, a ready controller and no pending trial. Neither observation establishes the cause of their earlier network loss.

Two separately audited cold recovery attempts stopped on transport timeouts. The first completed 43 resident-loader reads without any firmware mutation. Following another explicitly confirmed physical reset, the second matched the entire resident fingerprint, received acknowledgements for all 448 stock-bank program blocks, and compared 182 staging blocks before losing the next response. It sent no End/activation command, no Abort and no automatic retry. The stock controller restore is incomplete; a successful upload or active stock controller must not be inferred from those acknowledgements.

ESP-local UDP queries and neighbor discovery also stopped finding the second light after both failures. Static review found no fixed resident timeout or common failing read address that explains the disconnects. Power, Wi-Fi and stock ESP failure remain unresolved. The audits are retained privately, and no further controller traffic is sent on the expired reset premise.

The extracted Windows release launcher has now been exercised interactively through its normal guided path on the live LAN: it discovered both lights, distinguished the existing installation, selected the stock target, validated the bundled files and acquired the pinned recovery image before presenting review. Installation has not started through that TUI, and complete public-installer qualification and prerelease publication remain pending.

## Pairing feedback and growing client storage

An accepted physical three-second hold now requests one low-brightness green breath, then restores the prior output, including Off and running effects. Pairing opens independently of that cosmetic feedback. A request expires rather than flashing late; recording lock, a newer lighting command, loss of controller readiness and updates take priority. Independent review caught and corrected an early-cancellation case that could release an existing effect's controller lease. The actual button, worker and shared output coordinator pass their Windows and Linux sanitizer tests; the pulse's appearance has not yet been observed on a lamp.

Paired clients now use a growing collection instead of four fixed slots. Existing tokens and labels migrate on the first successful change, with no shared-NVS erase. Available flash and memory remain real limits and produce explicit errors. Storage mutations are prepared separately and published after persistence; uncertain writes prevent further client edits, and uncertain removals also stop authentication until reconciliation. The API contract documents that deliberately returning to v1-only firmware exposes its historical client record, rather than the newer additions or revocations.

Tests pair, authenticate, revoke and reload forty clients, exercise malformed records and allocation/persistence failures, and cover each pairing HTTP result. The dashboard displays and manages all forty clients without a slot counter. All 30 integrated host suites and 86 dashboard tests pass, with independent review of both firmware changes. The dashboard build is 153,312 compressed bytes. These changes are committed source; deployment and hardware verification remain separate.

## Packaged installer started on the second light

The pairing build compiles under ESP-IDF 5.5.5 at 1,216,032 bytes. Both hosted verification workflows passed for `c56fb91`; a new local Windows release archive contains this exact candidate. It has not replaced the public draft assets or been qualified on a light.

An explicitly requested ESP-only recovery attempt matched the stock ESP identity but lost its connection during transfer. The audit contains one Start, 115 data send intents and valid 1%, 2% and 3% processing notifications. It contains no End or acceptance. Subsequent ESP-local reads timed out. The notification contents do not establish a rejection reason, and send intents do not establish delivery. No automatic retry followed.

After another owner-confirmed physical reset, the new extracted Windows release was run through its actual CMD launcher and guided installation. It discovered the second light, verified its bundled files, obtained the pinned recovery image and started the selected installation. Its connection check stopped on a missing controller routing notification after HELLO, before any mutation. This is a real public-TUI failure, not a completed installation. The audit is preserved privately. The second light's last confirmed controller application is still original 0.1.1; the interrupted stock restoration did not activate the stock controller, so the normal stock-install starting state cannot be assumed.

## Finish an interrupted installation without replacing the light engine

The owner's subsequent TUI run received a controller-version reply whose full frame hash matches original 0.1.1.0 exactly, then stopped at the stock-only gate before any mutation. The public installer now offers an explicit three-stage finish path for this state. It independently verifies the target, original controller identity, a fresh ownership claim, same-boot status and Off before uploading only the ESP. It never resumes staging or describes the retained controller as newly qualified. Unknown controllers and resident loaders are refused.

The TUI offers this path at review and after a stopped initial check, requires a new explicit start, and creates a unique audit. Its progress presents only the checks actually performed. Review also corrected an acceptance-client race: a stopped backend or unexpected child exit now cancels further native acceptance work. All 63 installer tests pass, including real Python child processes for both workflows; all 132 migration tests and 17,388 differential cases pass. Independent reviewers checked the ownership/deadline gates and the UI cancellation boundaries. Live finish-path verification remains pending.
