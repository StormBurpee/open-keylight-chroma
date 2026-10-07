# Original NXP application — qualification work

This is the independently written Cortex-M0 half of **Open Keylight Chroma**. It has a portable command endpoint, bounded RGB/white arithmetic, a real but gated LPC11U-family register adapter, startup/vector/linker code and host tests. No vendor code, firmware, disassembly, device identity or private trace is included.

**The default image must not be installed.** It reads ROM part ID and clock configuration into RAM but leaves its physical SPI and PWM adapters disabled. An image that cannot service the installed recovery protocol is not a useful remote diagnostic. Hardware gates are intentionally unset; passing simulations must not set them.

## Implemented software

`nxp_app.c` implements one 97-byte SPI request/reply, strict 90-byte inner reports, transaction echo, XOR validation, owner identity/name, version and mode, RGB/white brightness, temperature, Static effect, custom RGB frames and an explicit recovery request. It rejects persistent profiles, unsupported effects and flash commands. State mutation is atomic with respect to validation; invalid framing produces no reply. The NXP version is the original application's `0.1.0.0`, not a vendor version.

The compatible wire selectors are profile 0, RGB region 0 and white region 32. Color state is effect 1 or 8; effect 0 permits white mode. Temperature is 3000..7000 K. White brightness is limited to 38/255 while RGB is enabled. Switching from bright white to RGB must first lower white brightness. Custom frames explicitly set white to zero. A custom-effect getter reports mode only, not the physical RGB framebuffer.

Explicit claims use the client's stable six-byte SPI tag. The owner report accepts the documented 72-byte form and the existing ESP bridge's 80-byte form with eight trailing zero bytes. Releases are allowed only from that owner; this tightens the older unconditional-release behavior. The endpoint interoperates with the original ESP driver in host integration tests. ACK means a request was accepted into state; it is not optical verification.

The existing ESP bridge also sends nine-byte connection events (kind11, connected flag, connection count). The first connection can claim an unowned controller; disconnect releases only its matching owner. Changes produce a kind4 owner notification. Other valid connection events still complete the READY/length handshake with length zero. They never change lighting or confirm the startup trial. Malformed lengths, flags and identities are rejected.

The pure rendering function keeps white compare values in 0..255 and RGB in 0..25500. Color scaling uses `floor(channel * brightness * 100 / 255)`, retaining more precision than the older percentage-quantized behavior within the same range. White temperature interpolates the lower contribution on either side of 5200 K; both white contributions can be full at 5200 K. This preserves an observed command/output envelope, not a measured current or photometric calibration.

`nxp_board.c` maps five timer channels, SSP1, ready GPIO and the transaction-boundary input through mockable register callbacks. It queues reply length and body as one transmit stream before asserting READY, so back-to-back length/body reads require neither a main-loop poll nor an IRQ between their chip-select pulses. Cumulative receive handling validates both logical phases, rejects truncated/overrun exchanges and expires abandoned replies. Transaction completion uses the select monitor: a preloaded slave transmit FIFO can keep SSP's BSY bit set after deselect. Every loop is bounded; interrupt latency and physical signal timing still need hardware validation.

The first installation encountered a retained-loader receive-overrun bug during commit. A quiet interval on the existing connection allowed a minimal original application to start and request software-reset recovery. A subsequent full SPI-only trial returned to the loader approximately 30 seconds after installation without a power cycle, but produced no application replies. The current host tests reproduce and correct a separate reply-handoff race: the previous adapter reset/refilled SSP between length and body while the ESP could already clock the next bytes. This correction awaits hardware testing; application SPI and PWM remain unqualified. See [controller update evidence and procedure](../docs/controller-updates.md).

The qualified PWM start prepares GPIO low, then dark timer compares, then PWM mux, then timer enable. The hard-fault handoff switches back to prepared low GPIO; stopping a high PWM timer alone is not used as an off mechanism. P0_13/14 use GPIO function 1, P0_16/18/19 use function 0, and ADC-capable pins are set to digital mode. These paths remain gated by polarity/board qualification.

## Memory and recovery contract

The linker targets the resident loader's application range `0x2000..0x8fff`; it never supplies or replaces the lower loader sectors. The first 192 SRAM bytes are reserved for its 48 remapped vectors. The current conservative layout uses only the first 4 KiB SRAM, stack top `0x10001000`, and reserves at least 1 KiB for stack. The bootloader already establishes the vector remap. Application startup initializes its own data/BSS and does not rewrite loader flags or EEPROM.

The original endpoint accepts class `00`, opcode `04`, argument `01` only from its current owner. It defers the recovery handoff until the full response body has been consumed. A timeout or malformed read cancels an unacknowledged recovery request. The platform then drives qualified outputs low, writes the resident loader's volatile recovery magic at `0x200047fc`, and requests a system reset. Startup-timeout recovery has been observed from the original application; command-triggered recovery still needs application SPI qualification. Neither guarantees recovery from an arbitrary bad image.

Every startup is a 30-second bring-up trial. The controller must claim ownership, establish protocol/part/controller health, then send class `00`, opcode `FD`, four argument bytes `4f4b4c43` (ASCII `OKLC`). A successful confirmation replies with status 2 and one byte `01`. This is identity-gated over the internal SPI link, not cryptographic authentication. Malformed, foreign, unclaimed or late confirmation is rejected. All optical output stays dark until confirmation, even if lighting commands have already updated state. Getter `00/FE` returns the observed four-byte big-endian IAP part ID, or an error if none was obtained.

