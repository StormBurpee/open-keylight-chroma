"""Fixed-record parser tests, including the actual firmware C predicates."""
from pathlib import Path
import os
import random
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import migration_profiles as profiles

PINS = sum(1 << b for b in (13, 14, 16, 18, 19))
GPIO = [0x81, 0x81, 0x80, 0, 0]
PWM = [0x83, 0x83, 0x82, 2, 2]


def pack(words):
    return struct.pack(f">{len(words)}I", *words)


def initial(profile, generation=7):
    if profile == "OFF1":
        return [0x4F464631, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x193, 30000, generation]
    return [0x4C4F5731, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0x393, 30000, generation, 100, 1500, 5]


def timer(color, stopped=False):
    period = 25499 if color else 254
    return [2 if stopped else 1, 0, 0 if color else 47, 0, 0x80,
            period + 1, period + 1, period, period + 1, 0, 0, 0, 11 if color else 3, 0]


def complete(profile, generation=7, start=1234):
    w = initial(profile, generation)
    w[2] = 2
    if profile == "OFF1":
        w[3:13] = [start, start, start + 400, start + 400, 1, 1, 0, 31, 2, 2]
        for i, phase in enumerate((0, 0, 1, 2, 2)):
            w += [start if i < 3 else start + 400, int(i >= 3), 0x600, PINS, 0]
            w += PWM if i == 2 else GPIO
            w += timer(False, i == 4) + timer(True, i == 4) + [phase, 0]
        w += [0] * 8
    else:
        w[3:10] = [start, start + 6500, start + 6500, 1, 1, 0, 31]
        w += [start, 0, 0x600, PINS, 0] + GPIO + timer(False) + timer(True) + [0, 0]
        for c in range(5):
            now = start + c * 1600
            active = [25245 if p < 3 else 253 for p in range(5)]
            off = [25500, 25500, 25500, 255, 255]
            matches = [active[p] if p == c else off[p] for p in range(5)]
            w += [c, now, now + 100, now + 100, 1, 0, 0x600, PINS, 0] + PWM
            w += [1, 1] + matches + [254, 25499, 3, 11, PINS, 0] + GPIO
            w += [2, 2] + off + [1]
    return w


def accepted(profile, words, is_initial=False, generation=7):
    try:
        if is_initial:
            profiles.validate_initial(profile, pack(words))
        else:
            profiles.validate_record(profile, pack(words), generation)
        return True
    except ValueError:
        return False


