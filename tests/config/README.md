# Settings API checks

Run `python tests/config/run_tests.py` with an AddressSanitizer-capable compiler
and the cJSON development library. It compiles the actual `config_api.c` with
real JSON parsing and storage spies. There are no device calls.

The tests verify exact sRGB/linear values, malformed and duplicate fields,
rejection of mixed encoding/config writes, no credential changes or reboot,
durable-before-memory ordering, revision publication, idempotence, Recording
Lock, controller readiness, an update beginning during request parsing, and
persistence failure. The services suite separately tests actual NVS helper and
boot-load code with missing/corrupt values, failures, byte preservation and
reboot reload. A static assertion pins the existing 407-byte `config_v1` ABI.

With Python `jsonschema` installed, `python tests/config/test_contract.py` checks
the corresponding OpenAPI schema boundary. Settings return 200 after persistence;
that response does not assert that the subsequent physical render has completed.
