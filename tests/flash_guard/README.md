# Flash and controller admission guard

`python tests/flash_guard/run_tests.py` compiles the actual coordinator with AddressSanitizer and UndefinedBehaviorSanitizer. Deterministic FreeRTOS mocks exercise uninitialized and failed initialization, idempotent initialization while held, exact absolute deadline boundaries, the five-second cap, delayed scheduling after a successful take, failed takes, and exclusion until a complete modeled controller reply ends. Both 1 ms and 10 ms tick configurations run.

These tests prove the coordinator's ownership and cleanup contract. They do not prove that every production caller uses it: transport, storage, controller-journal and OTA suites separately check call-site coverage. The lock order permits existing NVS writers to hold `app.mutex` while waiting only because a SPI guard holder never takes `app.mutex`. No test opens a device or network connection.
