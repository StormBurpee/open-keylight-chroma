"""Embed the audited dashboard assets into the application, deterministically."""
import hashlib
import gzip
import json
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[1]
manifest = json.loads((root / "dashboard/dist/asset-manifest.json").read_text())
lines = ["#ifndef KEYLIGHT_WEB_ASSETS_H", "#define KEYLIGHT_WEB_ASSETS_H", "#include <stddef.h>",
         "typedef struct {const char *path, *content_type; const unsigned char *data; size_t size;} web_asset;"]
entries = []
for index, item in enumerate(manifest["files"]):
    source = root / "dashboard/dist" / item["path"].lstrip("/")
    assert hashlib.sha256(source.read_bytes()).hexdigest() == item["sha256"]
    data = (root / "dashboard/dist" / item["gzip_path"]).read_bytes()
    assert len(data) == item["gzip_size"]
    assert gzip.decompress(data) == source.read_bytes(), "Compressed dashboard asset differs from its source"
    lines.append(f"static const unsigned char web_asset_{index}[] = {{")
    lines.extend(",".join(str(byte) for byte in data[offset:offset + 32]) + "," for offset in range(0, len(data), 32))
    lines.append("};")
    entries.append(f'{{{json.dumps(item["path"])},{json.dumps(item["content_type"])},web_asset_{index},sizeof(web_asset_{index})}}')
lines.append("static const web_asset web_assets[] = {" + ",".join(entries) + "};")
lines.extend(["#define WEB_ASSET_COUNT (sizeof(web_assets) / sizeof(web_assets[0]))", "#endif", ""])
destination = Path(sys.argv[1]) if len(sys.argv) > 1 else root / "firmware/main/generated/web_assets.h"
destination.parent.mkdir(parents=True, exist_ok=True)
destination.write_text("\n".join(lines), encoding="ascii")
print(f"Embedded {len(entries)} assets, {manifest['gzip_bytes']} compressed bytes")
