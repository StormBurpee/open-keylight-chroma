"""Offline fault tests. Any accidental real socket construction fails immediately."""
import hashlib
import json
from pathlib import Path
import struct
import sys
import tempfile
import threading
import time
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import stock_migration as m
from package_controller import build_package
from test_profiles import initial, complete, pack


class Clock:
    def __init__(self): self.value = 1.0
    def now(self): return self.value
    def sleep(self, seconds): self.value += seconds


class MemoryAudit:
    def __init__(self): self.events, self.failed, self.fail = [], False, None
    def record(self, event, **details):
        if event == self.fail:
            self.failed = True
            raise OSError("disk full")
        self.events.append((event, details))


def reply(cls, op, payload=b"", status=2):
    frame = bytearray(m.report_request(cls, op, payload))
    frame[1], frame[5] = 2, status
    return bytes(frame)


def esp_image():
    header = bytearray(24)
    header[:4] = bytes([0xE9, 2, 2, 0x20])
    struct.pack_into("<I", header, 4, 0x40080000)
    header[23] = 1
    descriptor = bytearray(192)
    struct.pack_into("<I", descriptor, 0, 0xABCD5432)
    descriptor[16:20], descriptor[48:61] = b"test", b"open_keylight"
    descriptor[112:116] = b"v5.5"
    body = bytes(header) + struct.pack("<II", 0x3F400020, 192) + descriptor
    body += struct.pack("<II", 0x40080000, 4) + bytes(4)
    checksum = 0xEF
    for b in descriptor: checksum ^= b
    body += bytes((len(body) | 15) - len(body)) + bytes([checksum])
    return body + hashlib.sha256(body).digest()


def bank():
    return struct.pack("<48I", 0x10001000, *([0x20C1] * 47)) + bytes(m.NXP_BANK_BYTES - 192)


class Socket:
    def __init__(self, chunks=()):
        self.chunks, self.sent, self.closed, self.fail_send = list(chunks), [], False, False
    def settimeout(self, value):
        if value <= 0: raise AssertionError("invalid timeout")
    def sendall(self, data):
        self.sent.append(data)
        if self.fail_send: raise OSError("lost send result")
    def recv(self, _):
        if not self.chunks: raise TimeoutError("no response")
        value = self.chunks.pop(0)
        if isinstance(value, BaseException): raise value
        return value
    def close(self): self.closed = True


def make_session(chunks=()):
    clock, audit, sock = Clock(), MemoryAudit(), Socket(chunks)
    session = m.StockSession(m.Target("192.168.86.249", "Storm Fill"), audit,
                             connector=lambda *_a, **_k: sock, clock=clock.now, sleep=clock.sleep)
    session.sock = sock
    return session, sock, clock, audit


class LoaderModel:
    """Complete staged-bank model. Failures can occur after a write was consumed."""
    def __init__(self, failure=None, corrupt=None):
        self.time, self.audit = Clock(), MemoryAudit()
        self.clock, self.sleep = self.time.now, self.time.sleep
        self.poisoned, self.failure, self.corrupt = False, failure, corrupt
        self.index, self.commands, self.staging, self.quiet_periods = 0, [], bytearray(m.NXP_BANK_BYTES), []
    def _remaining(self, deadline):
        if self.clock() >= deadline: raise TimeoutError("overall deadline")
        return deadline - self.clock()
    def exchange(self, cls, op, payload=b"", *, mutation=False, deadline=None):
        if deadline is not None: self._remaining(deadline)
        self.index += 1
        self.commands.append((cls, op, payload, mutation))
        if self.index == self.failure:
            self.poisoned = True
            raise TimeoutError("consumed request, lost response")
        if op == 1:
            self.staging[:] = b"\xff" * len(self.staging)
            return payload
        address = int.from_bytes(payload[1:5], "big") - 0x2000
        if op == 2:
            self.staging[address:address+64] = payload[5:]
            return payload
        if op == 0x83:
            data = bytes(self.staging[address:address+64])
            if address == self.corrupt: data = bytes([data[0] ^ 1]) + data[1:]
            return payload[:5] + data
        raise AssertionError("unreviewed command")
    def send(self, data, label, deadline, *, mutation=False):
        self._remaining(deadline)
        self.commands.append((data[11], data[12], data[13:], mutation))
        if self.failure == "end" and data[12] == 5:
            self.poisoned = True
            raise OSError("End may have arrived")
    def quiet(self, seconds): self.quiet_periods.append(seconds); self.sleep(seconds)


