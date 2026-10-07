# Reference-board lighting build

The original lighting application is a separate build profile for the observed LPC11U35/501 reference board (`0x0000bc40`, inherited 48 MHz clock, SPI mode 3). The default image remains inert. OFF1 and LOW1 retain their separate, unconfirmable diagnostic profiles.

```text
python firmware-nxp/tests/run_tests.py
python firmware-nxp/build.py --reference-lighting
python firmware-nxp/tests/emulate_production.py
```

The build requires ARM-capable Clang and LLVM objcopy. `CLANG` and `LLVM_OBJCOPY` can select installed tools; `CC` selects the host test compiler. The emulator additionally needs Unicorn, optionally through `NXP_UNICORN_PATH`. These commands perform no device operations.

Output is under `firmware-nxp/build/lighting/`. `lighting-bank.bin` contains the complete 28,672-byte application bank, including its explicit `ff` tail. The manifest pins the source and image. No bootloader, calibration, credential or proprietary firmware bytes are included in this repository. The manifest's `deployable:false` prevents a successful build from being mistaken for hardware qualification or an installation instruction.

## Qualification and limits

The explicit production mask `0x7f` covers part, clock, SPI routing/mode, resident recovery, observed PWM polarity and the stock-derived output policy. It is appropriate only after the target's OFF1 and LOW1 results are recorded and reviewed. OFF1 must establish all-off register setup and GPIO-low handoffs, with observed darkness. LOW1 must establish all five exact 100 ms channel records without errors, a timed resident return, and an operator-observed red, green, blue, warm-white, cool-white sequence with darkness between channels. Software records alone do not identify the emitted light.

The reference lamp completed those native diagnostic stages during development. This does not qualify another board revision automatically. Nor do short diagnostic pulses establish a new continuous-current or thermal rating. The lighting implementation preserves the existing timer and per-channel duty envelope rather than increasing it; a sustained full-output thermal test is a distinct observation.

CT32B0 drives white with prescale 47 and MR2 254. CT32B1 drives RGB with prescale 0 and MR2 25,499. P0_13/14/16 are blue/green/red; P0_18/19 are cool/warm white. PWM is inverse: an output match above the period commands low, while a zero match permits full duty.

Each white channel can reach 255 units. At 5,200 K, **both** warm and cool channels reach the requested white level; the stock-style mixing function does not conserve a 255-unit sum. Whenever any RGB output is nonzero, each white channel is capped at 38. RGB and white rendering, including rounding, are covered by the actual protocol/output-policy/driver integration tests. These bounds are a stock-derived operating policy, not a new electrical certification.

## Output and recovery behaviour

Startup first obtains verified GPIO-low ownership, clears inherited timer modes, starts both timers with off compares, and only then selects PWM pin functions. The physical renderer stays dark until an owned, typed `00/FD` confirmation. Accepted colour state before confirmation cannot light the lamp, and startup does not replay a stored scene.

For each frame, the production profile writes and verifies every commanded duty reduction before any increase. This prevents RGB from increasing while an old full-white command remains. Intermediate white command sums do not exceed the greater of the old and new frame. Timer latches still follow their hardware match/reset boundaries; the register model does not establish sub-cycle optical behaviour.

Off/recovery transfers pads to verified GPIO-low before stopping timers. If that handoff fails, the fallback sets output compares above the normal periods and leaves timers running rather than freezing a potentially high latch. The fault stays sticky and the platform requests recovery. This is a best-effort response to a hardware fault, not a guarantee against a broken GPIO, timer or halted processor.

An unconfirmed application returns to the resident loader at 30 seconds. Confirmation intentionally disables that trial timeout. An owned `00/04` request still returns to the loader after its full response is consumed. Wrong part, failed IAP identification, invalid inherited clock, watchdog-reset startup and failed PWM setup also use the established escape. Application recovery requires working CPU/interrupt execution; it is not immutable boot-ROM rollback.

The native updater must verify the complete bank, issue End once, preserve the quiet interval, observe the exact original role 2/capabilities 3 identity and dark state, confirm once, and obtain fresh state before clearing its journal. Neither an ambiguous transfer nor a diagnostic package can be treated as a successful lighting installation. The resident capability obtained after a diagnostic is volatile; an ESP restart requires the explicit recovery procedure again.

## First-use checks

Start with explicit Off, low-level pure RGB channels and warm/cool/middle-white settings, returning to Off between modes. Check native readback, visible channel identity, a colour-to-white handoff, a short fade/custom stream and final Off. Keep initial settings low while observing the new production path. The ESP physical-button owner is unchanged by this controller image.

The software suite checks every modeled write across 20,005 frame handoffs, dropped reductions, failed pin handoffs and invalid mixed-channel frames. The compiled emulator executes the real main loop before and after confirmation, the 30-second boundary, owned recovery, active PWM faults and early startup failures. MMIO, ROM and interrupt delivery are modeled; these tests supplement the physical observations above.
