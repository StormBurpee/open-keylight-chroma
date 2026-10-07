"""Actual executor with synthetic devices and real JSONL I/O; sockets forbidden."""
import io
import json
from pathlib import Path
import queue
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
import stock_migration as m
from migration_events import JsonEvents
from test_cli import Environment, manifest_fixture
from test_migration import LoaderModel, bank


class Input:
    def __init__(self): self.lines = queue.Queue()
    def readline(self, maximum): return self.lines.get(timeout=5)[:maximum]


class Output(io.StringIO):
    def __init__(self, callback): super().__init__(); self.callback = callback
    def write(self, line):
        value = super().write(line)
        self.callback(json.loads(line))
        return value


class Tests(unittest.TestCase):
    def setUp(self):
        self.block = patch('socket.socket', side_effect=AssertionError('No sockets'))
        self.block.start(); self.addCleanup(self.block.stop)
        self.temp = tempfile.TemporaryDirectory(); self.addCleanup(self.temp.cleanup)
        self.folder = Path(self.temp.name)
        self.path, self.manifest, self.assets = manifest_fixture(self.folder)
        self.env = Environment(self)
        getter = self.env.http
        def event_http(ip, path, maximum):
            raw = getter(ip, path, maximum)
            if path == '/api/v1/device':
                value = json.loads(raw)
                value['trial_pending'] = self.env.native_reads < 3
                value['pairing_open'] = True
                return json.dumps(value).encode()
            return raw
        self.env.http = event_http
        self.input = Input(); self.addCleanup(lambda: self.input.lines.put(''))
        self.events = []
        self.answer = 'yes'
        def received(event):
            self.events.append(event)
            if event['event'] == 'prompt':
                self.assertEqual(self.env.session.firmware, 'resident')
                self.input.lines.put(json.dumps({'v': 1, 'id': event['id'], 'answer': self.answer}) + '\n')
        self.output = Output(received)

    def run_cli(self, command='install', digest=None):
        args = [command, '--manifest', str(self.path), '--events-jsonl']
        if command != 'prepare':
            args += ['--audit', str(self.folder / 'attempt.jsonl'), '--execute', '--exclusive-control']
            if digest != 'missing':
                args += ['--expected-manifest-sha256', digest or m.digest(self.path.read_bytes())]
        with patch.object(m, 'inspect_nxp_loader', return_value={'reference_code_matches': True}), \
             patch.object(m, 'transfer_controller', side_effect=self.env.transfer), \
             patch.object(m, 'transfer_esp', side_effect=self.env.transfer_esp):
            return m.run_cli(args, input_fn=lambda _x: self.fail('No plaintext prompts'),
                output_fn=lambda _x: self.fail('No plaintext stdout'), session_factory=self.env.session_factory,
                audit_factory=self.env.audit, get_http=self.env.http, clock=self.env.time.now,
                sleep=self.env.time.sleep, event_input=self.input, event_output=self.output)

    def test_prepare_is_structured_offline_and_execution_is_manifest_bound(self):
        self.assertEqual(self.run_cli('prepare'), 0)
        self.assertEqual([row['event'] for row in self.events], ['status', 'completed'])
        self.assertEqual(self.events[0]['summary']['manifest_sha256'], m.digest(self.path.read_bytes()))
        self.assertEqual(self.events[0]['summary']['controller_version'], '0.1.0.0')
        self.assertEqual(self.events[1]['outcome'], 'prepared')
        self.assertIsNone(self.env.session)
        for digest in ('missing', '0' * 64):
            self.assertEqual(self.run_cli(digest=digest), 1)
            self.assertIsNone(self.env.session)
            self.assertFalse((self.folder / 'attempt.jsonl').exists())

    def test_real_stage_flow_events_prompts_native_action_and_terminal(self):
        self.assertEqual(self.run_cli(), 0)
        self.assertEqual([row['seq'] for row in self.events], list(range(len(self.events))))
        stages = [row for row in self.events if row['event'] == 'stage']
        expected = ['stock', 'identity', 'off1', 'low1', 'lighting', 'esp', 'native']
        self.assertEqual([(row['id'], row['phase']) for row in stages],
                         [(name, phase) for name in expected for phase in ('started', 'completed')])
        for name in ('off1', 'low1'):
            prompt = next(row for row in self.events if row['event'] == 'prompt' and row['kind'] == name + '_observation')
            finished = next(row for row in stages if row['id'] == name and row['phase'] == 'completed')
            self.assertLess(prompt['seq'], finished['seq'])
        action = next(row for row in self.events if row['event'] == 'action')
        self.assertEqual(action['manifest_sha256'], m.digest(self.path.read_bytes()))
        self.assertEqual(action['controller_version'], '0.1.0.0')
        self.assertEqual(action['device_id'], self.manifest['target']['device_id'])
        self.assertGreater(action['remaining_ms'], 0)
        self.assertEqual(self.events[-1]['outcome'], 'installed')
        self.assertTrue(self.env.audit_ref.file.closed)
        # Reproducible synthetic stream for the actual Ink reducer integration.
        if hasattr(self, 'sample_path'):
            self.sample_path.write_text(self.output.getvalue(), encoding='utf-8')

    def test_optical_no_does_not_advance_or_complete_stage(self):
        self.answer = 'no'
        self.assertEqual(self.run_cli(), 1)
        self.assertEqual([row[1] for row in self.env.session.commands if row[0] == 'install'], ['identity', 'OFF1'])
        self.assertEqual(self.events[-1]['event'], 'stopped')
        self.assertFalse(self.events[-1]['automatic_restore'])
        self.assertFalse(any(row['event'] == 'stage' and row['id'] == 'off1' and row['phase'] == 'completed'
                             for row in self.events))

    def test_cancel_waits_for_identity_return_before_stopping(self):
        original = self.env.transfer
        def cancel(session, raw, deadline):
            result = original(session, raw, deadline)
            self.input.lines.put('{"v":1,"command":"cancel"}\n')
            until = time.monotonic() + 2
            while not session.events._cancelled and time.monotonic() < until: time.sleep(.001)
            self.assertTrue(session.events._cancelled)
            return result
        self.env.transfer = cancel
        self.assertEqual(self.run_cli(), 1)
        commands = self.env.session.commands
        self.assertIn(('quiet', 33), commands)
        self.assertEqual(commands[-1][:2], (16, 0x80))
        self.assertEqual([row[1] for row in commands if row[0] == 'install'], ['identity'])
        self.assertFalse(any(row['event'] == 'prompt' for row in self.events))

    def test_native_cancel_warns_pending_trial_without_writes(self):
        original = self.output.callback
        def received(event):
            original(event)
            if event['event'] == 'action':
                self.input.lines.put('{"v":1,"command":"cancel"}\n')
        getter = self.env.http
        def after_cancel(ip, path, maximum):
            if self.env.native_reads >= 2 and path == '/api/v1/device':
                until = time.monotonic() + 2
                while not self.env.session.events._cancelled and time.monotonic() < until: time.sleep(.001)
            return getter(ip, path, maximum)
        self.output.callback = received
        self.env.http = after_cancel
        self.assertEqual(self.run_cli(), 1)
        self.assertTrue(self.events[-1]['native_trial_may_be_pending'])
        self.assertLessEqual(self.env.native_reads, 3)
        self.assertEqual(self.env.session.commands.count(('ESP',)), 1)

    def test_broken_progress_output_never_interrupts_real_commit_quiet(self):
        session = LoaderModel()
        class Broken(io.StringIO):
            def write(self, _line): raise BrokenPipeError('UI closed')
        session.events, session.stage_id = JsonEvents(Broken()), 'identity'
        result = m.transfer_controller(session, bank(), deadline=200)
        self.assertEqual(result['verified_blocks'], 448)
        self.assertEqual(session.quiet_periods, [3])
        self.assertEqual([op for _cls, op, _data, _mutation in session.commands].count(5), 1)
        self.assertNotIn(4, [op for _cls, op, _data, _mutation in session.commands])
        with self.assertRaises(OSError): session.events.emit('stage', id='identity', phase='completed')

    def test_native_pairing_closed_then_open_emits_one_fresh_bound_action(self):
        self.env.closed = True
        plan = m.load_plan(self.path)
        getter = self.env.http
        def opening(ip, path, maximum):
            raw = getter(ip, path, maximum)
            if path == '/api/v1/device':
                value = json.loads(raw)
                value['pairing_open'] = self.env.native_reads >= 3
                value['trial_pending'] = self.env.native_reads < 5
                return json.dumps(value).encode()
            return raw
        audit = m.Audit(self.folder / 'pairing.jsonl', plan['target'].ip, clock=self.env.time.now)
        try:
            m.await_native_confirmation(plan, audit, get_http=opening, clock=self.env.time.now,
                                        sleep=self.env.time.sleep, output_fn=lambda _: None,
                                        events=JsonEvents(self.output))
        finally:
            audit.close()
        actions = [event for event in self.events if event['event'] == 'action']
        self.assertEqual([action['pairing_open'] for action in actions], [False, True])
        self.assertEqual(actions[0]['manifest_sha256'], plan['manifest_sha256'])
        self.assertEqual(actions[0]['elf_sha256'], plan['esp_metadata']['elf_sha256'])
        self.assertLess(actions[0]['remaining_ms'], 175000)
        waiting = [event for event in self.events if event.get('code') == 'pairing_required']
        self.assertEqual(len(waiting), 1)
        self.assertLess(actions[0]['seq'], waiting[0]['seq'])
        self.assertLess(waiting[0]['seq'], actions[1]['seq'])
        records = [json.loads(line) for line in (self.folder / 'pairing.jsonl').read_text().splitlines()]
        self.assertIn('independent_client_confirmation_observed', [row['event'] for row in records])
        self.assertNotIn('human_dashboard_confirmation_observed', [row['event'] for row in records])

    def test_native_pairing_wait_cannot_hide_identity_reset_or_controller_fault(self):
        plan = m.load_plan(self.path)
        for fault in ('elf', 'reset', 'restart', 'readiness', 'stale_health', 'deadline'):
            env = Environment(self)
            env.closed = True
            output = io.StringIO()
            getter = env.http
            def invalid(ip, path, maximum):
                raw = getter(ip, path, maximum)
                if path == '/api/v1/device':
                    value = json.loads(raw)
                    value['pairing_open'] = env.native_reads >= 3
                    value['trial_pending'] = True
                    if env.native_reads == 3:
                        if fault == 'elf': value['firmware_elf_sha256'] = 'f' * 64
                        elif fault == 'reset': value['reset_reason'] = 9
                        elif fault == 'restart': value['uptime_ms'] = 0
                        elif fault == 'readiness': value['controller']['ready'] = False
                        elif fault == 'stale_health': value['controller']['last_health_ms'] = -10000
                        elif fault == 'deadline': env.time.sleep(175)
                    return json.dumps(value).encode()
                return raw
            with self.subTest(fault=fault):
                audit = m.Audit(self.folder / f'pairing-{fault}.jsonl', plan['target'].ip, clock=env.time.now)
                try:
                    with self.assertRaises((ValueError, TimeoutError)):
                        m.await_native_confirmation(plan, audit, get_http=invalid, clock=env.time.now,
                            sleep=env.time.sleep, output_fn=lambda _: None, events=JsonEvents(output))
                finally:
                    audit.close()
                rows = [json.loads(line) for line in output.getvalue().splitlines()]
                self.assertFalse(any(row['event'] == 'action' and row['pairing_open'] for row in rows))

    def test_closed_pairing_times_out_without_action_or_mutation(self):
        self.env.closed = True
        plan = m.load_plan(self.path)
        getter = self.env.http
        def closed(ip, path, maximum):
            raw = getter(ip, path, maximum)
            if path == '/api/v1/device':
                value = json.loads(raw)
                value['pairing_open'] = False
                value['trial_pending'] = True
                return json.dumps(value).encode()
            return raw
        audit = m.Audit(self.folder / 'closed.jsonl', plan['target'].ip, clock=self.env.time.now)
        try:
            with self.assertRaises(m.NativePairingDeadlineError):
                m.await_native_confirmation(plan, audit, get_http=closed, clock=self.env.time.now,
                    sleep=self.env.time.sleep, output_fn=lambda _: None, events=JsonEvents(self.output))
        finally:
            audit.close()
        self.assertGreaterEqual(self.env.time.value, 145)
        self.assertLess(self.env.time.value, 147)
        actions = [row for row in self.events if row['event'] == 'action']
        self.assertEqual([row['pairing_open'] for row in actions], [False])
        self.assertEqual(sum(row.get('code') == 'pairing_required' for row in self.events), 1)
        self.assertTrue(all(path == '/api/v1/device' or path in self.assets for _, path, _ in self.env.http_calls))

    def test_late_open_or_slow_action_audit_never_starts_native_client(self):
        plan = m.load_plan(self.path)
        for slow_audit in (False, True):
            env = Environment(self); env.closed = True
            output = io.StringIO(); getter = env.http
            def opening(ip, path, maximum):
                raw = getter(ip, path, maximum)
                if path == '/api/v1/device':
                    value = json.loads(raw)
                    value['pairing_open'] = env.native_reads >= 3
                    value['trial_pending'] = True
                    return json.dumps(value).encode()
                return raw
            audit = m.Audit(self.folder / f'late-open-{slow_audit}.jsonl', plan['target'].ip, clock=env.time.now)
            record = audit.record
            def save(event, **details):
                record(event, **details)
                if slow_audit and event == 'native_acceptance_action_intent' and details['pairing_open']:
                    env.time.sleep(2)
            audit.record = save
            def sleep(_seconds): env.time.value = 144 if slow_audit else 146
            try:
                with self.subTest(slow_audit=slow_audit), self.assertRaises(m.NativePairingDeadlineError) as error:
                    m.await_native_confirmation(plan, audit, get_http=opening, clock=env.time.now,
                        sleep=sleep, output_fn=lambda _: None, events=JsonEvents(output))
                self.assertEqual(error.exception.code, 'native_pairing_deadline')
            finally:
                audit.close()
            rows = [json.loads(line) for line in output.getvalue().splitlines()]
            self.assertEqual([row['pairing_open'] for row in rows if row['event'] == 'action'], [False])


if __name__ == '__main__': unittest.main()
