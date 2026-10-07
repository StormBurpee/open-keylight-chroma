"""Guided stock-migration CLI tests. Every network boundary is replaced."""
import copy
import gzip
import hashlib
import json
from pathlib import Path
import sys
import struct
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import stock_migration as m
from package_controller import build_package
from test_migration import Clock, FlowSession, bank, esp_image


def manifest_fixture(directory):
    """Synthetic original artifacts, never a vendor image or a real target."""
    directory = Path(directory)
    assets = {"/index.html": b"<html>Original test page</html>",
              "/assets/test.js": b"console.log('test')", "/assets/test.css": b"body{}"}
    assets_manifest = {"version": 1, "files": [
        {"path": path, "size": len(raw), "sha256": hashlib.sha256(raw).hexdigest()}
        for path, raw in assets.items()]}
    packages = {}
    for index, stage in enumerate(("identity", "OFF1", "LOW1", "lighting")):
        raw = bank()[:-1] + bytes([index])
        package = build_package(raw, (0, 1, 0, 0), "lighting" if stage == "lighting" else "diagnostic")[0]
        path = directory / (stage + ".oklnxp"); path.write_bytes(package)
        packages[stage] = {"path": path.name, "sha256": m.digest(package)}
    restore = directory / "owner-restore.bin"; restore.write_bytes(bank())
    image = esp_image()
    content = image[32:224] + b"".join(gzip.compress(raw, mtime=0) for raw in assets.values())
    content += bytes(-len(content) % 4)
    body = image[:24] + struct.pack("<II", 0x3F400020, len(content)) + content
    body += struct.pack("<II", 0x40080000, 4) + bytes(4)
    checksum = 0xEF
    for value in content: checksum ^= value
    body += bytes((len(body) | 15) - len(body)) + bytes([checksum])
    esp = directory / "open-keylight.bin"; esp.write_bytes(body + hashlib.sha256(body).digest())
    asset_path = directory / "asset-manifest.json"
    asset_path.write_text(json.dumps(assets_manifest))
    manifest = {"format": 1, "profile": "keylight-chroma-1.0.13", "source_commit": "a" * 40,
                "target": {"ip": "192.168.86.249", "name": "Test Key Light", "device_id": "keylight-123456"},
                "packages": packages,
                "restore": {"path": restore.name, "sha256": m.digest(restore.read_bytes()),
                            "version": "1.3.0.0", "provenance": "Synthetic reviewed test fixture only"},
                "esp": {"path": esp.name, "sha256": m.digest(esp.read_bytes())},
                "assets": {"path": asset_path.name, "sha256": m.digest(asset_path.read_bytes())}}
    path = directory / "migration.json"; path.write_text(json.dumps(manifest))
    return path, manifest, assets


