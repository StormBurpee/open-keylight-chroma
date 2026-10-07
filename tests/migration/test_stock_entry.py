"""Fixed vendor entry cadence and fail-closed admission; no device sockets."""
from pathlib import Path
import struct
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import stock_migration as m
from test_migration import FlowSession, flow, make_session, reply


class EntryPeer(FlowSession):
    def __init__(self, *, lose_first=False, fail=None):
        super().__init__()
        self.lose_first, self.fail = lose_first, fail
        self.entry_sends, self.gaps, self.barriers = [], [], 0
        self.resets, self.resident_noops = 0, 0

    def send(self, frame, label, deadline, mutation=False):
        assert frame == m.report_request(0, 4, b"\1\0")
        assert mutation and label == f"stock_entry_fixed_{len(self.entry_sends) + 1}"
        self.entry_sends.append((self.clock(), label))
        self.commands.append((0, 4, b"\1\0"))
        if self.fail == label:
            self.poisoned = True
            raise OSError("unknown send delivery")
        if len(self.entry_sends) == 1 and self.lose_first:
            return
        if self.firmware == "stock":
            self.resets += 1
            self.firmware = "resident"
        else:
            self.resident_noops += 1

    def quiet(self, seconds):
        assert seconds == .1
        self.gaps.append(seconds)
        self.sleep(seconds)
        if self.fail == f"gap{len(self.gaps)}":
            raise KeyboardInterrupt("cancelled after quiet")

    def network_get(self, opcode):
        assert opcode == 0x84 and len(self.entry_sends) == len(self.gaps) == 2
        self.barriers += 1
        self.commands.append(("barrier",))
        if self.fail == "barrier": raise TimeoutError("ESP queue did not complete")
        if self.fail == "wrong_version": return bytes(4)
        return self.target.esp_version

    def exchange(self, cls, opcode, payload=b"", **kwargs):
        if cls == 16:
            assert self.barriers == 1
            if self.fail == "info" and opcode == 0x80: raise TimeoutError("no information")
            if self.fail == "wrong_info" and opcode == 0x80: return bytes(80)
            if self.fail == "fingerprint" and opcode == 0x83: raise TimeoutError("no resident bytes")
        return super().exchange(cls, opcode, payload, **kwargs)