class Tests(unittest.TestCase):
    def test_complete_profiles_and_evidence_scope(self):
        for profile, size in (("OFF1", 896), ("LOW1", 1024)):
            for generation in (1, 7, 0xFFFFFFFF):
                self.assertEqual(profiles.validate_initial(profile, pack(initial(profile, generation))), generation)
                data = pack(complete(profile, generation))
                self.assertEqual(len(data), size)
                result = profiles.validate_record(profile, data, generation)
                self.assertEqual(result["generation"], generation)
                self.assertEqual(result["record_bytes"], size)
                self.assertTrue(result["record_verified"])
                self.assertTrue(result["physical_observation_required"])
                self.assertEqual(result["automatic_recovery_ms"], 30000)

    def test_exact_lengths_endianness_and_strict_input_types(self):
        for profile in ("OFF1", "LOW1"):
            head = pack(initial(profile))
            raw = pack(complete(profile))
            for value in (None, "", bytearray(raw), memoryview(raw), raw[:-1], raw + b"\0", head):
                with self.assertRaises(ValueError): profiles.validate_record(profile, value, 7)
            for value in (None, "", bytearray(head), head[:-1], head + b"\0", raw):
                with self.assertRaises(ValueError): profiles.validate_initial(profile, value)
            with self.assertRaises(ValueError):
                profiles.validate_record(profile, struct.pack(f"<{len(raw) // 4}I", *complete(profile)), 7)
            for generation in (None, True, False, 0, -1, 8, 1 << 32, 7.0, "7"):
                with self.assertRaises(ValueError): profiles.validate_record(profile, raw, generation)
        for profile in (None, [], {}, True, "off1", "LOW2", ""):
            with self.assertRaises(ValueError): profiles.validate_initial(profile, b"\0" * 64)

    def test_profiles_cannot_be_swapped_or_replayed(self):
        for profile, other in (("OFF1", "LOW1"), ("LOW1", "OFF1")):
            with self.assertRaises(ValueError): profiles.validate_initial(profile, pack(initial(other)))
            with self.assertRaises(ValueError): profiles.validate_initial(profile, pack(complete(profile)[:16]))
            for gen in (0, 6, 8):
                self.assertFalse(accepted(profile, complete(profile), generation=gen))
            for phase in (0, 1, 3):
                words = complete(profile); words[2] = phase
                self.assertFalse(accepted(profile, words))

    def test_boundaries_and_intentionally_unconstrained_bits(self):
        for profile, max_start in (("OFF1", 27999), ("LOW1", 21999)):
            self.assertTrue(accepted(profile, complete(profile, start=0)))
            self.assertTrue(accepted(profile, complete(profile, start=max_start)))
            self.assertFalse(accepted(profile, complete(profile, start=max_start + 1)))
        for elapsed in (400, 401, 500):
            w = complete("OFF1"); w[5] = w[4] + elapsed
            w[16 + 3 * 40] = w[5]; w[16 + 4 * 40] = w[5]
            self.assertTrue(accepted("OFF1", w))
        for elapsed in (399, 501):
            w = complete("OFF1"); w[5] = w[4] + elapsed
            self.assertFalse(accepted("OFF1", w))
        w = complete("OFF1")
        w[26:54] = [0xFFFFFFFF] * 28  # Inherited timer registers are evidence, not assumptions.
        w[21] |= 1 << 10  # An unrelated IOCON bit must not invalidate selected function.
        self.assertTrue(accepted("OFF1", w))
        for c, pin in enumerate((16, 14, 13, 19, 18)):
            w = complete("LOW1"); w[56 + c * 40 + 8] |= 1 << pin
            self.assertTrue(accepted("LOW1", w))  # Selected pin can be sampled in either PWM phase.
            w[56 + c * 40 + 8] |= 1 << (14 if pin != 14 else 13)
            self.assertFalse(accepted("LOW1", w))

    def test_actual_c_predicates_match_every_single_bit_mutation(self):
        compiler = os.environ.get("CC") or shutil.which("clang") or shutil.which("cc")
        self.assertTrue(compiler, "A C compiler is required for the independent predicate comparison")
        cases = []
        for profile in ("OFF1", "LOW1"):
            for is_initial, words in ((True, initial(profile)), (False, complete(profile))):
                cases.append((profile, is_initial, 7, words))
                for index in range(len(words)):
                    for bit in range(32):
                        changed = words.copy(); changed[index] ^= 1 << bit
                        cases.append((profile, is_initial, 7, changed))
        rng = random.Random(0x4F4B4C)
        for profile in ("OFF1", "LOW1"):
            for _ in range(500):
                w = complete(profile)
                for _ in range(rng.randrange(1, 9)):
                    w[rng.randrange(len(w))] = rng.getrandbits(32)
                cases.append((profile, False, 7, w))
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / ("oracle.exe" if os.name == "nt" else "oracle")
            subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wpedantic",
                            "-I", str(ROOT / "firmware/main"),
                            "-I", str(ROOT / "firmware/components/keylight_nxp/include"),
                            str(Path(__file__).with_name("profile_oracle.c")), "-o", str(binary)], check=True)
            data = b"".join(struct.pack("<259I", 1 if p == "OFF1" else 2, int(i), g,
                                       *(w + [0] * (256 - len(w)))) for p, i, g, w in cases)
            answer = subprocess.run([str(binary)], input=data, capture_output=True, check=True).stdout
        self.assertEqual(len(answer), len(cases))
        for number, ((profile, is_initial, generation, words), result) in enumerate(zip(cases, answer)):
            with self.subTest(case=number, profile=profile, initial=is_initial):
                self.assertEqual(accepted(profile, words, is_initial, generation), result == ord("1"))
        print(f"Diagnostic Python/C predicate comparison: {len(cases)} cases", flush=True)


if __name__ == "__main__":
    unittest.main()
