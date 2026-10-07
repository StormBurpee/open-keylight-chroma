# Controller update job and recovery journal

`firmware/main/controller_job.c` owns admission and status, independently of the HTTP receiver and the worker's exclusive SPI execution. Status reads copy cached data under the application mutex and perform no controller transactions. Host tests exercise the actual job manager and package validator; they do not establish physical flash reliability or electrical suitability of an uploaded application.

The HTTP receiver first reserves a job. Reservation shares `app.updating` with ESP application updates, so two update types cannot be admitted together. Receiving an incomplete or invalid package does not create a controller mutation intent. Submission validates the exact package length, header, conservative vector layout and complete bank digest. Only a `202` result transfers allocation ownership. The worker borrows the immutable package; the job manager frees it exactly once at terminal completion. Cancellation releases only the matching receiving reservation and cannot cancel an admitted job or another job's reservation.

The worker requalifies the cached source through fresh typed controller observations. No HTTP field chooses a loader source. The current implementation does not admit a controller found only in the resident loader: an information reply cannot establish fresh loader RAM, and an old pending sector can survive there. See the portable loader's reset-boundary requirements.

## Durable intent

Before the core enters or mutates the controller, it persists a versioned 128-byte record under `openkeylight/nxp_job_v1`. The record contains the job ID, source, phase, result, target bank digest and version, acknowledged/readback block counts, and mutation-intent flags. Its fields have explicit byte offsets and a SHA-256 integrity digest; it contains neither pointers nor compiler-dependent structures. The digest detects corruption, not malicious modification.

The core writes at phase boundaries, not after every 64-byte transfer. Cached counters can therefore be newer than the last durable checkpoint. Both values describe evidence, never an instruction to continue programming.

On startup, absence of the record permits ordinary worker initialization. A pending record, wrong size, bad digest, unknown format, or NVS read failure blocks automatic controller bootstrap, confirmation and output. A pre-commit record cannot reveal whether the commit reached the controller, so reloaded commit intent is reported as `maybe_sent`. No job is reconstructed from the journal and no upload buffer is retained across resets.

Journal clearing requires all of the following: complete 448-block programming and readback, verified loader entry, one known-complete commit, the full quiet interval, an established reset boundary, a matching original application observed dark, and the worker's separate typed trial confirmation. Clear/commit failure leaves the running application blocked even when the controller confirmation succeeded. Interrupted and failed mutations never trigger an automatic restore, confirmation retry, or replay of an earlier desired light state.

The current API has no journal-clearing recovery endpoint. Recovery requires a separately reviewed procedure with a proven fresh controller/transport boundary. A power cycle alone does not authorize deleting the journal. This intentionally limits recovery automation until that procedure exists.

## Verification

Run `python3 tests/controller_job/run_tests.py` with an ASan-capable C compiler and the cJSON development library. The suite uses the real manager and package parser with deterministic NVS, SHA-library and locking boundaries. It covers malformed packages, stale reservations, exactly-once allocation release, every record byte corrupted in turn, truncated records, pending records across simulated reboots, storage/hash failures, ambiguous persistence, and incomplete or unconfirmed terminal evidence. It also verifies that cached JSON reads issue no NVS operations.

The NVS mock models both a failed commit that leaves the old record and an error after the new record became durable. Atomic storage guarantees, scheduling under load and physical power interruption remain hardware/ESP-IDF qualification concerns.
