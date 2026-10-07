# ESP update and trial tests

Run `python tests/update/run_tests.py` from any directory. Set `CC` if Clang
is not on PATH. The harness compiles the actual `firmware/main/update.c`
with AddressSanitizer and deterministic IDF boundary mocks. It writes the
test executable, results and exact source hashes under `build/update-tests/`.

The test covers both orders of the confirmation/expiry race, a confirmation
whose NVS commit crosses the deadline, failed confirmation with and without
time remaining, duplicate calls, task-allocation failures, interrupted and
slow-drip uploads, every receive fragment size from1 through300 bytes,
SHA API failures, image digest mismatch, OTA errors, and socket-budget cleanup.
The mocks validate resource lifetimes and ensure boot selection never occurs
before image validation and reboot-task allocation. They do not replace
IDF integration tests, flash fault testing, or a physical trial.

The same harness links the actual progress-indicator module. Snapshot checks
at hash, flash-write, image-validation and boot-selection boundaries prove that
received bytes publish only after a successful write and remain below full
progress until boot selection succeeds. Failed uploads retain their terminal
generation and successfully written byte count; rejected admission leaves that
record unchanged. A new explicitly accepted upload gets a new generation.
Snapshots are locked copies. A lost HTTP acceptance response cannot undo an
already verified image or stop the independent 1.5-second reboot task. The tests
do not exercise physical indicator colour or worker rendering.

Trial decisions are serialized by the application mutex. A confirmation that
reserves the decision before180 seconds may finish its NVS operation after
the deadline. Expiry waits for that result: successful persistence confirms,
failure returns to pending so expiry can choose fallback. Confirmation cannot
win once fallback has been elected. A failed fallback remains unconfirmed
and blocks OTA from overwriting the recovery slot. The atomic pending accessor
is safe when its caller already holds the application mutex.

An update has a120-second total processing budget, checked around receive,
hash, flash-write and validation boundaries. Each receive is limited to the
remaining budget or five seconds, whichever is smaller. A blocking IDF flash
operation cannot be preempted by this handler; an overrun detected on return
rejects the update before boot selection. The original socket receive timeout
is restored. A reboot task is allocated before selecting the new boot slot
and is notified only after attempting the acceptance response.

The installed older ESP bootloader does **not** implement automatic trial
confirmation or rollback. This application-level180-second fallback only works
if this application reaches and continues running its trial task. A valid
image that crashes before startup can still require external recovery. These
tests do not remove that limitation or make NVS writes power-loss atomic across
multiple keys.
