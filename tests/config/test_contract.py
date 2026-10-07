"""Check the published settings schema against the implemented encoding boundary."""
from pathlib import Path
import json
from jsonschema import Draft202012Validator

ROOT = Path(__file__).resolve().parents[2]
doc = json.loads((ROOT / "docs/openapi.json").read_text(encoding="utf-8"))
schemas = doc["components"]["schemas"]
Draft202012Validator.check_schema(schemas["SettingsPatch"])
validator = Draft202012Validator(schemas["SettingsPatch"])
for value in [{"output_encoding": "srgb"}, {"output_encoding": "linear"}, {"name": "Lamp"}]:
    assert validator.is_valid(value), value
for value in [{}, {"output_encoding": None}, {"output_encoding": 0}, {"output_encoding": True},
              {"output_encoding": "sRGB"}, {"output_encoding": "linear "},
              {"output_encoding": "linear", "name": "Lamp"},
              {"output_encoding": "srgb", "unexpected": 1}]:
    assert not validator.is_valid(value), value
assert "output_encoding" in schemas["Settings"]["required"]
assert schemas["Settings"]["properties"]["output_encoding"]["default"] == "srgb"
assert "423" in doc["paths"]["/settings"]["patch"]["responses"]
print("PASS settings output-encoding schema boundary")
