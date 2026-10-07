# Native SPI transport recovery tests

Run `python tests/transport/run_tests.py` with an AddressSanitizer-capable Clang in PATH (or `CC`). The tests compile the actual `firmware/main/nxp_transport.c` and `okl_nxp.c`; they replace only ESP-IDF hardware/RTOS boundaries. The runner records tested source digests and results under `build/transport-tests/`.

The regression starts from an ordinary driver request receiving a two-byte zero length. That remains a failed getter, but recovery waits for READY high without inventing a body read. It preserves the consumed-length boundary across timeout, then admits a subsequent correlated getter. Startup stale replies, delayed stock replies, malformed lengths, mutex/deadline failures and SPI errors are also exercised.

An original controller may expire an unconsumed reply after 100 ms. If the ESP last recorded a sent request and currently sees READY high, the transport cannot distinguish that expiry from a stock response still being prepared. Tests require bounded failure and retained recovery poison in this case. A future explicit peer/lifecycle contract is needed before changing it; elapsed time and a high pin alone are not evidence of a known idle peer.

These host tests do not qualify physical pin timing, resolve ambiguous SPI/DMA failure phases, or establish the peer's pending length/body boundary after an ESP reset. Recovery assumes a startup READY-low response is awaiting its length, as the pre-existing implementation does. No flash, reset, GPIO writes or device requests are performed by the tests.
