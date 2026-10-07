"""Synthetic Python-child fixture for the real Ink execution adapter.

This is test infrastructure, never a device installer: socket construction is
forbidden. ``--create DIRECTORY`` writes original synthetic artifacts and prints
their manifest path. Other arguments are forwarded to the actual run_cli with
fake device boundaries and real stdin/stdout JSONL. No response is auto-answered.
"""
from __future__ import annotations

import json
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

sys.path[:0] = [str(Path(__file__).resolve().parents[2] / "tools"), str(Path(__file__).resolve().parent)]
import stock_migration as migration
from test_cli import Environment, manifest_fixture
from test_finish import ExistingPeer


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    with patch("socket.socket", side_effect=AssertionError("Synthetic fixture forbids all sockets")):
        if len(argv) == 2 and argv[0] == "--create":
            directory = Path(argv[1])
            directory.mkdir(parents=True, exist_ok=False)
            path, _, _ = manifest_fixture(directory)
            print(json.dumps({"manifest": str(path), "manifest_sha256": migration.digest(path.read_bytes())}))
            return 0
        if "--manifest" not in argv:
            raise ValueError("Test fixture requires --manifest")
        path = Path(argv[argv.index("--manifest") + 1])
        case = unittest.TestCase()
        case.path, case.folder = path, path.parent
        case.manifest = json.loads(path.read_bytes())
        case.assets = {"/index.html": b"<html>Original test page</html>",
                       "/assets/test.js": b"console.log('test')", "/assets/test.css": b"body{}"}
        environment = Environment(case)
        session_factory = environment.session_factory
        if argv and argv[0] == "finish":
            peer = ExistingPeer(migration.load_plan(path))
            environment.time, environment.session = peer.time, peer
            def close():
                peer.closed = environment.closed = True
            peer.close = close
            def session_factory(target, audit, **_kwargs):
                peer.target, peer.audit = target, audit
                return peer
        getter = environment.http

        def http(ip, requested, maximum):
            raw = getter(ip, requested, maximum)
            if requested == "/api/v1/device":
                value = json.loads(raw)
                value["pairing_open"] = True
                value["trial_pending"] = environment.native_reads < 3
                return json.dumps(value).encode()
            return raw

        try:
            with patch.object(migration, "inspect_nxp_loader", return_value={"reference_code_matches": True}), \
                 patch.object(migration, "transfer_controller", side_effect=environment.transfer), \
                 patch.object(migration, "transfer_esp", side_effect=environment.transfer_esp):
                return migration.run_cli(argv, session_factory=session_factory,
                    audit_factory=environment.audit, get_http=http,
                    clock=environment.time.now, sleep=environment.time.sleep,
                    mac_reader=lambda *_args: bytes.fromhex("021111123456"))
        finally:
            case.doCleanups()


if __name__ == "__main__":
    raise SystemExit(main())
