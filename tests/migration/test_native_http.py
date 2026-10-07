"""Native observer over real loopback HTTP framing; never contact a lamp."""
import gzip
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import http.client
import io
import json
from pathlib import Path
import sys
import tempfile
import threading
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
import stock_migration as m
from migration_events import JsonEvents
from test_cli import manifest_fixture
from test_migration import Clock, MemoryAudit


class Handler(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'
    def log_message(self, *_args): pass
    def do_GET(self):
        self.server.requests.append((self.command, self.path))
        body = gzip.compress(self.server.payload(self.path), mtime=0)
        self.send_response(200)
        self.send_header('Content-Encoding', 'gzip')
        self.send_header('Connection', 'close')
        self.send_header('Transfer-Encoding', 'chunked')
        self.end_headers()
        for start in range(0, len(body), 11):
            chunk = body[start:start + 11]
            self.wfile.write(f'{len(chunk):x}\r\n'.encode() + chunk + b'\r\n')
            self.wfile.flush()
        self.wfile.write(b'0\r\n\r\n'); self.wfile.flush()
        self.close_connection = True


class Tests(unittest.TestCase):
    def test_real_gzip_chunked_assets_then_closed_open_action_and_confirmation(self):
        with tempfile.TemporaryDirectory() as folder:
            manifest, _, assets = manifest_fixture(Path(folder))
            plan = m.load_plan(manifest)
            clock, audit, reads = Clock(), MemoryAudit(), []
            def payload(path):
                if path != '/api/v1/device': return assets[path]
                reads.append(clock.now())
                uptime = int(clock.now() * 1000)
                return json.dumps({'id': plan['device_id'], 'api_version': 1,
                    'firmware': plan['esp_metadata']['version'], 'firmware_elf_sha256': plan['esp_metadata']['elf_sha256'],
                    'uptime_ms': uptime, 'reset_reason': 3, 'trial_pending': len(reads) < 5,
                    'pairing_open': len(reads) >= 3, 'controller': {
                        'ready': True, 'connected': True, 'backend': 'original', 'status': 'ready',
                        'part_id': 0xbc40, 'trial_confirmed': True, 'version': '0.1.0.0',
                        'last_health_ms': uptime}}).encode()
            server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
            server.daemon_threads = True; server.payload = payload; server.requests = []
            thread = threading.Thread(target=server.serve_forever, daemon=True); thread.start()
            connection = http.client.HTTPConnection
            def local_only(ip, port=80, timeout=None):
                self.assertEqual((ip, port), (plan['target'].ip, 80))
                return connection('127.0.0.1', server.server_port, timeout=timeout)
            output = io.StringIO(); events = JsonEvents(output)
            try:
                with patch.object(m.http.client, 'HTTPConnection', side_effect=local_only):
                    result = m.await_native_confirmation(plan, audit, clock=clock.now, sleep=clock.sleep,
                        output_fn=lambda _: None, events=events)
            finally:
                events.close(); server.shutdown(); server.server_close(); thread.join(timeout=2)
            rows = [json.loads(line) for line in output.getvalue().splitlines()]
            actions = [row for row in rows if row['event'] == 'action']
            self.assertEqual([row['pairing_open'] for row in actions], [False, True])
            self.assertGreaterEqual(actions[1]['remaining_ms'], 30000)
            self.assertLess(actions[1]['remaining_ms'], actions[0]['remaining_ms'])
            self.assertFalse(result['trial_pending'])
            self.assertEqual([path for method, path in server.requests if path != '/api/v1/device'], list(assets))
            self.assertTrue(all(method == 'GET' for method, path in server.requests))
            self.assertIn('independent_client_confirmation_observed', [event for event, _ in audit.events])
            self.assertEqual([details['pairing_open'] for event, details in audit.events
                              if event == 'native_acceptance_action_intent'], [False, True])


if __name__ == '__main__': unittest.main()
