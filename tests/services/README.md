# Pairing, HTTP and network service tests

Run `python3 tests/services/run_tests.py` on a host with a C compiler,
AddressSanitizer and the cJSON development library (`libcjson-dev` on Ubuntu).
The runner compiles the actual storage, HTTP and network source files with
deterministic IDF boundaries. Core validation, policy checks and JSON parsing
use the real project code and cJSON. Results and source hashes are written to
`build/service-tests/result.json`.

The storage/HTTP suite pairs and authenticates 40 clients, exercises capacity
bounds derived from the actual NVS partition, and checks one-use pairing windows,
expiry, reload, individual revocation and physical clear with Wi-Fi preserved.
It covers v1 migration (including formerly accepted label padding), authoritative
empty v2 records, hostile lengths/counts/duplicate IDs, allocation/hash failures,
and set/commit errors both before and after a complete record becomes durable.
Uncertain additions preserve existing authentication; uncertain revocation/clear
blocks authentication until reload. Further mutations cannot overwrite ambiguous
persistence. HTTP tests distinguish closed-window 403, resource 507 and storage
503 errors, and prove token responses need no JSON allocation after issuance.
They also cover token non-disclosure, authentication and Host/Origin rejection,
fragmented JSON, duplicate fields, NUL escapes, trailing input, body limits
and a final fragment that crosses the deadline.

The actual scene-store code is included as well: four exact persisted defaults,
no activation, one-time migration, existing IDs and edited default names, full
collections, deletion without reseeding, unsupported/corrupt records, namespace
absence and read/write/commit faults. A failed commit is tested both before and
after the complete blob becomes durable; further edits remain blocked until
reload. These mocks model the blob boundary, not physical flash power loss.

The separate output-encoding byte is tested for absent, invalid and unreadable
values, explicit sRGB/linear reload, invalid enum rejection, persistence failure,
handle cleanup and preservation of configuration and paired-client bytes.

The network suite exercises the actual periodic reconnect task with a virtual
clock: offline physical setup, continued AP access after pairing consumes its
window, independent180-second AP expiry, failed AP configuration and cleanup,
repeated cleanup after mode-switch failure, ordinary reconnects preserving
credentials, and unconfigured first-boot setup. No network traffic is sent.
It also verifies flash exclusion around synchronous Wi-Fi initialization and
first-start PHY calibration, both admission/call failures, and release before
later setup. Calibration persistence itself is provided by IDF, not emulated.

The NVS mock models failed writes and commits; it cannot prove physical flash
power-loss behavior. The digest stand-in is deliberately not a cryptographic
implementation: these tests verify service control flow and fail-closed error
handling, while SHA256 and randomness are provided by ESP-IDF on the target.
Radio behavior, bootloader recovery, physical buttons and electrical outputs
still require target qualification.
