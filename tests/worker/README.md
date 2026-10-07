# Lighting worker and controller-update adapter tests

Run `python tests/worker/run_tests.py` with AddressSanitizer-capable Clang. The runner compiles the actual worker, app startup, output policy, state arithmetic and controller-update adapter in three deterministic host executables. Results and source hashes are saved under `build/worker-tests`.

The worker tests cover exact native readback, ownership and transport errors, trial qualification and uncertain confirmation, reset detection without replay, and the update gate. A pending journal prevents even SPI initialization. A taken controller job excludes ordinary rendering/health; receiving reservations keep accepted Off and health functional. Post-update confirmation does not open readiness until durable journal completion succeeds, and the old desired scene is replaced by fresh dark readback.

Canonical colour tests cover zero-duration frame-to-Static ordering, no master mute when entering from canonical Static, retained-frame retargeting with fresh ownership, foreign-owner invalidation, an ACK crossing a fade deadline, encoding changes, exact forward-converted readback and logical-intent retention across matching restoration. Every raw channel byte is adopted after a simulated restart in both encodings, followed by a brightness-only command; reconstructed fields stay unconfirmed until that new intent succeeds. These are protocol/model assertions, not optical smoothness measurements.

ESP upload indication uses this same worker and suspends the ordinary renderer.
The actual coordinator is compiled into the worker harness: tests exercise blue
upload frames, verified purple, two red failure pulses, exact native restoration,
an interrupted custom renderer, Recording Lock, pending/new Off, poisoned setup
and journal exclusion. Its own periodic lease/mode checks and per-frame ACKs run
while indication is active; the ordinary health/bootstrap loop resumes afterward.
No HTTP callback or second task accesses SPI. Reboot timing remains independent.
See `tests/update_indicator` for boundary fault injection and sampler properties.

Legacy 1.3 exempts its version getter from ownership checks, but a stale foreign owner can deny the new status getter before the legacy unsupported-command response. Bootstrap handles only that exact version and validated denial with one verified claim, then requires a fresh status response. Tests reject malformed denials, other versions, failed claims and second probes, diagnostic roles, wrong parts and failed cleanup without lighting writes or trial confirmation. The claim is reused for native-state adoption and guarded release; health probes themselves remain read-only. An unsynchronized transport is never used for cleanup.

A durably rejected read-only incompatible update returns to fresh ordinary bootstrap without replaying the old output revision or effect. A new Off accepted during receive is attempted once afterward; failed or already consumed Off intents are not replayed. Rejection-clear failure keeps the journal gate closed. The real adapter/core suite separately proves the typed rejection outcome; job-manager tests verify its durable cleanup requirements.

The adapter tests execute `controller_worker.c` against bounded protocol/RTOS mocks. They verify original identity/ABI/part gates, explicit legacy1.3 plus FE part qualification, two Off writes without temperature/RGB changes, exact dark readback, no retry after ambiguous writes or entry, three seconds of silence after entry, and mutation-free postcommit observation. The portable loader state machine and real transport each have separate suites; this adapter harness mocks their call boundaries and does not claim full end-to-end hardware qualification.

An unresolved resident response remains unresolved: the read-only observer cannot prove a loader's RAM is fresh. Interrupted journals currently require an explicit recovery workflow that is not implemented here. No flash, GPIO writes or device requests occur in these tests.

The loader's `dark_state_verified` field means effect0 and white-brightness0 were read back. Original rendering maps those settings to all-off PWM matches; legacy effect0 selects a black target and can still be completing a fade. Neither a getter nor these host tests measures instantaneous pin levels, optical darkness or completion of that legacy fade.


Brownout startup cases use the actual worker and output policy. Only reset reason
ESP_RST_BROWNOUT invokes native Off after the normal identity/ownership gate.
Both setters must succeed, fresh getters must report white0/effect0, and release
must complete before readiness. Claim, setter, ambiguous transport, readback and
release failures are held unavailable without a second startup attempt. Invalid
role/part/version receives no output writes. Ordinary reset reasons preserve
retained-output adoption. These host checks prove sequencing and failure policy,
not optical darkness or the cause of a supply-voltage drop.
