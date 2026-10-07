"""Existing-original finish admission and three-stage CLI; real sockets forbidden."""
import io
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import stock_migration as m
from test_cli import manifest_fixture
from test_migration import Clock, MemoryAudit
from test_jsonl import Input


class ExistingPeer:
    def __init__(self, plan, *, confirmed=False, uptime=5000):
        self.target, self.audit = plan['target'], MemoryAudit()
        self.time = Clock(); self.clock, self.sleep = self.time.now, self.time.sleep
        self.boot = self.clock() - uptime / 1000
        self.confirmed, self.reset, self.owner = confirmed, 1, None
        self.routing_identity = None
        self.actual_tag = bytes.fromhex('e89c251255c2')
        self.version = plan['packages']['lighting'][24:28]
        self.part, self.role, self.caps, self.requested = 0xbc40, 2, 3, False
        self.commands, self.hook = [], None
        self.bad_owner, self.bad_off, self.lost_fd = False, False, False
        self.closed, self.poisoned, self.events = False, False, None

    def connect(self, *, require_owner=True):
        if require_owner: raise AssertionError('Owner notification is intentionally absent')
        self.commands.append(('connect', False))

    def network_get(self, op):
        self.commands.append(('network', op))
        if op == 0x84: return self.target.esp_version
        if op == 0x88:
            name = self.target.name.encode()
            return bytes([len(name)]) + name
        raise AssertionError('Unexpected network getter')

    def exchange(self, cls, op, payload=b'', *, mutation=False, deadline=None):
        if deadline is not None and self.clock() >= deadline: raise TimeoutError('deadline before send')
        self.commands.append((cls, op, payload, mutation))
        if self.hook: self.hook(cls, op)
        if deadline is not None and self.clock() >= deadline: raise TimeoutError('deadline after response')
        if (cls, op) == (0, 0x87): return self.version
        if (cls, op) == (0, 0xFE): return self.part.to_bytes(4, 'big')
        if (cls, op) == (0, 0xFC):
            return b'OKLC\x01\0' + bytes([self.role, self.confirmed | (self.requested << 1)]) + \
                struct.pack('>4I', self.caps, self.part, int((self.clock() - self.boot) * 1000), self.reset)
        if (cls, op) == (0, 0x49):
            assert mutation and len(payload) == 72 and payload[:7] == b'\1' + bytes(6)
            assert payload[8:8 + payload[7]].startswith(b'OKL finish ')
            self.owner = b'\1' + self.actual_tag + payload[7:8 + payload[7]]
            return payload
        if (cls, op) == (0, 0xC9):
            return self.owner[:-1] + b'!' if self.bad_owner else self.owner
        if (cls, op) == (15, 0x82): return b'\0\0\1\0\0\0' if self.bad_off else bytes(6)
        if (cls, op) == (3, 0x83): return b'\0\x20\0\0'
        if (cls, op) == (0, 0xFD):
            assert mutation and payload == b'OKLC'
            self.confirmed = True
            if self.lost_fd: raise TimeoutError('FD outcome unknown')
            return b'\1'
        raise AssertionError(f'Unexpected controller command {cls:02x}/{op:02x}')

    def close(self): self.closed = True


