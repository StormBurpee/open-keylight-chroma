# Controller updates

An NXP controller update must own the complete internal transport until installation and reset have finished. Receiving the final program acknowledgement or verifying staging does not mean the active application has been installed.

The retained loader installs an entire 28,672-byte application bank. A prepared update must therefore include an exact complete bank, verify every staged byte, and issue the commit once. A timeout after commit is an uncertain outcome; it must not trigger another commit or an automatic restoration.

## Protect the commit interval

After attempting the commit send, keep the existing update connection open and silent for **at least three seconds**. This is the conservative interval exercised during bring-up, not a claimed minimum for every controller variant. Apply it even when the send returns an error: some or all command bytes may already have reached the device.

During that interval:

- Do not open or close controller connections, send a greeting, poll status, or request a reset.
- Suspend animation and other work that can produce internal SPI traffic. Other clients must not bypass the update's exclusive transport ownership.
- Defer orderly cancellation cleanup until the interval ends. Closing a connection is not necessarily harmless: the compatibility bridge generates controller traffic for connection events.

Then observe the expected application or recovery endpoint. Check its identity and a fresh response; connection acceptance alone does not prove controller health. Preserve the update audit before deciding on any separate recovery operation.

The reason is concrete: flash installation masks controller interrupts. New SPI traffic can overrun reception while the loader cannot service it. The installed loader's error path can leave a receive interrupt pending and prevent installation from reaching its final reset. NXP documents a 95–105 ms sector-erase operation, before programming and other update work, so an immediate reconnect is inappropriate. [NXP LPC11U3X data sheet, Table 9](https://www.nxp.com/docs/en/data-sheet/LPC11U3X.pdf)

## What has been qualified

The corrected SPI-only application completed a confirmed 60-second hardware run on the reference LPC11U35/501: 153 of 153 health getters passed, and the controller recorded 163 complete request/reply exchanges with no phase errors, expiry or overrun. An owned recovery command then returned it to the retained loader at 63,154 ms, without a power cycle. A separate trial verified the same recovery command before the 30-second automatic deadline. The previous working application was restored after each trial. These results qualify those bounded communication and recovery cases; the diagnostic images contained no PWM code and do not establish lighting output or long-term reliability.

The earlier experiments below explain why both the installation quiet interval and corrected SPI receive boundary are required.

On the reference LPC11U35/501 board, a minimal original application was installed, wrote a fresh entry record, and returned to the resident loader through software reset. This succeeded with the three-second quiet interval and no physical power cycle. An earlier attempt with a reconnect about 25 ms after commit did not return a controller response. The two diagnostic images differed only in their record identifier.

This establishes the minimal application-entry and recovery path for that board and the need to protect installation from other transport activity. It does **not** qualify the full replacement application's SPI timing, watchdog behavior, lighting channels, PWM, current limits, or every failure mode. Consult [the NXP application qualification notes](../firmware-nxp/README.md) for those separate gates. Emulator and host-test results remain distinct from measurements on hardware.

A later SPI-only candidate also returned to the resident loader without a power cycle, approximately 30.5 seconds after commit. No application reply was received and no trial confirmation was sent. That trial was consistent with the 30-second recovery guard but did not establish SPI interoperability.

Subsequent candidates produced original application version and part replies. A trace-only candidate also acknowledged an explicit ownership claim and returned its exact owner/name through a getter. Its retained RAM record ended at the 30,000 ms recovery deadline, and the previous working application was restored. The trace identified premature reply cancellation at a receive/chip-select boundary; ownership release was not verified because its guard getter failed. Correcting this boundary race led to the successful communication trials above.

This repository contains original source and documentation. It does not redistribute the installed loader, vendor application binaries, or private device captures. The public controller application builder is not an installer; follow the explicitly reviewed qualification procedure for a particular candidate and board.
