# Controller updater integration tests

Run `python tests/controller_worker/run_tests.py` with an AddressSanitizer-capable C compiler.

This suite compiles the actual controller worker adapter, loader core, report codec and output policy together. A buffered seven-sector model exercises the complete 448-write/448-readback path, both supported entry types, each failed transaction boundary, typed reset transitions, entry and commit silence, and strict post-update identity/dark-state checks. It rejects any FD confirmation, output replay or other lighting mutation beyond the two pre-entry Off commands.

The SPI interface, native driver queries, RTOS clock, persistence and SHA boundary are mocked. Real SHA256/package interoperability lives in `tests/loader`; physical transport and worker admission/concurrency are covered separately. Passing this suite is not evidence of physical flashing or electrical qualification.
