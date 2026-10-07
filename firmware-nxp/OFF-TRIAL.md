# Native off-only qualification profile

This is a deliberately separate diagnostic image. It reports controller role 1
and recovery capability only, rejects confirmation and all lighting setters,
and always returns to the resident loader at 30,000 ms. It does not establish
PWM polarity, optical darkness or safe nonzero output merely by passing tests.

Build from the repository root with a Cortex-M0 capable LLVM toolchain:

```text
python firmware-nxp/tests/run_tests.py
python firmware-nxp/build.py --pwm-off-trial --spi-mode 3
python firmware-nxp/tests/emulate_off.py
```

The last command additionally needs the Python Unicorn emulator. It executes
the compiled image with modeled MMIO and IAP54; it does not open a device. The
optional `NXP_UNICORN_PATH` environment variable can point to an existing local
installation. Build output is under `firmware-nxp/build/pwm-off-trial/`. The
full bank is 28,672 bytes, including its explicit `ff` tail. Its manifest is
not deployment authorization: an independent review must bind the exact bank,
source, native uploader and observer before an operator uses it.

The dedicated permission mask is `0x193`: part, clock, recovery, experimental
SPI and experimental off-only PWM. Production polarity and power-limit bits
remain unset. Only the observed reference part `0x0000bc40` and mode 3 are used
by this profile. The public default and ordinary SPI-only build stay separate.

## Commands and fixed evidence

After fresh role/version/part checks and an explicit owner claim, send class
`00`, opcode `70`, exactly four bytes `OFF1`. Only one request is accepted in
each boot, before 28,000 ms. Its response must be status 2 with exactly `OFF1`.
The experiment starts only after the entire matching response is consumed.
An expired, truncated or cancelled response permanently spends the request.
Neither a duplicate command nor subsequent traffic can extend the window.

Getter `00/F0` takes no arguments and returns eight bytes:

| Offset | Meaning |
| --- | --- |
| 0–3 | ASCII `OFF1` |
| 4 | 0 unused, 1 response pending, 2 response consumed, 3 spent |
| 5 | zero |
| 6–7 | big-endian 400 ms |

This getter reports protocol state, not timer or optical verification.

Getter `00/F1` takes one page index, 0 through 13. It returns exactly 72 bytes:
ASCII `OFF1`, page index, page count 14, payload length 64, zero, then that fixed
64-byte slice. It accepts no address or variable length. Read and assemble all
14 pages only after the record says completed or failed; final records are
immutable while the application remains running. Ownership rules still apply.

The assembled 896 bytes contain 224 big-endian words. Words 0 through 15 are:

| Word | Meaning |
| --- | --- |
| 0 | `0x4f464631` (`OFF1`) |
| 1 | record ABI 1 |
| 2 | phase: 0 idle, 1 running, 2 completed, 3 failed |
| 3–6 | request, mux, GPIO-dark and fixed deadline times in milliseconds |
| 7 | accepted experiment count, zero or one |
| 8 | reason: 0 none, 1 deadline, 2 owner loss, 3 SPI fault, 4 register failure, 5 cancelled, 6 recovery |
| 9 | SPI error counter |
| 10 | snapshot presence bits 0 through 4 |
| 11–12 | final CT32B0 and CT32B1 TCR values |
| 13 | exact permission mask `0x193` |
| 14 | immutable recovery deadline 30,000 ms |
| 15 | nonzero retained generation counter |

Words 16 through 215 hold five 40-word snapshots. Words 216 through 223 are
zero. Snapshot order is GPIO low before timer configuration; running off timers
before mux; timer mux connected; GPIO low before timer stop; timers stopped.
Each snapshot contains:

1. Time, reason, AHB clock-enable register, GPIO direction and pad-value words.
2. IOCON values for P0_13, P0_14, P0_16, P0_18 and P0_19.
3. Fourteen CT32B0 words, then fourteen CT32B1 words, at offsets
   `04,08,0c,10,14,18,1c,20,24,28,3c,70,74,00`.
4. Phase and SPI error counter.

The retained generation uses only two words at USB SRAM `0x20004000..007`;
the resident recovery magic at `0x200047fc` is untouched until recovery. The
counter helps correlate records. It is not authentication, an immutable boot
identifier or proof of a hardware reset.

## What the observer must verify

Require the initial idle record, count zero, exact ABI/mask/deadline and a
nonzero generation. After the one accepted request, require the same generation,
phase 2, reason 1, count one, all five snapshots and a 400 ms logical window.
Validate every snapshot's fixed fields rather than trusting the phase alone.

White uses CT32B0, prescaler 47, MR2 254, MR0/MR1/MR3 255 and PWMC 3. RGB uses
CT32B1, prescaler 0, MR2 25499, MR0/MR1/MR3 25500 and PWMC 11. Both have only
MR2-reset enabled in MCR (`0x80`), CTCR/CCR zero and EMR controls cleared.
Timers run while pads are still GPIO low before connection. The selected IOCON
functions are 3,3,2,2,2; final GPIO functions are 1,1,0,0,0. ADC-capable pins
13/14/16 also require digital-mode bit 7. Other preserved IOCON bits are not
assumed zero. All five final GPIO directions are outputs and all pads read low.
Both final TCR words must be 2. Counter and prescaler-counter readings are live
snapshots, not fixed constants; IR is a write-one-to-clear register.

SysTick performs the 400 ms GPIO handoff independently of the main loop. It
also invokes resident recovery at 30 seconds. Short, bounded interrupt-masked
regions can delay delivery; neither tests nor register snapshots guarantee
recovery from a permanently masked or failed CPU. If GPIO handoff fails, the
record fails and the already off-configured timers keep running, rather than
stopping a potentially exposed output latch.

The native update operation must keep its journal and exclusive SPI ownership
through this diagnostic. Never send `FD`. After final evidence, stop application
traffic through the expected reset and the resident-loader quiet interval,
then require exact loader information. A journal must not be cleared based
only on arbitrary loader information or on the F0 protocol-state getter.

## Next physical gates

First observe the all-off test and validate its captured register record. A
separate reviewed low-output profile can then test one named channel at a time,
at most 1% nominal duty for at most 100 ms, with an independent deadline,
owner-loss shutdown, no re-arm and the same 30-second recovery. It must not
accept arbitrary compare values or claim production lighting readiness.

Only the resulting board observations can establish channel mapping and polarity.
The complete production envelope must then be validated against the retained
controller's known operating range. Absolute current, thermal limits and
waveform timing remain separate from protocol tests. No low-output experiment
is implemented or enabled by this off-only profile.
