"""Validate the fixed OFF1/LOW1 diagnostic records during stock migration.

The wire format is big-endian uint32 words. These checks intentionally match
firmware/main/controller_diagnostic.h. They establish software timing and
captured register values, not optical polarity, current or thermal limits.
No transport or device access occurs in this module.
"""
from __future__ import annotations

import hashlib
import struct

_PINS = sum(1 << bit for bit in (13, 14, 16, 18, 19))
_GPIO = (0x81, 0x81, 0x80, 0, 0)
_PWM = (0x83, 0x83, 0x82, 2, 2)
_CHANNEL_PINS = tuple(1 << bit for bit in (16, 14, 13, 19, 18))
_WORDS = {"OFF1": 224, "LOW1": 256}


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def _decode(profile: str, raw: bytes, initial: bool) -> tuple[int, ...]:
    _require(type(profile) is str and profile in _WORDS, "Unknown diagnostic profile")
    count = 16 if initial else _WORDS[profile]
    _require(type(raw) is bytes and len(raw) == count * 4,
             f"{profile} requires exactly {count * 4} bytes")
    return struct.unpack(f">{count}I", raw)


def _profile(profile: str, w: tuple[int, ...]) -> None:
    if profile == "OFF1":
        valid = (w[0] == 0x4F464631 and w[1] == 1 and w[2] <= 3
                 and w[7] <= 1 and w[8] <= 6 and not w[10] & ~31
                 and w[13] == 0x193 and w[14] == 30000 and w[15] != 0)
    else:
        valid = (w[0] == 0x4C4F5731 and w[1] == 1 and w[2] <= 3
                 and w[6] <= 1 and w[7] <= 7 and not w[9] & ~31
                 and w[10] == 0x393 and w[11] == 30000 and w[12] != 0
                 and w[13] == 100 and w[14] == 1500 and w[15] == 5)
    _require(valid, f"Invalid {profile} header")


def validate_initial(profile: str, raw: bytes) -> int:
    """Return the nonzero generation of an untouched 64-byte profile header.

    Raise ValueError for an unknown profile, wrong length, invalid ABI or a
    previously started diagnostic. Retain this generation for validate_record.
    """
    w = _decode(profile, raw, True)
    _profile(profile, w)
    stop = 13 if profile == "OFF1" else 10
    _require(w[2] == 0 and not any(w[3:stop]), "Diagnostic was already started")
    return w[15 if profile == "OFF1" else 12]


def _mux(values: tuple[int, ...], expected: tuple[int, ...]) -> bool:
    return all(value & (0x87 if p < 3 else 7) == expected[p]
               for p, value in enumerate(values))


def _timer(t: tuple[int, ...], color: bool, stopped: bool = False) -> bool:
    period = 25499 if color else 254
    return (t[0] == (2 if stopped else 1) and t[2] == (0 if color else 47)
            and t[4] == 0x80 and t[5] == period + 1 and t[6] == period + 1
            and t[7] == period and t[8] == period + 1 and not any(t[9:12])
            and t[12] == (11 if color else 3)
            and (not stopped or (t[1] == 0 and t[3] == 0)))


def _off(w: tuple[int, ...], generation: int) -> None:
    _require(w[15] == generation and w[2] == 2 and w[7:13] == (1, 1, 0, 31, 2, 2)
             and w[3] < 28000 and w[4] == w[3] and w[6] == w[4] + 400
             and w[4] + 400 <= w[5] <= w[4] + 500 and not any(w[216:224]),
             "OFF1 did not complete the bounded all-off sequence")
    for i, phase in enumerate((0, 0, 1, 2, 2)):
        s = w[16 + i * 40:56 + i * 40]
        _require(s[0] == w[4 if i < 3 else 5] and s[1] == (0 if i < 3 else 1)
                 and s[2] & 0x600 == 0x600 and s[3] & _PINS == _PINS
                 and not s[4] & _PINS and s[38] == phase and s[39] == 0
                 and _mux(s[5:10], _PWM if i == 2 else _GPIO),
                 f"OFF1 snapshot {i} GPIO/timing mismatch")
        if i:  # Snapshot zero deliberately records arbitrary inherited timers.
            _require(_timer(s[10:24], False, i == 4) and _timer(s[24:38], True, i == 4),
                     f"OFF1 snapshot {i} timer mismatch")


def _low(w: tuple[int, ...], generation: int) -> None:
    _require(w[12] == generation and w[2] == 2 and w[3] < 22000
             and w[4] == w[3] + 6500 and w[5] == w[4] and w[6:10] == (1, 1, 0, 31),
             "LOW1 did not complete the bounded five-channel sequence")
    b = w[16:56]
    _require(b[0] == w[3] and b[1] == 0 and b[2] & 0x600 == 0x600
             and b[3] & _PINS == _PINS and not b[4] & _PINS
             and b[38] == 0 and b[39] == 0 and _mux(b[5:10], _GPIO)
             and _timer(b[10:24], False) and _timer(b[24:38], True),
             "LOW1 baseline GPIO/timer mismatch")
    for c in range(5):
        s = w[56 + c * 40:96 + c * 40]
        start = w[3] + c * 1600
        _require(s[0:6] == (c, start, start + 100, start + 100, 1, 0)
                 and s[6] & 0x600 == 0x600 and s[7] & _PINS == _PINS
                 and not s[8] & (_PINS & ~_CHANNEL_PINS[c])
                 and s[14:16] == (1, 1) and s[21:25] == (254, 25499, 3, 11)
                 and s[25] & _PINS == _PINS and not s[26] & _PINS
                 and s[32:34] == (2, 2) and s[39] == 1
                 and _mux(s[9:14], _PWM) and _mux(s[27:32], _GPIO),
                 f"LOW1 channel {c} GPIO/timing mismatch")
        for p in range(5):
            active = 25245 if p < 3 else 253
            off = 25500 if p < 3 else 255
            _require(s[16 + p] == (active if p == c else off) and s[34 + p] == off,
                     f"LOW1 channel {c} compare mismatch")


def validate_record(profile: str, raw: bytes, generation: int) -> dict:
    """Validate a complete record against its initial generation, or raise.

    Returned metadata describes the captured software/register evidence only.
    An operator must independently observe the expected physical output before
    treating a diagnostic as hardware qualification.
    """
    _require(type(generation) is int and 0 < generation <= 0xFFFFFFFF,
             "Generation must be a nonzero uint32")
    w = _decode(profile, raw, False)
    _profile(profile, w)
    (_off if profile == "OFF1" else _low)(w, generation)
    return {
        "profile": profile, "abi": 1, "generation": generation,
        "record_bytes": len(raw), "record_sha256": hashlib.sha256(raw).hexdigest(),
        "started_ms": w[3], "completed_ms": w[5], "automatic_recovery_ms": 30000,
        "record_verified": True, "physical_observation_required": True,
        "scope": "software timing and captured registers only",
    }