class FinishTests(unittest.TestCase):
    def setUp(self):
        self.block = patch('socket.socket', side_effect=AssertionError('Real network forbidden'))
        self.block.start(); self.addCleanup(self.block.stop)
        self.temp = tempfile.TemporaryDirectory(); self.addCleanup(self.temp.cleanup)
        self.folder = Path(self.temp.name)
        self.path, self.manifest, self.assets = manifest_fixture(self.folder)
        self.plan = m.load_plan(self.path)

    def migration(self, peer):
        return m.Migration(peer, self.plan['packages'], self.plan['pins'],
                           restore_bank=self.plan['restore_bank'], restore_sha256=self.plan['restore_sha256'])

    def admit(self, peer, **kw):
        migration = self.migration(peer)
        def mac(target, device_id, audit):
            self.assertEqual(device_id, 'keylight-123456')
            self.assertEqual(peer.commands, [])
            peer.commands.append(('udp',))
            return bytes.fromhex('021111123456')
        migration.open_existing(self.plan['device_id'], mac_reader=mac, **kw)
        return migration

    def test_unconfirmed_one_nonce_claim_and_fd_no_controller_flash(self):
        peer = ExistingPeer(self.plan)
        migration = self.admit(peer, nonce_factory=lambda: '0123456789abcdef')
        writes = [(c[0], c[1]) for c in peer.commands if len(c) == 4 and c[3]]
        self.assertEqual(writes, [(0, 0x49), (0, 0xFD)])
        self.assertEqual(peer.routing_identity, peer.actual_tag)
        self.assertEqual(migration.phase, 'controller_confirmed')
        self.assertFalse(migration.part_verified)
        self.assertFalse(migration.lighting_approved)
        self.assertEqual(migration.completed_profiles, set())
        event = next(d for e, d in peer.audit.events if e == 'existing_controller_admitted')
        self.assertFalse(event['controller_flash']); self.assertFalse(event['optical_observation'])
        self.assertFalse(event['diagnostics_performed']); self.assertTrue(event['confirmation_sent'])

    def test_confirmed_old_app_no_fd_and_nonce_is_fresh(self):
        labels = []
        for _ in range(2):
            peer = ExistingPeer(self.plan, confirmed=True, uptime=100000)
            self.admit(peer)
            self.assertFalse(any(c[:2] == (0, 0xFD) for c in peer.commands))
            labels.append(peer.owner[8:])
        self.assertNotEqual(*labels)

    def test_unknown_metadata_rejected_before_claim(self):
        cases = [('version', b'\1\3\0\0'), ('version', b''), ('part', 1), ('role', 1),
                 ('caps', 1), ('caps', 7), ('requested', True)]
        for field, value in cases:
            with self.subTest(field=field, value=value):
                peer = ExistingPeer(self.plan); setattr(peer, field, value)
                with self.assertRaises(ValueError): self.admit(peer)
                self.assertFalse(any(len(c) == 4 and c[3] for c in peer.commands))

    def test_real_cli_entrypoint_and_no_execution(self):
        script = Path(m.__file__)
        help_result = subprocess.run([sys.executable, str(script), 'finish', '--help'], capture_output=True, text=True, timeout=5)
        self.assertEqual(help_result.returncode, 0, help_result.stderr)
        self.assertIn('finish', help_result.stdout)
        refused = subprocess.run([sys.executable, str(script), 'finish', '--manifest', str(self.path)],
                                 capture_output=True, text=True, timeout=5)
        self.assertEqual(refused.returncode, 1)
        self.assertIn('require --execute', refused.stdout)

    def test_udp_identity_failure_precedes_tcp(self):
        peer = ExistingPeer(self.plan); migration = self.migration(peer)
        def fail(*a): raise ValueError('MAC mismatch')
        with self.assertRaises(ValueError): migration.open_existing(self.plan['device_id'], mac_reader=fail)
        self.assertEqual(peer.commands, [])

    def test_nonce_and_off_mismatch_refuse_fd(self):
        for field in ('bad_owner', 'bad_off'):
            peer = ExistingPeer(self.plan); setattr(peer, field, True)
            with self.assertRaises(ValueError): self.admit(peer)
            self.assertFalse(any(c[:2] == (0, 0xFD) for c in peer.commands))

    def test_lost_fd_ack_never_retried(self):
        peer = ExistingPeer(self.plan); peer.lost_fd = True
        with self.assertRaises(TimeoutError): self.admit(peer)
        self.assertEqual(sum(c[:2] == (0, 0xFD) for c in peer.commands), 1)

    def test_late_initial_or_preclaim_clock_expiry_no_write(self):
        peer = ExistingPeer(self.plan, uptime=15000)
        with self.assertRaises(ValueError): self.admit(peer)
        self.assertFalse(any(len(c) == 4 and c[3] for c in peer.commands))
        peer = ExistingPeer(self.plan, uptime=14000)
        def late_nonce(): peer.time.sleep(6); return 'a' * 16
        with self.assertRaises(ValueError): self.admit(peer, nonce_factory=late_nonce)
        self.assertFalse(any(len(c) == 4 and c[3] for c in peer.commands))

    def test_lifecycle_change_or_delayed_claim_no_fd(self):
        for change in ('reset', 'boot', 'stall'):
            peer = ExistingPeer(self.plan, uptime=14000)
            def hook(cls, op):
                if op == 0x49:
                    if change == 'reset': peer.reset += 1
                    elif change == 'boot': peer.boot -= 2
                    else: peer.time.sleep(6)
            peer.hook = hook
            with self.subTest(change=change), self.assertRaises((ValueError, TimeoutError)): self.admit(peer)
            self.assertFalse(any(c[:2] == (0, 0xFD) for c in peer.commands))

    def test_no_esp_upload_if_confirmed_controller_restarts_between_stages(self):
        peer = ExistingPeer(self.plan, confirmed=True)
        migration = self.admit(peer)
        peer.reset += 1
        with patch.object(m, 'transfer_esp', side_effect=AssertionError('No upload')):
            with self.assertRaises(ValueError): migration.install_esp(self.plan['esp_image'], self.plan['esp_metadata']['sha256'])

    def test_exact_udp_reply_all_bytes_deadlines_no_retry(self):
        class Audit:
            def record(self, *a, **kw): pass
        calls = []
        raw = bytes.fromhex('aa02000b89021111123456')
        reply = [raw]
        clock = Clock()
        class Socket:
            def __enter__(self): return self
            def __exit__(self, *a): pass
            def settimeout(self, value): pass
            def connect(self, address): calls.append(('connect', address))
            def send(self, data): calls.append(('send', data)); return len(data)
            def recv(self, size): return reply[0]
        kwargs = dict(socket_factory=lambda *a: Socket(), clock=clock.now)
        self.assertEqual(m.read_stock_mac(self.plan['target'], self.plan['device_id'], Audit(), **kwargs), raw[5:])
        self.assertEqual(calls[1], ('send', bytes.fromhex('aa00000589')))
        for index in list(range(5)) + list(range(8, 11)):
            reply[0] = raw[:index] + bytes([raw[index] ^ 1]) + raw[index + 1:]
            with self.subTest(index=index), self.assertRaises(ValueError):
                m.read_stock_mac(self.plan['target'], self.plan['device_id'], Audit(), **kwargs)
        for bad in (raw + b'\0', raw[:-1], raw[:5] + bytes(6), raw[:5] + b'\3' + raw[6:]):
            reply[0] = bad
            with self.assertRaises(ValueError): m.read_stock_mac(self.plan['target'], self.plan['device_id'], Audit(), **kwargs)
        class TimeoutSocket(Socket):
            def recv(self, size): raise TimeoutError('UDP loss')
        before = len(calls)
        with self.assertRaises(TimeoutError):
            m.read_stock_mac(self.plan['target'], self.plan['device_id'], Audit(), socket_factory=lambda *a: TimeoutSocket())
        self.assertEqual(len(calls) - before, 2)

    def test_real_cli_three_stages_only_esp_transfer_no_optical_prompts(self):
        peer = ExistingPeer(self.plan)
        events, source = io.StringIO(), Input()
        self.addCleanup(lambda: source.lines.put(''))
        upload = []
        def session(target, audit, **kw): peer.audit = audit; return peer
        def transfer(s, raw):
            upload.append(raw)
            self.assertTrue(peer.confirmed)
            return {'bytes_sent': len(raw), 'device_reported_success': True, 'trial_confirmed': False}
        def observer(*a, **kw):
            self.assertTrue(peer.closed)
            kw['events'].emit('action', kind='native_acceptance', manifest_sha256=self.plan['manifest_sha256'])
        args = ['finish', '--manifest', str(self.path), '--audit', str(self.folder / 'finish.jsonl'),
                '--execute', '--exclusive-control', '--events-jsonl', '--expected-manifest-sha256', self.plan['manifest_sha256']]
        with patch.object(m, 'transfer_controller', side_effect=AssertionError('No controller flash')), \
             patch.object(m, 'transfer_esp', side_effect=transfer), \
             patch.object(m, 'await_native_confirmation', side_effect=observer):
            result = m.run_cli(args, session_factory=session, clock=peer.clock, sleep=peer.sleep,
                               event_input=source, event_output=events, mac_reader=lambda *a: b'\2\x11\x11\x12\x34\x56',
                               input_fn=lambda *a: self.fail('No optical prompt'))
        self.assertEqual(result, 0)
        self.assertEqual(upload, [self.plan['esp_image']])
        rows = [json.loads(line) for line in events.getvalue().splitlines()]
        workflow = next(r for r in rows if r.get('code') == 'installation_workflow')
        self.assertEqual(workflow['workflow'], 'finish'); self.assertEqual(workflow['total'], 3)
        self.assertTrue(workflow['message'])
        self.assertEqual([(s['id'], s['index']) for s in workflow['stages']], [('existing', 1), ('esp', 2), ('native', 3)])
        stages = [r for r in rows if r['event'] == 'stage']
        self.assertEqual([(r['id'], r['phase']) for r in stages],
                         [(s, p) for s in ('existing', 'esp', 'native') for p in ('started', 'completed')])
        self.assertTrue(all(r['total'] == 3 for r in stages))
        self.assertFalse(any(r['event'] == 'prompt' for r in rows))
        self.assertEqual(rows[-1]['outcome'], 'installed')


if __name__ == '__main__': unittest.main()
