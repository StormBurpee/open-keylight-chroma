"""Build an explicitly non-deployable original Cortex-M0 qualification image."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import shutil
import struct
import subprocess

root = Path(__file__).resolve().parent
parser = argparse.ArgumentParser(description=__doc__)
profiles = parser.add_mutually_exclusive_group()
profiles.add_argument("--spi-only-trial", action="store_true", help="Explicit experimental 30-second link/recovery trial; PWM paths absent")
profiles.add_argument("--pwm-off-trial", action="store_true", help="Unconfirmable one-shot OFF1 experiment; no nonzero PWM API")
parser.add_argument("--spi-mode", type=int, choices=(0, 3), default=3)
args = parser.parse_args()
trial = args.spi_only_trial or args.pwm_off_trial
profile = "pwm-off-trial" if args.pwm_off_trial else "spi-trial-mode" + str(args.spi_mode)
if args.pwm_off_trial and args.spi_mode != 3:
    parser.error("The reviewed off-only reference profile requires SPI mode3")
build = root / "build" / profile if trial else root / "build"
build.mkdir(parents=True, exist_ok=True)
clang = os.environ.get("CLANG") or shutil.which("clang")
if not clang:
    raise SystemExit("clang with arm-none-eabi support is required; add it to PATH or set CLANG.")
bin_dir = Path(clang).resolve().parent
objcopy = os.environ.get("LLVM_OBJCOPY") or shutil.which("llvm-objcopy")
if not objcopy:
    adjacent_objcopy = bin_dir / ("llvm-objcopy.exe" if os.name == "nt" else "llvm-objcopy")
    if not adjacent_objcopy.is_file():
        raise SystemExit("llvm-objcopy is required; add it to PATH or set LLVM_OBJCOPY.")
    objcopy = str(adjacent_objcopy)
sources = [root / "src/nxp_app.c", root / "src/nxp_board.c", root / "src/nxp_pwm_off_trial.c",
           root / "platform/main.c", root / "platform/memory.c", root / "platform/startup.S"]
flags = ["--target=arm-none-eabi", "-mcpu=cortex-m0", "-mthumb", "-std=c99", "-Os", "-g",
         "-ffreestanding", "-fno-builtin", "-fno-unwind-tables", "-fno-asynchronous-unwind-tables",
         "-ffunction-sections", "-fdata-sections", "-Wall", "-Wextra", "-Werror", "-I", str(root / "include")]
flags.append("-fstack-usage")
if trial:
    flags += ["-DNXP_SPI_ONLY_TRIAL=1", "-DNXP_APPROVED_PART_ID=0x0000bc40u",
              "-DNXP_BOARD_QUALIFICATIONS=" + ("0x193u" if args.pwm_off_trial else "0x93u"),
              "-DNXP_QUALIFIED_SPI_MODE=" + str(args.spi_mode) + "u"]
if args.pwm_off_trial:
    flags += ["-DNXP_PWM_OFF_TRIAL=1"]
macros = subprocess.run([clang, *flags, "-dM", "-E", "-x", "c", str(root / "platform/qualification.h")],
                        check=True, capture_output=True, text=True).stdout
for gate in ("NXP_APPROVED_PART_ID", "NXP_BOARD_QUALIFICATIONS"):
    definition = next(line.split(maxsplit=2)[2] for line in macros.splitlines() if line.startswith("#define " + gate + " "))
    expected = "0x0000bc40u" if gate == "NXP_APPROVED_PART_ID" else ("0x193u" if args.pwm_off_trial else "0x93u")
    allowed = definition == expected if trial else definition in ("0", "0u", "0U")
    if not allowed:
        raise SystemExit(f"{gate} does not match this explicitly selected build's hardware gates.")
objects = []
for source in sources:
    output = build / (source.stem + ".o")
    subprocess.run([clang, *flags, "-c", str(source), "-o", str(output)], check=True)
    objects.append(output)
name = "open-keylight-nxp-pwm-off-trial" if args.pwm_off_trial else "open-keylight-nxp-spi-trial" if args.spi_only_trial else "open-keylight-nxp-qualification"
elf = build / (name + ".elf")
binary = build / (name + ".bin")
subprocess.run([clang, "--target=arm-none-eabi", "-mcpu=cortex-m0", "-mthumb", "-nostdlib", "-fuse-ld=lld",
                *map(str, objects), "-Wl,--gc-sections", "-Wl,--undefined=nxp_process",
                "-Wl,--undefined=nxp_link_transaction", "-Wl,--undefined=nxp_render",
                "-Wl,-T," + str(root / "platform/application.ld"), "-Wl,-Map," + str(build / "application.map"),
                "-o", str(elf)], check=True)
subprocess.run([objcopy, "-O", "binary", str(elf), str(binary)], check=True)
image = binary.read_bytes()
stack, reset = struct.unpack_from("<II", image)
assert len(image) <= 0x7000 and stack == 0x10001000 and reset & 1 and 0x2000 <= (reset & ~1) < 0x9000
assert len(image[:192]) == 192
bank = image + b"\xff" * (0x7000 - len(image))
if trial:
    (build / ("pwm-off-trial-bank.bin" if args.pwm_off_trial else "spi-trial-bank.bin")).write_bytes(bank)
    # Linker map evidence: no peripheral PWM/GPIO-off output routines survive.
    map_text = (build / "application.map").read_text()
    for prohibited in ("nxp_board_start_pwm", "nxp_board_apply_pwm", "nxp_board_force_off"):
        assert prohibited not in map_text, prohibited
    if not args.pwm_off_trial:
        for address in (0x40014000, 0x40018000):
            assert struct.pack("<I", address) not in image, "PWM base unexpectedly linked"
manifest = {"name": "Open Keylight Chroma NXP " + ("PWM off-only trial" if args.pwm_off_trial else "SPI trial" if trial else "qualification"), "deployable": False,
            "hardware_tested": False, "device_operations": 0, "application_start": "0x2000",
            "application_bank_bytes": 28672, "image_bytes": len(image),
            "sha256": hashlib.sha256(image).hexdigest(), "stack_top": hex(stack), "reset_vector": hex(reset),
            "output_enable": False, "physical_spi_adapter_present": True, "physical_spi_service_enabled": trial,
            "experimental_spi_only_trial": args.spi_only_trial, "experimental_pwm_off_trial": args.pwm_off_trial,
            "spi_mode": args.spi_mode if trial else None,
            "trial_timeout_ms": 30000, "trial_confirm_command": "unsupported for diagnostic roles; role2 only",
            "off_window_ms": 400 if args.pwm_off_trial else None,
            "bank_sha256": hashlib.sha256(bank).hexdigest() if trial else None,
            "hardware_flags": "0x193 (off-only experiment; no production polarity/power qualification)" if args.pwm_off_trial else "0x93 (part,clock,recovery,experimentalSPIpermission)" if trial else "0",
            "source_sha256": {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest()
                              for p in sources + [root / "platform/application.ld", root / "platform/qualification.h", root / "include/nxp_app.h", root / "include/nxp_board.h", root / "include/nxp_pwm_off_trial.h", Path(__file__)]},
            "reference_part_id": "0x0000bc40", "reference_variant": "LPC11U35/501",
            "limitations": ["Package and PCB nets not physically confirmed", "SPI mode/select wiring unqualified",
                            "PWM polarity/current/thermal limits unqualified", "Bootloader recovery entry not qualified",
                            ("Experimental build permits one400ms all-low timer/mux test, fixed snapshot pages, no nonzero output or FD" if args.pwm_off_trial else
                             "Experimental build enables SPI and prepared low GPIO only; PWM paths absent" if args.spi_only_trial else
                             "Default build enables no output mux/timers or physical SPI; IAP54 and clock snapshot are read-only"),
                            "Physical adapter needs CS-gap/FIFO timing qualification and a proven recovery request"]}
(build / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
print(json.dumps({key: manifest[key] for key in ("deployable", "experimental_spi_only_trial", "image_bytes", "sha256", "bank_sha256", "reset_vector")}, indent=2))
