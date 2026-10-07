# Lighting worker and controller-update adapter tests

Run `python tests/worker/run_tests.py` with AddressSanitizer-capable Clang. The runner compiles the actual worker, app startup, output policy, state arithmetic and controller-update adapter in three deterministic host executables. Results and source hashes are saved under `build/worker-tests`.

The worker tests cover exact native readback, ownership and transport errors, trial qualification and uncertain confirmation, reset detection without replay, and the update gate. A pending journal prevents even SPI initialization. A taken controller job excludes ordinary rendering/health; receiving reservations keep accepted Off and health functional. Post-update confirmation does not open readiness until durable journal completion succeeds, and the old desired scene is replaced by fresh dark readback.

The adapter tests execute `controller_worker.c` against bounded protocol/RTOS mocks. They verify original identity/ABI/part gates, explicit legacy1.3 plus FE part qualification, two Off writes without temperature/RGB changes, exact dark readback, no retry after ambiguous writes or entry, three seconds of silence after entry, and mutation-free postcommit observation. The portable loader state machine and real transport each have separate suites; this adapter harness mocks their call boundaries and does not claim full end-to-end hardware qualification.

An unresolved resident response remains unresolved: the read-only observer cannot prove a loader's RAM is fresh. Interrupted journals currently require an explicit recovery workflow that is not implemented here. No flash, GPIO writes or device requests occur in these tests.

The loader's `dark_state_verified` field means effect0 and white-brightness0 were read back. Original rendering maps those settings to all-off PWM matches; legacy effect0 selects a black target and can still be completing a fade. Neither a getter nor these host tests measures instantaneous pin levels, optical darkness or completion of that legacy fade.
