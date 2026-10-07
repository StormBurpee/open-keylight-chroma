# LOW1 fixed low-output diagnostic

LOW1 is an explicitly selected original NXP diagnostic application: role 1, recovery capability 1. It cannot be confirmed, and ordinary lighting setters remain unsupported. It does not claim production PWM polarity, current or thermal qualification. The default, SPI-only and OFF1 profiles retain their separate build gates.

Build with `python firmware-nxp/build.py --pwm-low-trial --spi-mode 3`. This produces a complete 28,672-byte application bank under `firmware-nxp/build/pwm-low-trial/`; the resident bootloader, calibration and persistent flags are outside that bank. The manifest records its source and image hashes. Build output is not committed. Deployment requires the explicit diagnostic-low update mode and a reviewed target-specific qualification procedure. Build/test success alone is not hardware qualification.

## Fixed experiment

One owned command00/71 with four ASCII bytes `LOW1` prepares an ACK. The experiment starts only after the entire matching ACK body has been consumed and the link is back in request phase. Loss of that body spends the request without starting output. No second arm is accepted during this boot.

The fixed order is red, green, blue, warm white, cool white. Each channel receives100ms of PWM followed by1500ms of GPIO-low darkness before the next channel. Starts are at0,1600,3200,4800,6400ms relative to the first pulse; the sequence ends at6500ms. Only the initial arm uses main service. SysTick starts the later pulses and ends every pulse, independently of the main polling loop. A missing scheduled tick is a permanent failure, never a catch-up pulse. This mechanism depends on a running CPU and interrupts; it is not an independent hardware one-shot under a halted CPU.

The selected RGB comparator is25245 with CT32B1 MR2=25499; the selected white comparator is253 with CT32B0 MR2=254. Other RGB compares remain25500 and other white compares255. These fixed inverse-PWM values are no more than1% nominal high duty, assuming the documented timer behavior and the inferred active-high driver polarity. No duty, duration, channel, memory address or timer register is accepted from a network request. A passing register test does not measure optical brightness, current, temperature or physical polarity.

GPIO-low ownership is verified before timer setup. Both timers start with all compares above period, then pads are connected to PWM, then the sole fixed low comparator is written. At cutoff, GPIO-low handoff precedes timer stop. If that handoff fails, all comparators are set above period and the timers continue running; stopping could otherwise freeze a high PWM latch. This fallback is reported as failure. Owner loss, SPI errors, register mismatch, requested recovery and late scheduling abort the whole sequence with no re-arm.

Qualification flags are exactly0x393: observed part, inherited clock, established resident recovery, experimental SPI permission, experimental off setup and experimental low output. Production PWM-polarity and power-limit flags remain absent. The part is the observed0x0000bc40; the clock gate requires the same inherited48MHz register state as OFF1. Requests at uptime22,000ms or later are rejected; the observer should begin admission before20,000ms.

## Fixed native record

F0 has no arguments and returns8bytes: `LOW1`, request state,5channels, BE16(100ms). Request states are0unused,1ACKpending,2ACKfullyconsumed,3spent.

F1 accepts exactly one page index0..15. Each72-byte reply is `LOW1`, page index,16pages,64payload bytes,0reserved, then the requested64-byte slice of a fixed1024-byte record. There is no arbitrary read. Main mutations/parser calls share interrupt exclusion; SysTick record changes cannot interleave a parser copy. After completion the record is immutable until reset.

All record words are big-endian32-bit. Header words0..15 are: magic0x4c4f5731, ABI1, phase(0idle/1running/2completed/3failed), sequence start, end, deadline(start+6500), accepted count, reason, error count, completed mask, flags0x393, recovery deadline30000, generation, pulse100, gap1500, channel count5. Reasons are0none,1deadline,2owner,3SPI,4register,5cancelled,6recovery,7late. Generation is a retained USB-SRAM correlation counter, not authentication or cryptographic proof of reset.

Words16..55 hold the baseline40-word OFF1-compatible register snapshot, captured after all-off timers are prepared but before PWM muxing: time, reason0, AHB clocks, GPIO direction, GPIO pads, five IOCON words(P0_13/14/16/18/19),14white timer registers,14RGB timer registers, phase0, errors0. Timer offsets are04,08,0c,10,14,18,1c,20,24,28,3c,70,74,00. Baseline timers run with TCR1, PR47/0, MR2=254/25499, all output matches above period, PWMC3/11, and GPIO-low ownership.

Five40-word pulse records follow, beginning at word56+40×channel. Their fields are:

| Words | Meaning |
|---|---|
|0..5|channel index, start, end, deadline(start+100), reason, SPI errors|
|6..8|active AHB clocks, GPIO direction, GPIO pads|
|9..13|active IOCON for P0_13/14/16/18/19|
|14..15|active white/RGB TCR|
|16..20|active red/green/blue/warm/cool compares|
|21..24|white MR2, RGB MR2, white PWMC, RGB PWMC|
|25..26|post-cutoff GPIO direction/pads|
|27..31|post-cutoff IOCON in the same order|
|32..33|post-cutoff white/RGB TCR|
|34..38|post-cutoff red/green/blue/warm/cool compares|
|39|1 only for a completed exact100ms pulse|

Active GPIO sampling can observe either level for the selected PWM channel; non-selected output pins must be low. Every post-cutoff pad must be GPIO-low, output compares above period and TCR2. Successful header mask is31, count1, phase2, reason1, errors0, with exact scheduled timestamps.

The native observer must remain completely SPI-silent for at least6.7seconds after consuming the LOW1 ACK, then read F0, all16pages and page0 again. This avoids SPI FIFO servicing contention during bounded SysTick timer setup and snapshot work. It must validate the entire fixed record and leave the application unconfirmed. No reset/erase/program/FD retry follows a missing or invalid record. Only the previously reviewed explicit recovery procedure can establish a new resident-loader update opportunity.

## Reproducible offline checks

Set `CC` to an AddressSanitizer-capable compiler, then run `python firmware-nxp/tests/run_tests.py`. This executes the normal protocol, board, PWM and OFF1 suites plus a separately compiled LOW1 target. The LOW1 test includes the real ESP record predicates, checks every page index, malformed command lengths, foreign ownership, missing ACK, no re-arm, missed scheduling ticks, owner loss during a gap, preexisting SPI errors, dropped comparator writes and failed GPIO handoff. Its model checks the exposed comparator bounds at every MMIO write. The CMake host test is `nxp_pwm_low`.

After building, `python firmware-nxp/tests/emulate_low.py` uses Unicorn (optionally supplied through `NXP_UNICORN_PATH`) to execute actual compiled startup, protocol arm, all 6,500 SysTick steps and the 30-second recovery path. It also exercises ROM failure, wrong part, wrong clock, watchdog reset and selected-CS startup. Source/image pins, vectors, RAM/stack bounds, unresolved symbols and absence of production PWM APIs are checked. This test models MMIO, ROM and interrupt delivery; it does not execute vendor firmware or qualify physical electrical behavior.

The implementation was first reviewed in an isolated original-source snapshot. The public merge is checked to produce the identical LOW1 bank and to preserve the existing OFF1 bank. No proprietary binaries, firmware extracts or device credentials belong in this repository.