class Tests(unittest.TestCase):
    def setUp(self):
        self.network = patch("socket.socket", side_effect=AssertionError("Real sockets forbidden"))
        self.network.start(); self.addCleanup(self.network.stop)

    def migration(self, **kwargs):
        migration, _, _ = flow()
        peer = EntryPeer(**kwargs)
        migration.session, migration.phase = peer, "stock"
        return migration, peer

    def test_fixed_pair_with_first_consumed_or_lost(self):
        for lose in (False, True):
            migration, peer = self.migration(lose_first=lose)
            with self.subTest(lose_first=lose), patch.object(m, "inspect_nxp_loader", return_value={"reference_code_matches": True}):
                migration.enter_stock_loader()
            self.assertEqual(peer.resets, 1)
            self.assertEqual(peer.resident_noops, 0 if lose else 1)
            self.assertEqual(len(peer.entry_sends), 2)
            self.assertAlmostEqual(peer.entry_sends[1][0] - peer.entry_sends[0][0], .1)
            self.assertEqual(peer.gaps, [.1, .1])
            self.assertEqual(peer.barriers, 1)
            self.assertTrue(migration.fresh_resident)
            self.assertEqual(migration.phase, "resident")
            reads = [x for x in peer.commands if x[:2] == (16, 0x83)]
            self.assertEqual([int.from_bytes(x[2][1:5], "big") for x in reads], list(range(0, 8192, 64)))
            self.assertFalse(any(x[0] == "install" or x[:2] in ((16, 1), (16, 2), (16, 4), (16, 5)) for x in peer.commands))

    def test_each_ambiguity_never_retries_or_admits_an_image(self):
        cases = ("stock_entry_fixed_1", "stock_entry_fixed_2", "barrier", "wrong_version", "info", "wrong_info", "fingerprint")
        for failure in cases:
            migration, peer = self.migration(fail=failure)
            with self.subTest(failure=failure), self.assertRaises(m.StockEntryError) as caught:
                migration.enter_stock_loader()
            self.assertEqual(caught.exception.code, "stock_loader_entry_unconfirmed")
            self.assertIn("No controller image was erased, programmed or committed", str(caught.exception))
            self.assertFalse(migration.fresh_resident)
            self.assertEqual(migration.phase, "failed")
            self.assertEqual(len(peer.entry_sends), 1 if failure == "stock_entry_fixed_1" else 2)
            self.assertFalse(any(x[:2] in ((16, 1), (16, 2), (16, 4), (16, 5)) for x in peer.commands))
            with patch.object(m, "transfer_controller", side_effect=AssertionError("No image after unresolved entry")):
                with self.assertRaises(ValueError): migration.install("identity")
            if failure in ("barrier", "wrong_version"):
                self.assertFalse(any(x[0] == 16 for x in peer.commands))

    def test_cancellation_preserves_gap_and_stops_the_planned_sequence(self):
        for failure in ("gap1", "gap2"):
            migration, peer = self.migration(fail=failure)
            with self.subTest(failure=failure), self.assertRaises(KeyboardInterrupt): migration.enter_stock_loader()
            self.assertEqual(peer.gaps, [.1] * (1 if failure == "gap1" else 2))
            self.assertEqual(peer.barriers, 0)
            self.assertFalse(migration.fresh_resident)

    def test_failed_entry_intent_audit_cannot_send(self):
        migration, peer = self.migration()
        peer.audit.fail = "stock_entry_sequence"
        with self.assertRaises(m.StockEntryError): migration.enter_stock_loader()
        self.assertEqual(peer.entry_sends, [])
        self.assertEqual(peer.barriers, 0)

    def test_real_receiver_drains_noop_reply_before_esp_barrier(self):
        migration, peer = self.migration()
        tag, label = peer.routing_identity, b"Open Keylight migration"
        claim = b"\1" + tag + bytes([len(label)]) + label + bytes(64 - len(label))
        barrier = b"\xaa\x02\0\x09\x84" + peer.target.esp_version
        chunks = [reply(0, 0x49, claim), reply(0, 0xC9, claim[:8 + len(label)]),
                  reply(15, 2, b"\0\x05" + bytes(10)), reply(3, 3, b"\0\x20\0\0"),
                  reply(15, 0x82, bytes(6)), reply(3, 0x83, b"\0\x20\0\0"),
                  reply(0, 4, b"\1\0") + barrier[:3], barrier[3:],
                  reply(16, 0x80, m.NXP_INFORMATION + bytes(71))]
        chunks += [reply(16, 0x83, b"\x40" + struct.pack(">I", address) + bytes(64))
                   for address in range(0, 8192, 64)]
        session, sock, clock, audit = make_session(chunks)
        session.routing_identity = tag
        migration.session = session
        with patch.object(m, "inspect_nxp_loader", return_value={"reference_code_matches": True}):
            migration.enter_stock_loader()
        self.assertEqual(sock.sent[6:9], [m.report_request(0, 4, b"\1\0")] * 2 + [b"\xaa\0\0\x05\x84"])
        self.assertEqual(sock.sent[9], m.report_request(16, 0x80, bytes(80)))
        sends = [d for e, d in audit.events if e == "send_intent"]
        self.assertEqual([d["label"] for d in sends[6:9]], ["stock_entry_fixed_1", "stock_entry_fixed_2", "network_get"])
        self.assertFalse(session.poisoned)
        self.assertEqual(sock.chunks, [])
        self.assertAlmostEqual(clock.now(), 1.2)


if __name__ == "__main__": unittest.main()
