#!/usr/bin/env python3
"""Prepare a reviewed, experimental stock-migration plan. Never contacts a light."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import tempfile

from stock_migration import ESP_SLOT_BYTES, NXP_BANK_BYTES, bounded_read, digest, load_plan


def prepare(args: argparse.Namespace) -> dict:
    """Validate all artifacts, then publish one complete plan without replacing files.

    Paths in the plan are relative to its directory where possible. The temporary
    manifest resides in that same directory, so validation uses the final path
    semantics. An exclusive hard-link publishes the already flushed file in one
    operation; filesystems without hard-link support fail without publishing.
    No firmware, restore-bank payload, credential or browser token is copied.
    """
    output = Path(args.output).absolute()
    if output.exists() or output.is_symlink():
        raise FileExistsError(f"Output already exists: {output}")

    def artifact(path: Path, maximum: int) -> dict:
        source = Path(path).resolve(strict=True)
        raw = bounded_read(source, maximum)
        try:
            name = os.path.relpath(source, output.parent)
        except ValueError:  # Windows paths on different drives cannot be relative.
            name = str(source)
        return {"path": name, "sha256": digest(raw)}

    packages = {
        name: artifact(getattr(args, option), NXP_BANK_BYTES + 64)
        for name, option in (("identity", "identity"), ("OFF1", "off1"),
                             ("LOW1", "low1"), ("lighting", "lighting"))
    }
    restore = artifact(args.restore, NXP_BANK_BYTES)
    restore.update(version=args.restore_version, provenance=args.restore_provenance)
    document = {
        "format": 1,
        "profile": "keylight-chroma-1.0.13",
        "source_commit": args.source_commit,
        "target": {"ip": args.target_ip, "name": args.target_name, "device_id": args.device_id},
        "packages": packages,
        "restore": restore,
        "esp": artifact(args.esp, ESP_SLOT_BYTES),
        "assets": artifact(args.assets, 131072),
    }
    raw = (json.dumps(document, indent=2, ensure_ascii=False, allow_nan=False) + "\n").encode("utf-8")
    temporary = None
    try:
        descriptor, name = tempfile.mkstemp(prefix=f".{output.name}.", suffix=".tmp", dir=output.parent)
        temporary = Path(name)
        with os.fdopen(descriptor, "wb") as file:
            file.write(raw)
            file.flush()
            os.fsync(file.fileno())
        plan = load_plan(temporary)
        # Unlike rename/replace, link refuses an existing destination on both
        # Windows and POSIX, including one created after the initial check.
        os.link(temporary, output)
        return {"plan": str(output), "manifest_sha256": plan["manifest_sha256"],
                "target_ip": plan["target"].ip, "target_name": plan["target"].name,
                "device_id": plan["device_id"], "device_operations": 0,
                "experimental": True, "hardware_qualified_by_preparation": False}
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def main(argv=None) -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True, help="New local JSON plan; never overwritten")
    parser.add_argument("--target-ip", required=True, help="One explicitly selected private IPv4 address")
    parser.add_argument("--target-name", required=True, help="Exact current stock light name")
    parser.add_argument("--device-id", required=True, help="keylight- followed by the light MAC's final six hex digits")
    parser.add_argument("--source-commit", required=True, help="Full 40-character reviewed source commit")
    for option in ("identity", "off1", "low1", "lighting"):
        parser.add_argument("--" + option, type=Path, required=True, help="Reviewed original controller package")
    parser.add_argument("--esp", type=Path, required=True, help="Standalone original ESP application .bin")
    parser.add_argument("--assets", type=Path, required=True, help="Matching dashboard asset-manifest.json")
    parser.add_argument("--restore", type=Path, required=True, help="Owner-local reviewed complete 28 KiB restore bank")
    parser.add_argument("--restore-version", required=True, help="Four-byte expected restored version, e.g. 1.3.0.0")
    parser.add_argument("--restore-provenance", required=True,
                        help="Origin, preserved tail, modifications and restore evidence; not merely a staging dump")
    args = parser.parse_args(argv)
    try:
        summary = prepare(args)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