class FlowSession:
    """Application/diagnostic peer for testing the staged orchestration itself."""
    def __init__(self):
        self.time, self.audit = Clock(), MemoryAudit()
        self.clock, self.sleep = self.time.now, self.time.sleep
        self.target = m.Target("192.168.86.249", "Storm Fill")
        self.routing_identity = bytes.fromhex("e89c251255c2")
        self.poisoned, self.firmware, self.confirmed = False, "stock", False
        self.owner, self.commands, self.completed = None, [], False
        self.failure, self.corrupt, self.started, self.no_spi_until = None, False, 0, 0
    def connect(self, **kwargs): self.commands.append(("connect",))
    def network_get(self, op):
        return b"\x0aStorm Fill" if op == 0x88 else bytes([1, 0, 13, 0])
    def send(self, frame, label, deadline, mutation=False):
        self.commands.append((frame[11], frame[12], frame[13:]))
        if frame[11:13] == b"\0\x04": self.firmware = "resident"
    def quiet(self, seconds):
        self.commands.append(("quiet", seconds)); self.sleep(seconds)
        if seconds >= 33: self.firmware = "resident"
    def exchange(self, cls, op, payload=b"", mutation=False, deadline=None):
        if self.clock() < self.no_spi_until: raise AssertionError("SPI during bounded diagnostic sequence")
        self.commands.append((cls, op, payload))
        if (cls, op) == self.failure: raise TimeoutError("injected unknown reply")
        if cls == 16:
            if self.firmware != "resident": raise m.RemoteError("not loader")
            if op == 0x80: return m.NXP_INFORMATION + bytes(71)
            if op == 0x83: return payload
        if cls == 15: return bytes(6) if op == 0x82 else payload
        if cls == 3: return b"\0\x20\0\0" if op == 0x83 else payload
        if cls == 0:
            if op == 0x87: return bytes([1,3,0,0]) if self.firmware == "stock" else bytes([0,1,0,0])
            if op == 0x84: return b"\0"
            if op == 0xFE: return b"\0\0\xbc\x40"
            if op == 0xFC:
                role = 2 if self.firmware == "lighting" else 1
                return b"OKLC\x01\0" + bytes([role, int(self.confirmed)]) + struct.pack(">4I", 3 if role==2 else 1, 0xBC40, 5000, 16)
            if op == 0x49: self.owner = payload; return payload
            if op == 0xC9: return self.owner[:8+self.owner[7]]
            if op == 0xFD: self.confirmed = True; return b"\x01"
            if op in (0x70, 0x71):
                self.completed, self.started = True, self.clock()
                self.no_spi_until = self.clock() + (0.6 if op == 0x70 else 6.7)
                return payload
            if op == 0xF0:
                return self.firmware.encode() + bytes([3 if self.completed else 0]) + (
                    b"\0\x01\x90" if self.firmware == "OFF1" else b"\x05\0\x64")
            if op == 0xF1:
                count = 14 if self.firmware == "OFF1" else 16
                words = complete(self.firmware) if self.completed else initial(self.firmware)
                raw = bytearray(pack(words))
                if self.corrupt and self.completed: raw[8] ^= 1
                return self.firmware.encode() + bytes([payload[0], count, 64, 0]) + bytes(raw[payload[0]*64:(payload[0]+1)*64])
        raise AssertionError((cls, op, payload))


def flow():
    packages = {}
    for index, name in enumerate(("identity", "OFF1", "LOW1", "lighting")):
        raw = bank()[:-1] + bytes([index])
        packages[name] = build_package(raw, (0,1,0,0), "lighting" if name=="lighting" else "diagnostic")[0]
    s = FlowSession()
    migration = m.Migration(s, packages, {k:m.digest(v) for k,v in packages.items()},
                            restore_bank=bank(), restore_sha256=m.digest(bank()))
    def transfer(session, raw, deadline):
        stage = next(k for k,v in packages.items() if v[64:] == raw)
        session.commands.append(("install", stage))
        session.firmware, session.completed, session.confirmed = stage, False, False
        return {"commit_attempted":True,"program_blocks":448,"verified_blocks":448}
    return migration, s, transfer


