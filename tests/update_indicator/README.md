# ESP upload indicator tests

`run_tests.py` compiles the portable indicator state/colour sampler and the actual
output coordinator with AddressSanitizer. Coordinator tests use the real command
builders and deterministic driver boundaries; no sockets or devices are used.

The math suite checks upload generations, monotonic accepted-byte progress, failure
and terminal states, integer overflow boundaries, the reserved verification
endpoint, periodicity, symmetry, channel limits and the breath's turning points.
At a fixed breath phase, increasing progress never shifts the colour back toward
blue. The sample stream covers 213 distinct rounded blue values per breath.

The required native RGB master is **12/255**, with white off. The RGB frames use
43–255 blue and up to 170 red to avoid limiting the animation itself to twelve
byte values. These are commanded values; electrical PWM resolution and visible
smoothness require separate device qualification.

The coordinator is called only by the existing lighting worker. It mutes the
inherited master, installs custom mode and an acknowledged frame, then enables
the low master. It samples at most one frame every 20ms without catching up a
backlog. Ownership, mode, white-off and master settings are rechecked every
250ms; frame acceptance is an ACK, not a framebuffer or optical measurement.

Progress is capped at 99% until SHA, application validation and boot-slot
selection all succeed. Verified uploads have a 300ms purple dwell, followed by
bounded restoration ending no later than 1400ms after verification. The separate
reboot task still runs after its 1500ms grace and never depends on animation.
Failed accepted uploads get two low red pulses over 1600ms, including a failure
which happened before the worker first observed that upload generation.

A fresh native snapshot is restored only with the same desired revision and a
coherent owned controller. Recording Lock suppresses the indicator; a newer Off
interrupts it between output commands. Unknown external custom framebuffers and
unsupported mixed states are left untouched. A prior in-memory renderer can
resume after precommit failure; after a successful upload its last ACKed frame
is parked as native Static before reboot. There is no saved-scene NVS record or
automatic replay across boot.

The suite injects faults and cancellation at setup/restore boundaries, verifies
one lease-cleanup attempt, stale-generation suppression, no ownership reclaim,
no full-master setup flash, exact restoration and the fixed reboot deadline.
Receiving and failure handoffs charge actual wire time but exclude each validated
flash-admission wait exactly once. The tests inject waits at every driver
boundary, including compound reads and lease cleanup, and ensure a credited
wait never converts a failed exchange into success. Verified uploads instead
have a hard admission/wire cutoff; tests cover verification published while an
older receiving snapshot waits behind flash and preservation of stricter caller
deadlines. The native transport suite separately tests the real admission path.
`tests/worker/run_tests.py` additionally exercises the actual worker integration,
including pending Off, lock, failure red, verified purple and renderer resume.

Hard crashes, power loss, unknown transport state and deadline exhaustion can
prevent the indication or restoration. They do not cause retries or delay boot.
NXP updates do not create this ESP indicator and exclude cosmetic SPI work while
the controller is being rewritten. Visible pulse smoothness requires separate
device qualification.