class Environment:
    """Actual staged Migration orchestration over a synthetic application peer."""
    def __init__(self, test):
        self.test = test
        self.time = Clock()
        self.session = None
        self.prompts, self.output, self.http_calls = [], [], []
        self.audit_created = False
        self.closed = False
        self.answer = "yes"
        self.failure = None
        self.native_reads = 0
        self.restore = False
        self.packages = {name: (test.folder / entry['path']).read_bytes()
                         for name, entry in test.manifest['packages'].items()}

    def audit(self, path, target, **_kwargs):
        audit = m.Audit(path, target, clock=self.time.now)
        self.test.addCleanup(audit.close)
        self.audit_created = True
        self.audit_ref = audit
        return audit

    def session_factory(self, target, audit, **_kwargs):
        self.test.assertTrue(self.audit_created, "Durable exclusive audit must precede connection")
        s = self.session = FlowSession()
        s.time, s.clock, s.sleep = self.time, self.time.now, self.time.sleep
        s.target, s.audit = target, audit
        if self.restore: s.firmware = "resident"
        def network_get(op):
            name = target.name.encode()
            return bytes([len(name)]) + name if op == 0x88 else target.esp_version
        s.network_get = network_get
        def close(): self.closed = True
        s.close = close
        return s

    def transfer(self, session, raw, deadline):
        if self.restore:
            self.test.assertEqual(raw, bank())
            session.commands.append(("restore",))
            if self.failure == "restore": raise TimeoutError("Unknown restore result")
            session.firmware = "stock"
            return {"commit_attempted": True, "program_blocks": 448, "verified_blocks": 448}
        stage = next(k for k, v in self.packages.items() if v[64:] == raw)
        session.commands.append(("install", stage))
        if self.failure == stage:
            raise TimeoutError("Ambiguous injected transfer; never retry")
        session.firmware, session.completed, session.confirmed = stage, False, False
        return {"commit_attempted": True, "program_blocks": 448, "verified_blocks": 448}

    def transfer_esp(self, session, image):
        session.commands.append(("ESP",))
        self.test.assertTrue(session.confirmed)
        if self.failure == "ESP": raise TimeoutError("ESP outcome unknown")
        return {"bytes_sent": len(image), "device_reported_success": True, "trial_confirmed": False}

    def input(self, question):
        self.prompts.append(question)
        self.test.assertEqual(self.session.firmware, "resident", "No blocking prompt during a trial")
        if isinstance(self.answer, BaseException): raise self.answer
        return self.answer

    def http(self, ip, path, maximum):
        self.http_calls.append((ip, path, maximum))
        self.test.assertTrue(self.closed, "Close stock session before native HTTP acceptance")
        self.test.assertEqual(ip, self.test.manifest['target']['ip'])
        if path != "/api/v1/device":
            if self.failure == "native_asset": return b"wrong served asset"
            return self.test.assets[path]
        self.native_reads += 1
        up = int(self.time.value * 1000)
        metadata = m.inspect_esp_application((self.test.folder / "open-keylight.bin").read_bytes())
        value = {"id": self.test.manifest['target']['device_id'], "firmware": metadata['version'],
                 "firmware_elf_sha256": metadata['elf_sha256'], "api_version": 1,
                 "trial_pending": self.native_reads < 2, "uptime_ms": up, "reset_reason": 3,
                 "controller": {"ready": True, "connected": True, "backend": "original", "status": "ready",
                                "part_id": 0xBC40, "trial_confirmed": True, "version": "0.1.0.0", "last_health_ms": up}}
        if self.failure == "native_identity": value['id'] = "other"
        if self.failure == "native_unconfirmed": value['trial_pending'] = True
        if self.failure == "native_controller": value['controller']['ready'] = False
        if self.failure == "native_no_trial": value['trial_pending'] = False
        if self.failure == "native_reboot" and self.native_reads > 1: value['uptime_ms'] = 0
        if self.failure == "native_bad_json": return b'{"id":'
        return json.dumps(value).encode()

    def run(self, command="install", extra=()):
        self.restore = command == "restore"
        args = [command, "--manifest", str(self.test.path), "--audit", str(self.test.folder / "audit.jsonl"),
                "--execute", "--exclusive-control", *extra]
        with patch.object(m, "inspect_nxp_loader", return_value={"reference_code_matches": True}), \
             patch.object(m, "transfer_controller", side_effect=self.transfer), \
             patch.object(m, "transfer_esp", side_effect=self.transfer_esp):
            return m.run_cli(args, input_fn=self.input, output_fn=self.output.append,
                             session_factory=self.session_factory, audit_factory=self.audit,
                             get_http=self.http, clock=self.time.now, sleep=self.time.sleep)


