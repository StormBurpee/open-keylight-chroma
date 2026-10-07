# Bounded diagnostic protocol tests

Run `python tests/controller_diagnostic/run_tests.py` with an AddressSanitizer-capable compiler. CTest also registers this runner.

The actual LOW1 request builders and reply decoders accept only its fixed sequence and sixteen numbered pages. Tests reject altered tags, sizes, status, channels, duration and page boundaries without changing output buffers. The independent record fixture checks all required header fields, five fixed pulse timestamps, selected and unselected matches, timer configuration, GPIO handoff and generation. Each constrained bit is mutated; sampled selected-channel GPIO levels and unrelated register bits remain explicitly unconstrained.

These tests establish format and register predicates only. They do not execute a worker sequence, establish physical pin timing, measure current or verify optical output. LOW1 remains a diagnostic role with recovery capability only; none of these helpers admits FD confirmation or production lighting readiness.
