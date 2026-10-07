# Controller updater integration tests

Run `python tests/controller_worker/run_tests.py` with an AddressSanitizer-capable C compiler.

This suite compiles the actual controller worker adapter, loader core, report codec and output policy together. A buffered seven-sector model exercises the complete 448-write/448-readback path, both supported entry types, each failed transaction boundary, typed reset transitions, entry and commit silence, and strict post-update identity/dark-state checks. It rejects any FD confirmation, output replay or other lighting mutation beyond the two pre-entry Off commands.

Read-only incompatibility cases exercise the exact typed outcome used by journal rejection: unsupported firmware/status/part replies, a decoded incompatible role/part/capability, and mismatched legacy identity. Timeout, malformed typed payload, poisoned transport, non-idle lease release, final persistence failure and a failed ownership claim cannot become a safe rejection. The outcome changes to mutation-attempted before invoking claim, even when that call fails.

The separate actual-source recovery adapter harness requires a pristine driver and 33 seconds without wire operations before its sole loader-information request. It checks exact resident qualification, delivery ambiguity, raw response diagnostics, every failed native operation, and the separately admitted legacy version/status/part path. Only that proven legacy path may claim ownership, write Off, verify it and release. All loader writes, reset commands, FD confirmation and retries are forbidden by the mocks. Power-cycle admission and journal reconciliation remain manager/HTTP responsibilities tested in their own suites.

The SPI interface, native driver queries, RTOS clock, persistence and SHA boundary are mocked. Real SHA256/package interoperability lives in `tests/loader`; physical transport and worker admission/concurrency are covered separately. Passing this suite is not evidence of physical flashing or electrical qualification.