class Tests(unittest.TestCase):
    def setUp(self):
        self.network = patch("socket.socket", side_effect=AssertionError("Real network forbidden"))
        self.network.start()
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.addCleanup(self.network.stop)
        self.folder = Path(self.temp.name)
        self.path, self.manifest, self.assets = manifest_fixture(self.folder)

    def test_prepare_and_default_no_execution(self):
        output = []
        def forbidden(*_args, **_kwargs): self.fail("Preparation must not contact or prompt")
        code = m.run_cli(["prepare", "--manifest", str(self.path)], input_fn=forbidden,
                         output_fn=output.append, session_factory=forbidden, get_http=forbidden)
        self.assertEqual(code, 0)
        self.assertTrue(output)

    def test_manifest_rejects_unpinned_or_invalid_artifacts(self):
        plan = m.load_plan(self.path)
        self.assertIsInstance(plan, dict)
        for path in ("identity.oklnxp", "OFF1.oklnxp", "LOW1.oklnxp", "lighting.oklnxp",
                     "owner-restore.bin", "open-keylight.bin", "asset-manifest.json"):
            artifact = self.folder / path; original = artifact.read_bytes()
            artifact.write_bytes(original[:-1] + bytes([original[-1] ^ 1]))
            with self.subTest(path=path), self.assertRaises(ValueError): m.load_plan(self.path)
            artifact.write_bytes(original)
        for field, value in (("format", 2), ("format", True), ("profile", "unknown"), ("source_commit", "short")):
            changed = copy.deepcopy(self.manifest); changed[field] = value
            self.path.write_text(json.dumps(changed))
            with self.subTest(field=field), self.assertRaises(ValueError): m.load_plan(self.path)
        self.path.write_text(json.dumps(self.manifest))

    def test_duplicate_json_bad_paths_and_role_mismatch(self):
        self.path.write_text(self.path.read_text().replace('"format": 1', '"format": 1, "format": 1'))
        with self.assertRaises(ValueError): m.load_plan(self.path)
        self.path.write_text(json.dumps(self.manifest))
        for mutation in ('role', 'unknown_key', 'device_id', 'target_ip', 'provenance'):
            document = copy.deepcopy(self.manifest)
            if mutation == 'role': document['packages']['identity'] = document['packages']['lighting']
            if mutation == 'unknown_key': document['extra'] = True
            if mutation == 'device_id': document['target']['device_id'] = 'unbound'
            if mutation == 'target_ip': document['target']['ip'] = '8.8.8.8'
            if mutation == 'provenance': document['restore']['provenance'] = ''
            self.path.write_text(json.dumps(document))
            with self.subTest(mutation=mutation), self.assertRaises(ValueError): m.load_plan(self.path)
        self.path.write_text(json.dumps(self.manifest))
        path = self.folder / 'asset-manifest.json'; original = path.read_bytes()
        for bad_path in ('/api/v1/update', '/assets/../token', '//other/index.html', 'https://example.test/file'):
            assets = json.loads(original); assets['files'][0]['path'] = bad_path
            path.write_text(json.dumps(assets))
            document = copy.deepcopy(self.manifest); document['assets']['sha256'] = m.digest(path.read_bytes())
            self.path.write_text(json.dumps(document))
            with self.subTest(path=bad_path), self.assertRaises(ValueError): m.load_plan(self.path)

    def test_execution_flags_and_existing_audit_reject_before_session(self):
        def forbidden(*_a, **_kw): self.fail('Must reject before session/HTTP/input')
        common = ['--manifest', str(self.path)]
        cases = [['install', *common], ['install', *common, '--execute'],
                 ['restore', *common, '--execute', '--exclusive-control', '--audit', str(self.folder/'a.jsonl')],
                 ['prepare', *common, '--execute']]
        for args in cases:
            with self.subTest(args=args):
                self.assertEqual(m.run_cli(args, output_fn=lambda _x: None, input_fn=forbidden,
                                           session_factory=forbidden, get_http=forbidden), 1)
        audit = self.folder / 'audit.jsonl'; audit.write_text('existing record')
        args = ['install', *common, '--execute', '--exclusive-control', '--audit', str(audit)]
        self.assertEqual(m.run_cli(args, output_fn=lambda _x: None, session_factory=forbidden,
                                   input_fn=forbidden, get_http=forbidden), 1)
        self.assertEqual(audit.read_text(), 'existing record')

    def test_complete_real_stage_orchestration_prompts_only_after_recovery(self):
        env = Environment(self)
        self.assertEqual(env.run(), 0)
        self.assertEqual(len(env.prompts), 2)
        stages = [row[1] for row in env.session.commands if row[0] == "install"]
        self.assertEqual(stages, ["identity", "OFF1", "LOW1", "lighting"])
        self.assertEqual(env.session.commands.count(("ESP",)), 1)
        confirms = [row for row in env.session.commands if row[:2] == (0, 0xFD)]
        self.assertEqual(len(confirms), 1)
        self.assertTrue(env.closed)
        self.assertTrue(env.http_calls)
        self.assertTrue(env.audit_ref.file.closed)
        records = [json.loads(line) for line in (self.folder/'audit.jsonl').read_text().splitlines()]
        self.assertTrue(any(r['event'] == 'human_dashboard_confirmation_observed' for r in records))

    def test_failed_stage_stops_without_automatic_restore_or_retries(self):
        for failure in ("identity", "OFF1", "LOW1", "lighting", "ESP"):
            with self.subTest(failure=failure):
                audit = self.folder / "audit.jsonl"
                if audit.exists(): audit.unlink()
                env = Environment(self); env.failure = failure
                self.assertNotEqual(env.run(), 0)
                self.assertTrue(env.closed)
                installs = [row[1] for row in env.session.commands if row[0] == "install"]
                self.assertEqual(len(installs), len(set(installs)))
                self.assertFalse(env.http_calls)
                if failure != "ESP": self.assertNotIn(("ESP",), env.session.commands)

    def test_negative_cancelled_or_absent_observation_never_advances(self):
        for answer in ("no", "", "not yes", EOFError(), KeyboardInterrupt()):
            with self.subTest(answer=repr(answer)):
                audit = self.folder / "audit.jsonl"
                if audit.exists(): audit.unlink()
                env = Environment(self); env.answer = answer
                self.assertNotEqual(env.run(), 0)
                stages = [row[1] for row in env.session.commands if row[0] == "install"]
                self.assertEqual(stages, ["identity", "OFF1"])
                self.assertTrue(env.closed)
                self.assertFalse(env.http_calls)

    def test_explicit_restore_requires_cold_resident_and_never_runs_forward_stages(self):
        env = Environment(self)
        self.assertEqual(env.run('restore', ('--power-cycled',)), 0)
        self.assertEqual(env.session.commands.count(('restore',)), 1)
        self.assertFalse(any(row[0] in ('install', 'ESP') or row[:2] == (0, 0xFD)
                             for row in env.session.commands))
        self.assertFalse(env.prompts or env.http_calls)
        self.assertTrue(env.closed)

    def test_restore_failure_never_retries_or_starts_install(self):
        env = Environment(self); env.failure = 'restore'
        self.assertEqual(env.run('restore', ('--power-cycled',)), 1)
        self.assertEqual(env.session.commands.count(('restore',)), 1)
        self.assertTrue(env.closed)
        self.assertFalse(env.http_calls)

    def test_native_acceptance_failures_never_send_mutations(self):
        plan = m.load_plan(self.path)
        for failure in ('native_identity', 'native_unconfirmed', 'native_controller',
                        'native_asset', 'native_no_trial', 'native_reboot', 'native_bad_json'):
            env = Environment(self); env.failure = failure; env.closed = True
            audit_path = self.folder / (failure + '.jsonl')
            with self.subTest(failure=failure):
                audit = m.Audit(audit_path, plan['target'].ip, clock=env.time.now)
                try:
                    with self.assertRaises((ValueError, TimeoutError)):
                        m.await_native_confirmation(plan, audit, get_http=env.http,
                            clock=env.time.now, sleep=env.time.sleep, output_fn=env.output.append)
                finally: audit.close()
                self.assertTrue(all(path == '/api/v1/device' or path in self.assets
                                    for _ip, path, _limit in env.http_calls))
                records = [json.loads(line) for line in audit_path.read_text().splitlines()]
                self.assertFalse(any(r['event'] == 'human_dashboard_confirmation_observed' for r in records))

    def test_slow_native_reads_cannot_extend_readiness_or_confirmation_deadlines(self):
        plan = m.load_plan(self.path)
        for phase in ('first_device', 'asset', 'confirmation'):
            env = Environment(self); env.closed = True
            def delayed(ip, path, maximum):
                data = env.http(ip, path, maximum)
                if ((phase == 'first_device' and env.native_reads == 1 and path == '/api/v1/device')
                    or (phase == 'asset' and path != '/api/v1/device')
                    or (phase == 'confirmation' and env.native_reads == 2 and path == '/api/v1/device')):
                    env.time.sleep(200)
                return data
            audit_path = self.folder / (phase + '.jsonl')
            audit = m.Audit(audit_path, plan['target'].ip, clock=env.time.now)
            try:
                with self.subTest(phase=phase), self.assertRaises((ValueError, TimeoutError)):
                    m.await_native_confirmation(plan, audit, get_http=delayed,
                        clock=env.time.now, sleep=env.time.sleep, output_fn=env.output.append)
            finally: audit.close()

    def test_read_only_startup_retry_and_confirmation_read_loss(self):
        plan = m.load_plan(self.path)
        for fail_at, expected_success in ((1, True), (2, False)):
            env = Environment(self); env.closed = True
            calls = 0
            def flaky(ip, path, maximum):
                nonlocal calls
                if path == '/api/v1/device':
                    calls += 1
                    if calls == fail_at: raise TimeoutError('Read lost')
                return env.http(ip, path, maximum)
            audit = m.Audit(self.folder / f'loss-{fail_at}.jsonl', plan['target'].ip, clock=env.time.now)
            try:
                if expected_success:
                    result = m.await_native_confirmation(plan, audit, get_http=flaky,
                        clock=env.time.now, sleep=env.time.sleep, output_fn=env.output.append)
                    self.assertFalse(result['trial_pending'])
                else:
                    with self.assertRaises(TimeoutError):
                        m.await_native_confirmation(plan, audit, get_http=flaky,
                            clock=env.time.now, sleep=env.time.sleep, output_fn=env.output.append)
            finally: audit.close()

    def test_caught_up_uptime_and_changed_reset_reason_reject_acceptance(self):
        plan = m.load_plan(self.path)
        for failure in ('caught_up', 'reason'):
            env = Environment(self); env.closed = True
            def getter(ip, path, maximum):
                raw = env.http(ip, path, maximum)
                if path != '/api/v1/device': return raw
                value = json.loads(raw)
                if env.native_reads == 2:
                    value['trial_pending'] = True
                    if failure == 'reason': value['reset_reason'] = 9
                if env.native_reads >= 3:
                    value['uptime_ms'] -= 5000
                    value['controller']['last_health_ms'] = value['uptime_ms']
                return json.dumps(value).encode()
            audit = m.Audit(self.folder / (failure + '.jsonl'), plan['target'].ip, clock=env.time.now)
            try:
                with self.subTest(failure=failure), self.assertRaisesRegex(ValueError, 'boot origin/reset cause'):
                    m.await_native_confirmation(plan, audit, get_http=getter,
                        clock=env.time.now, sleep=lambda _seconds: env.time.sleep(10), output_fn=env.output.append)
            finally: audit.close()

    def test_actual_http_get_is_bounded_read_only_and_closes(self):
        class Reply:
            status = 200
            encoding = 'identity'
            def __init__(self, chunks): self.chunks = list(chunks)
            def getheader(self, _name, default): return self.encoding or default
            def read1(self, maximum):
                self_maximum.append(maximum)
                return self.chunks.pop(0) if self.chunks else b''
        class Connection:
            def __init__(self): self.closed = False; self.sock = self; self.requests = []; self.timeouts = []
            def settimeout(self, timeout): self.timeouts.append(timeout)
            def request(self, *args, **kwargs): self.requests.append((args, kwargs))
            def getresponse(self): return reply
            def close(self): self.closed = True
        fixtures = [(b'{}', 'identity', True), (gzip.compress(b'{}', mtime=0), 'gzip', True),
                    (b'x' * 129, 'identity', False), (gzip.compress(b'x' * 129, mtime=0), 'gzip', False),
                    (gzip.compress(b'{}', mtime=0) + b'tail', 'gzip', False), (b'{}', 'br', False)]
        for raw, encoding, valid in fixtures:
            reply = Reply([raw]); reply.encoding = encoding
            connection = Connection(); self_maximum = []
            with patch.object(m.http.client, 'HTTPConnection', return_value=connection):
                if valid: self.assertEqual(m.http_get('192.168.86.249', '/api/v1/device', 128), b'{}')
                else:
                    with self.assertRaises(ValueError): m.http_get('192.168.86.249', '/api/v1/device', 128)
            self.assertTrue(connection.closed)
            self.assertEqual(connection.requests[0][0], ('GET', '/api/v1/device'))
            self.assertTrue(all(0 < value <= 5 for value in connection.timeouts))
            self.assertTrue(all(0 < value <= 129 for value in self_maximum))
        reply = Reply([]); reply.status = 302; connection = Connection()
        with patch.object(m.http.client, 'HTTPConnection', return_value=connection):
            with self.assertRaises(ValueError): m.http_get('192.168.86.249', '/api/v1/device', 128)
        self.assertTrue(connection.closed)
        with patch.object(m.http.client, 'HTTPConnection') as constructor:
            for path in ('/api/v1/update', '/api/v1/confirm', 'http://other', '/assets/../secret'):
                with self.assertRaises(ValueError): m.http_get('192.168.86.249', path, 128)
            constructor.assert_not_called()

    def test_audit_failure_before_connection_and_during_stage_stops(self):
        def forbidden(*_args, **_kwargs): self.fail('No connection after journal failure')
        args = ['install', '--manifest', str(self.path), '--audit', str(self.folder/'failed.jsonl'),
                '--execute', '--exclusive-control']
        opened = []
        original_open = Path.open
        def open_capture(path, *args, **kwargs):
            file = original_open(path, *args, **kwargs)
            if args and args[0] == 'x':
                opened.append(file)
                self.addCleanup(file.close)
            return file
        with patch.object(m.os, 'fsync', side_effect=OSError('disk full')), patch.object(Path, 'open', open_capture):
            self.assertEqual(m.run_cli(args, output_fn=lambda _x: None, session_factory=forbidden,
                                      input_fn=forbidden, get_http=forbidden), 1)
        self.assertEqual(len(opened), 1)
        self.assertTrue(opened[0].closed)
        env = Environment(self)
        original = env.audit
        def audit_factory(*args, **kwargs):
            audit = original(*args, **kwargs)
            record = audit.record
            def failing(event, **details):
                if event == 'stage_intent' and details.get('stage') == 'install_LOW1':
                    audit.failed = True
                    raise OSError('Disk full before LOW1')
                return record(event, **details)
            audit.record = failing
            return audit
        env.audit = audit_factory
        self.assertEqual(env.run(), 1)
        self.assertEqual([r[1] for r in env.session.commands if r[0] == 'install'], ['identity', 'OFF1'])
        self.assertTrue(env.closed)
        self.assertTrue(env.audit_ref.file.closed)

    def test_socket_close_failure_still_closes_audit(self):
        env = Environment(self); env.failure = 'identity'
        original = env.session_factory
        def factory(*args, **kwargs):
            session = original(*args, **kwargs)
            def fail_close(): raise OSError('Socket close failed')
            session.close = fail_close
            return session
        env.session_factory = factory
        with self.assertRaises(OSError): env.run()
        self.assertTrue(env.audit_ref.file.closed)

    def test_first_boot_pairing_instruction_matches_reported_window(self):
        plan = m.load_plan(self.path)
        for pairing_open in (False, True):
            env = Environment(self); env.closed = True
            def getter(ip, path, maximum):
                raw = env.http(ip, path, maximum)
                if path == '/api/v1/device':
                    value = json.loads(raw); value['pairing_open'] = pairing_open
                    return json.dumps(value).encode()
                return raw
            audit = m.Audit(self.folder / f'pairing-{pairing_open}.jsonl', plan['target'].ip, clock=env.time.now)
            try:
                result = m.await_native_confirmation(plan, audit, get_http=getter,
                    clock=env.time.now, sleep=env.time.sleep, output_fn=env.output.append)
            finally: audit.close()
            self.assertFalse(result['trial_pending'])
            text = '\n'.join(env.output)
            self.assertEqual('Hold the light\'s button for three seconds' in text, not pairing_open)
            self.assertEqual('while the pairing window is open' in text, pairing_open)


if __name__ == "__main__":
    unittest.main()
