"""Keep explicit recovery requests aligned with the strict HTTP boundary."""
import json
from pathlib import Path
from jsonschema import Draft202012Validator

ROOT = Path(__file__).resolve().parents[2]
document = json.loads((ROOT / "docs/openapi.json").read_text())
schemas = document["components"]["schemas"]
for schema in schemas.values():
    Draft202012Validator.check_schema(schema)
validator = Draft202012Validator(schemas["ControllerRecoveryRequest"])
for job_id in (1, 4294967295):
    assert validator.is_valid({"job_id": job_id, "power_cycle_acknowledged": True})
for job_id in (0, -1, 1.5, 4294967296, True, "1", None):
    assert not validator.is_valid({"job_id": job_id, "power_cycle_acknowledged": True})
for acknowledgment in (False, 1, "true", None):
    assert not validator.is_valid({"job_id": 1, "power_cycle_acknowledged": acknowledgment})
assert not validator.is_valid({"job_id": 1})
assert not validator.is_valid({"job_id": 1, "power_cycle_acknowledged": True, "extra": 0})
assert document["paths"]["/controller/recover"]["post"]["security"] == [{"BearerToken": []}]
digest = Draft202012Validator(schemas["Device"]["properties"]["firmware_elf_sha256"])
assert digest.is_valid("a" * 64)
assert not digest.is_valid("a" * 64 + "\n")
print("PASS recovery request and firmware identity schema boundaries")

diagnostic = Draft202012Validator(schemas["ControllerUpdateStatus"]["properties"]["diagnostic"])
assert diagnostic.is_valid(None)
for profile, size in (("OFF1", 224), ("LOW1", 256)):
    value = {"profile": profile, "command_attempted": True, "command_acknowledged": True,
             "registers_verified": True, "generation": 1, "snapshot_words": [0] * size, "error": None}
    assert diagnostic.is_valid(value)
    for wrong_size in (0, size - 1, size + 1, 256 if size == 224 else 224):
        assert not diagnostic.is_valid({**value, "snapshot_words": [0] * wrong_size})
    for wrong_profile in ("LOW2", "off1", None):
        assert not diagnostic.is_valid({**value, "profile": wrong_profile})
    assert not diagnostic.is_valid({**value, "snapshot_words": [-1] + [0] * (size - 1)})
    assert not diagnostic.is_valid({**value, "snapshot_words": [4294967296] + [0] * (size - 1)})
print("PASS fixed diagnostic profile and snapshot schema boundaries")