class MigrationTests(unittest.TestCase):
    def setUp(self):
        self.no_network = patch("socket.socket", side_effect=AssertionError("Real sockets forbidden"))
        self.no_network.start()
    def tearDown(self): self.no_network.stop()

    def test_app_inspection_and_transfer_exact_bytes(self):
        data = esp_image()
        self.assertEqual(m.inspect_esp_application(data)["project"], "open_keylight")
        packets = list(m.esp_transfer_frames(data))
        self.assertEqual(b"".join(p[6:] for p in packets[1:-1]), data)
        self.assertEqual(packets[-1], bytes.fromhex("aa0000060102"))

    def test_invalid_image_before_first_packet(self):
        data = bytearray(esp_image()); data[-1] ^= 1
        with self.assertRaises(ValueError): next(m.esp_transfer_frames(bytes(data)))

    def test_segment_address_and_mmu_rejected_before_digest(self):
        for address in (0, 0x40000000, 0x3F400024, 0xFFFFFFFF):
            data = bytearray(esp_image()); struct.pack_into("<I", data, 24, address)
            with self.subTest(address=address), self.assertRaises(ValueError): m.inspect_esp_application(bytes(data))

    def test_controller_package_role_digest_vector_bounds(self):
        data, _ = build_package(bank(), (0, 1, 0, 0), "diagnostic")
        self.assertEqual(m.inspect_controller_package(data)["role"], 1)
        for index in (0, 8, 12, 16, 20, 21, 22, 23, 28, 60, 64, 68, len(data)-1):
            damaged = bytearray(data); damaged[index] ^= 4
            with self.subTest(index=index), self.assertRaises(ValueError): m.inspect_controller_package(bytes(damaged))

    def test_target_no_scan_or_external_host(self):
        for host in ("example.com", "127.0.0.1", "0.0.0.0", "8.8.8.8", "224.0.0.1", "::1"):
            with self.subTest(host=host), self.assertRaises(ValueError): m.Target(host, "Fill")

    def test_audit_exclusive_and_hash_chain(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "audit.jsonl"
            audit = m.Audit(path, "192.168.86.249")
            audit.record("intent", mutation=True); audit.close()
            lines = path.read_text().splitlines()
            self.assertEqual(json.loads(lines[1])["previous_sha256"], m.digest(lines[0].encode()))
            with self.assertRaises(FileExistsError): m.Audit(path, "other")

    def test_audit_failure_prevents_wire(self):
        session, sock, _, audit = make_session()
        audit.fail = "send_intent"
        with self.assertRaises(OSError): session.exchange(0, 0x87)
        self.assertEqual(sock.sent, [])

    def test_hello_fragmentation_and_owner_tag(self):
        mac = bytes.fromhex("e89c251255c2")
        owner = reply(0, 0x49, b"\x01" + mac + bytes(73))
        hello = bytes.fromhex("aa02001c02") + m.HELLO[5:] + bytes(9)
        for split in range(1, len(owner + hello)):
            data = owner + hello
            session, sock, _, _ = make_session([data[:split], data[split:]])
            session.sock = None
            session.connect()
            self.assertEqual(session.routing_identity, mac)
            self.assertEqual(sock.sent, [m.HELLO])

    def test_hello_no_owner_or_wrong_echo_rejected(self):
        hello = bytes.fromhex("aa02001302") + m.HELLO[5:]
        for data in (hello, hello[:-1] + b"\xff"):
            session, _, _, _ = make_session([data]); session.sock = None
            with self.assertRaises(ValueError): session.connect()

    def test_captured_loader_hello_has_no_owner_notification(self):
        # Exact observed loader-only shape: HELLO plus opaque session trailer;
        # no invented NXP owner/tag is necessary or accepted for application claims.
        hello = bytes.fromhex("aa02001c020912022026010900110000401500000050d3d80050d3d8")
        session, _, _, _ = make_session([hello]); session.sock = None
        session.connect(require_owner=False)
        self.assertIsNone(session.routing_identity)

    def test_complete_correlated_reply_and_unrelated_notification(self):
        frame = reply(0, 0x87, b"\x01\x03\0\0")
        session, _, _, _ = make_session([b"\xaa\x02\0\x05\x85" + frame])
        self.assertEqual(session.exchange(0, 0x87), b"\x01\x03\0\0")

    def test_partial_tail_poisons_and_no_next_send(self):
        session, sock, _, _ = make_session([reply(0, 0x87, bytes(4)) + b"\xaa"])
        with self.assertRaises(ValueError): session.exchange(0, 0x87)
        self.assertTrue(session.poisoned)
        with self.assertRaises(ValueError): session.exchange(0, 0x87)
        self.assertEqual(len(sock.sent), 1)

    def test_nack_is_correlated_but_timeout_is_unknown(self):
        session, _, _, _ = make_session([reply(16, 0x80, bytes(80), status=5)])
        with self.assertRaises(m.RemoteError): session.exchange(16, 0x80, bytes(80))
        self.assertFalse(session.poisoned)
        session, _, _, _ = make_session([])
        with self.assertRaises(TimeoutError): session.exchange(16, 0x80, bytes(80))
        self.assertTrue(session.poisoned)

    def test_corrupt_report_is_unknown_not_retryable(self):
        for index in (6, 7, 9, 93, 94):
            data = bytearray(reply(0, 0x87, bytes(4))); data[index] ^= 1
            session, _, _, _ = make_session([bytes(data)])
            with self.subTest(index=index), self.assertRaises((ValueError, TimeoutError)): session.exchange(0, 0x87)
            self.assertTrue(session.poisoned)

    def test_send_exception_is_unknown(self):
        session, sock, _, _ = make_session(); sock.fail_send = True
        with self.assertRaises(OSError): session.exchange(0, 4, b"\x01\0", mutation=True)
        self.assertTrue(session.poisoned)
        self.assertEqual(len(sock.sent), 1)

    def test_late_reply_or_audit_delay_cannot_extend_deadline(self):
        for delay_at_audit in (False, True):
            session, sock, clock, audit = make_session([reply(0,0x87,bytes(4))])
            if delay_at_audit:
                record = audit.record
                def delayed_record(event, **details):
                    record(event, **details)
                    if event == "received": clock.sleep(5)
                audit.record = delayed_record
            else:
                receive = sock.recv
                def late_receive(size):
                    clock.sleep(5)
                    return receive(size)
                sock.recv = late_receive
            with self.assertRaises(TimeoutError): session.exchange(0,0x87)
            self.assertTrue(session.poisoned)
            self.assertEqual(len(sock.sent),1)

    def test_quiet_holds_close_even_keyboard_interrupt(self):
        session, sock, clock, _ = make_session()
        calls = []
        def sleep(seconds):
            calls.append(seconds)
            if len(calls) == 1: raise KeyboardInterrupt
            clock.sleep(seconds)
        session.sleep = sleep
        with self.assertRaises(KeyboardInterrupt): session.quiet(3)
        self.assertEqual(clock.now(), 4)
        session.close(); self.assertTrue(sock.closed)

    def test_full_bank_exact_bounds_and_commit(self):
        s = LoaderModel()
        result = m.transfer_controller(s, bank(), deadline=200)
        self.assertEqual(result["program_blocks"], 448)
        self.assertEqual(result["verified_blocks"], 448)
        self.assertEqual(s.commands[0][2], struct.pack(">II", 0x2000, 0x8FFF))
        self.assertEqual([op for _, op, _, _ in s.commands].count(5), 1)
        self.assertEqual(s.quiet_periods, [3])
        self.assertEqual(bytes(s.staging), bank())

    def test_every_lost_response_stops_no_abort_or_end(self):
        for failure in range(1, 898):
            s = LoaderModel(failure=failure)
            with self.subTest(failure=failure), self.assertRaises(TimeoutError):
                m.transfer_controller(s, bank(), deadline=200)
            self.assertEqual(len(s.commands), failure)
            self.assertNotIn(5, [op for _, op, _, _ in s.commands])
            self.assertNotIn(4, [op for _, op, _, _ in s.commands])

    def test_corrupt_verified_bank_aborts_once_never_commits(self):
        for address in (0, 0x3FC0, 0x6FC0):
            s = LoaderModel(corrupt=address)
            with self.assertRaises(ValueError): m.transfer_controller(s, bank(), deadline=200)
            self.assertEqual([op for _, op, _, _ in s.commands].count(4), 1)
            self.assertNotIn(5, [op for _, op, _, _ in s.commands])
            self.assertEqual(s.quiet_periods, [3])

    def test_lost_end_no_abort_and_preserves_quiet(self):
        s = LoaderModel(failure="end")
        with self.assertRaises(OSError): m.transfer_controller(s, bank(), deadline=200)
        self.assertEqual([op for _, op, _, _ in s.commands].count(5), 1)
        self.assertNotIn(4, [op for _, op, _, _ in s.commands])
        self.assertEqual(s.quiet_periods, [3])

    def test_invalid_bank_no_cleanup_or_wire(self):
        s = LoaderModel()
        with self.assertRaises(ValueError): m.transfer_controller(s, b"bad", deadline=200)
        self.assertEqual(s.commands, [])

    def test_expired_overall_deadline_never_erase(self):
        s = LoaderModel()
        with self.assertRaises(TimeoutError): m.transfer_controller(s, bank(), deadline=1)
        self.assertEqual([op for _, op, _, _ in s.commands], [4])

    def test_esp_final_success_cannot_hide_late_error(self):
        model = m.EspNotifications(); model.end_attempted = True
        model.feed(bytes.fromhex("aa020007016401"))
        self.assertTrue(model.success)
        with self.assertRaises(ValueError): model.feed(bytes.fromhex("aa020007016400"))

    def test_esp_premature_extra_payload_and_bad_percent(self):
        for data in ("aa020007016401", "aa02000801010100", "aa020007016501", "aa030007010101"):
            with self.subTest(data=data), self.assertRaises(ValueError): m.EspNotifications().feed(bytes.fromhex(data))

    def test_stage_integrity_and_order_no_silicon_inference(self):
        diagnostic, _ = build_package(bank(), (0, 1, 0, 0), "diagnostic")
        lighting, _ = build_package(bank(), (0, 1, 0, 0), "lighting")
        packages = {"identity": diagnostic, "OFF1": diagnostic, "LOW1": diagnostic, "lighting": lighting}
        pins = {k: m.digest(v) for k, v in packages.items()}
        session, sock, _, _ = make_session()
        migration = m.Migration(session, packages, pins, restore_bank=bank(), restore_sha256=m.digest(bank()))
        with self.assertRaises(ValueError): migration.install("lighting")
        self.assertEqual(sock.sent, [])
        migration.phase, migration.fresh_resident = "resident", True
        with self.assertRaises(ValueError): migration.install("OFF1")
        self.assertEqual(sock.sent, [])
        self.assertEqual(migration.phase, "failed")

    def test_diagnostic_cannot_confirm_or_auto_approve(self):
        diagnostic, _ = build_package(bank(), (0, 1, 0, 0), "diagnostic")
        lighting, _ = build_package(bank(), (0, 1, 0, 0))
        packages = dict(identity=diagnostic, OFF1=diagnostic, LOW1=diagnostic, lighting=lighting)
        session, sock, _, _ = make_session()
        migration = m.Migration(session, packages, {k:m.digest(v) for k,v in packages.items()},
                                restore_bank=bank(), restore_sha256=m.digest(bank()))
        migration.phase, migration.current, migration.part_verified = "trial", "LOW1", True
        with self.assertRaises(ValueError): migration.confirm_controller()
        self.assertEqual(sock.sent, [])
        migration.phase, migration.completed_profiles = "resident", {"OFF1", "LOW1"}
        with self.assertRaises(ValueError): migration.accept_physical_checks(off_was_dark=True, low_channels_expected=False)
        self.assertFalse(migration.lighting_approved)

    def test_complete_stage_order_and_real_profile_validation(self):
        migration, s, transfer = flow()
        with patch.object(m, "inspect_nxp_loader", return_value={"reference_code_matches":True}), \
             patch.object(m, "transfer_controller", side_effect=transfer):
            migration.open_stock(); migration.enter_stock_loader()
            migration.install("identity"); migration.await_recovery()
            for stage in ("OFF1", "LOW1"):
                migration.install(stage)
                raw = migration.run_diagnostic()
                self.assertEqual(len(raw), 896 if stage=="OFF1" else 1024)
                migration.await_recovery()
            migration.accept_physical_checks(off_was_dark=True, low_channels_expected=True)
            migration.install("lighting"); migration.confirm_controller()
        self.assertEqual(migration.phase, "controller_confirmed")
        self.assertEqual([x for x in s.commands if x[0] == "install"],
                         [("install", v) for v in ("identity", "OFF1", "LOW1", "lighting")])
        self.assertEqual(sum(x[:2] == (0, 0xFD) for x in s.commands), 1)
        self.assertEqual(sum(x[:2] == (0, 0x70) for x in s.commands), 1)
        self.assertEqual(sum(x[:2] == (0, 0x71) for x in s.commands), 1)
        self.assertEqual(sum(x == ("quiet", 33) for x in s.commands), 3)
        self.assertEqual(sum(event == "diagnostic_registers_verified" for event,_ in s.audit.events), 2)

    def test_diagnostic_corruption_stops_before_recovery_proof(self):
        for stage in ("OFF1", "LOW1"):
            migration, s, _ = flow()
            migration.phase, migration.current, migration.part_verified = "trial", stage, True
            s.firmware, s.corrupt = stage, True
            with self.assertRaises(ValueError): migration.run_diagnostic()
            self.assertEqual(migration.phase, "failed")
            self.assertFalse(migration.fresh_resident)
            self.assertFalse(migration.record_verified)
            with self.assertRaises(ValueError): migration.await_recovery()
            self.assertNotIn(("quiet", 33), s.commands)

    def test_lost_trigger_ack_never_retry_or_read_snapshot(self):
        migration, s, _ = flow()
        migration.phase, migration.current = "trial", "LOW1"
        s.firmware, s.failure = "LOW1", (0, 0x71)
        with self.assertRaises(TimeoutError): migration.run_diagnostic()
        self.assertEqual(sum(x[:2] == (0, 0x71) for x in s.commands), 1)
        self.assertEqual(sum(x[:2] == (0, 0xF1) for x in s.commands), 1)
        self.assertFalse(migration.fresh_resident)

    def test_cold_recovery_requires_ack_and_exact_resident(self):
        migration, s, _ = flow()
        with self.assertRaises(ValueError): migration.open_cold_recovery(whole_light_power_cycled=False)
        self.assertEqual(s.commands, [])
        migration, s, _ = flow()
        with self.assertRaises(m.RemoteError): migration.open_cold_recovery(whole_light_power_cycled=True)
        self.assertFalse(migration.fresh_resident)
        self.assertFalse(any(x[:2] == (16,0x83) for x in s.commands))

    def test_restore_is_explicit_and_not_available_after_failed_trial(self):
        migration, s, _ = flow()
        migration.phase, migration.fresh_resident = "resident", True
        with self.assertRaises(ValueError): migration.restore_owner_bank(bytes([1,3,0,0]))
        self.assertEqual(s.commands, [])
        migration, s, _ = flow(); s.firmware = "resident"
        with patch.object(m, "inspect_nxp_loader", return_value={"reference_code_matches":True}), \
             patch.object(m, "transfer_controller") as transfer:
            migration.open_cold_recovery(whole_light_power_cycled=True)
            transfer.side_effect = lambda *_a, **_k: setattr(s,"firmware","stock")
            migration.restore_owner_bank(bytes([1,3,0,0]))
            transfer.assert_called_once()
        self.assertEqual(migration.phase,"restored_application_seen")

    def test_live_esp_receiver_full_transfer_and_late_failure(self):
        for late_error in (False, True):
            session, sock, _, audit = make_session()
            session.clock, session.sleep = time.monotonic, time.sleep
            condition, queue = threading.Condition(), []
            def sendall(data):
                sock.sent.append(data)
                if data == bytes.fromhex("aa0000060102"):
                    with condition:
                        queue.append(bytes.fromhex("aa020007016401"))
                        if late_error: queue.append(bytes.fromhex("aa020007016400"))
                        queue.append(b""); condition.notify_all()
            def ready(*_args):
                with condition:
                    if not queue: condition.wait(0.01)
                    return ([sock] if queue else [], [], [])
            def recv(_size):
                with condition: return queue.pop(0)
            sock.sendall, sock.recv = sendall, recv
            if late_error:
                with self.assertRaises(ValueError): m.transfer_esp(session, esp_image(), selector=ready)
                self.assertFalse(any(e == "esp_transfer_accepted" for e,_ in audit.events))
            else:
                result = m.transfer_esp(session, esp_image(), selector=ready)
                self.assertEqual(result["bytes_sent"], len(esp_image()))
                self.assertFalse(result["trial_confirmed"])
            self.assertEqual(sock.sent.count(bytes.fromhex("aa0000060102")), 1)


if __name__ == "__main__": unittest.main()