With an explicitly approved actual part and recovery gate, failure to confirm by 30,000 ms, a clock/startup failure or a hard fault returns to the retained loader. The SysTick timebase is configured before SPI work. Recovery explicitly enables the USB SRAM clock, writes the magic and uses software reset; a watchdog reset alone does not satisfy the retained loader's magic check. If the early reset-status snapshot still reports a watchdog reset, the qualified application also requests that software-reset recovery path. These are implemented guards requiring hardware qualification, not protection from an invalid vector or arbitrary failure before startup executes.

The main loop services an already enabled watchdog only when its counter is inside the configured feed window. It preserves interrupt state around the `AA/55` feed, never changes watchdog timeout/mode, and never feeds from the SPI ISR. Inherited clock/window behavior remains a board test requirement. A stalled loop therefore does not keep feeding merely because interrupts continue.

The retained loader's install operation copies an entire 28,672-byte staged bank. Preparing an eventual deployment requires a complete image with deliberately specified tail fill, exact staging verification and a separate reviewed commit operation. The default builder emits only the unqualified application image. It contains no uploader.

An explicit `python firmware-nxp/build.py --spi-only-trial --spi-mode 3` builds a separate experimental link/recovery trial for the observed `0x0000bc40` part. Its distinct permission flag allows testing the unqualified SPI timing without claiming that timing has been proven. GPIO outputs are prepared low; PWM start/apply routines and timer peripheral addresses are excluded from the linked image. It also emits an exact 28 KiB bank padded with `ff`, under `build/spi-trial-mode3/`. This is not a general deployment release. Link-qualification trials omit confirmation and observe the 30-second return to the resident loader before an independently reviewed, explicit restore. No automatic commit retry or restoration is supplied here.

## Hardware facts and remaining gates

The reference lamp's actual ROM IAP54 result is `0x0000bc40`: **LPC11U35/501**. NXP UM10462 Table 377 maps that ID to both the FHI33 (HVQFN33) and FET48 (TFBGA48) packages, so the physical package remains unconfirmed. The variant provides 64 KiB flash, 4 KiB EEPROM and 12 KiB SRAM split into 8 KiB main, 2 KiB USB and 2 KiB additional SRAM. The runtime probe uses ROM IAP command 54 at Thumb entry `0x1fff1ff1`, accepts result word 1 only if status word 0 is zero, and runs while interrupts remain masked. This reads identification; it does not program flash. The direct DEVICE_ID register is not substituted for IAP identification.

| Signal | NXP pin / register |
| --- | --- |
| Cool / warm white | P0_18 / P0_19; CT32B0 MR0 / MR1 |
| Blue / green / red | P0_13 / P0_14 / P0_16; CT32B1 MR0 / MR1 / MR3 |
| SPI MOSI / MISO | P0_21 / P0_22 |
| SPI clock / select | P1_15 / P1_19 |
| Ready / transaction monitor | P0_9 / P0_17; physical nets still require qualification |

The configured clock expectation is a 12 MHz oscillator and 48 MHz core. The adapter refuses any other declared clock; startup also checks the current PLL/clock registers. It does not currently reconfigure the PLL or flash wait states. White PWM uses prescaler 47, period match 254; RGB uses prescaler 0, period match 25499. These imply approximately 3.922 kHz and 1.882 kHz carriers at the expected clock; they are calculated values, not measured waveforms.

The recorded ESP SPI master is mode 0; the old NXP peripheral uses CPOL/CPHA both set. Both sample rising edges, which is insufficient to establish first-bit and chip-select behavior. The new adapter requires explicit selection of mode 0 or 3 after qualification. No automatic trial of alternate modes or pins is performed.

`platform/qualification.h` deliberately approves no part and no board flags by default. Separate flags cover observed part, clock, SPI wiring, SPI mode, recovery, PWM polarity and power limits. Normal SPI requires the first five; PWM requires all seven. The explicit SPI-only trial permission is separate and never enables PWM. Candidate MCU IDs must not be treated as observed IDs. The builder verifies the exact gates appropriate to the selected build so an enabled image cannot be labelled inert.

Before any deployment: identify the actual part; prove an available recovery path; qualify SPI with outputs held low; validate clock and watchdog handoff; verify low-state mux ordering and timer readbacks; then test each channel at low duty before validating the maximum envelope. Absolute current, thermal behavior, external bias and PCB routing remain unmeasured. There is no identified NXP touch input in the existing interface; no unused pin is assumed free.

Register semantics and the ROM identification ABI are documented in the [NXP-authored LPC11Uxx manual, UM10462](https://www.farnell.com/datasheets/1847085.pdf). Port-to-lamp roles come from the prior private interface investigation, not a published board schematic.

## Reproduce the software checks

From the repository root:

```text
python firmware-nxp/tests/run_tests.py
python firmware-nxp/build.py
```

The first command needs clang and uses C99, strict warnings and AddressSanitizer. It exercises parser bounds/checksums, all 16-bit temperature inputs, RGB brightness arithmetic, white mixing, ownership and recovery sequencing, complete exchanges against the original ESP driver, qualification gates and register/FIFO simulations.

The second needs clang, lld and llvm-objcopy supporting `arm-none-eabi`. It produces ELF, application binary, map, stack-usage records and a manifest under ignored `build/`. No dependency or device tool is downloaded or executed. The image is explicitly marked non-deployable, with source hashes and limitations. The current compiler records give a 624-byte ordinary call chain through connection-event handling, including the C entry frame; ROM IAP and exception/platform behavior need separate qualified bounds. The linker reserves at least 1024 stack bytes.
