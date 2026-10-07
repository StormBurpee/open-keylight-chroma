"""Real loopback HTTP/socket regressions; never connect to a light or LAN host."""
import gzip
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import http.client
from pathlib import Path
import sys
import threading
import time
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import stock_migration as m

BODY = b'{"id":"synthetic-local-only","trial_pending":true}'


class Handler(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'
    def log_message(self, *_args): pass
    def do_GET(self):
        self.server.requests.append((self.command, self.path, dict(self.headers)))
        case = self.path.rsplit('/', 1)[-1]
        raw = gzip.compress(BODY, mtime=0) if 'gzip' in case else BODY
        if case == 'large': raw = b'x' * 129
        if case == 'bomb-gzip': raw = gzip.compress(b'x' * 129, mtime=0)
        if case == 'trailing-gzip': raw += b'garbage'
        self.send_response(302 if case == 'redirect' else 200)
        self.send_header('Connection', 'close')
        if 'gzip' in case: self.send_header('Content-Encoding', 'gzip')
        if 'chunked' in case:
            self.send_header('Transfer-Encoding', 'chunked')
        elif case != 'close-only':
            self.send_header('Content-Length', str(len(raw) + (7 if case == 'truncated' else 0)))
        self.end_headers()
        try:
            if 'chunked' in case:
                for start in range(0, len(raw), 3):
                    data = raw[start:start + 3]
                    # Deliberately separate framing and body socket writes.
                    self.wfile.write(f'{len(data):x}\r\n'.encode()); self.wfile.flush()
                    self.wfile.write(data + b'\r\n'); self.wfile.flush()
                self.wfile.write(b'0\r\nX-Test: finished\r\n\r\n'); self.wfile.flush()
            elif case == 'fragments':
                for start in range(0, len(raw), 2):
                    self.wfile.write(raw[start:start + 2]); self.wfile.flush(); time.sleep(.001)
            elif case == 'late-complete':
                self.wfile.write(raw[:2]); self.wfile.flush()
                time.sleep(.08)
                self.wfile.write(raw[2:]); self.wfile.flush()
            else:
                self.wfile.write(raw); self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
            pass
        self.close_connection = True


class Tests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        cls.server.daemon_threads = True
        cls.server.requests = []
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown(); cls.server.server_close(); cls.thread.join(timeout=2)

    def get(self, case, maximum=128, *, fast_deadline=False):
        connection_type = http.client.HTTPConnection
        def connect(host, port=80, timeout=None, **kwargs):
            self.assertEqual(host, '127.0.0.1')
            self.assertEqual(port, 80)
            return connection_type('127.0.0.1', self.server.server_port, timeout=timeout, **kwargs)
        real_clock = time.monotonic
        start = real_clock()
        clock = (lambda: start + (real_clock() - start) * 100) if fast_deadline else real_clock
        with patch.object(m.http.client, 'HTTPConnection', side_effect=connect), \
             patch.object(m.time, 'monotonic', side_effect=clock):
            return m.http_get('127.0.0.1', '/assets/' + case, maximum)

    def test_content_length_close_completes_without_touching_closed_socket(self):
        self.assertEqual(self.get('length'), BODY)

    def test_gzip_content_length_close(self):
        self.assertEqual(self.get('length-gzip'), BODY)

    def test_fragmented_content_length(self):
        self.assertEqual(self.get('fragments'), BODY)

    def test_close_delimited_and_chunked_bodies(self):
        for case in ('close-only', 'chunked', 'chunked-gzip'):
            with self.subTest(case=case): self.assertEqual(self.get(case), BODY)

    def test_truncated_known_length_is_rejected(self):
        with self.assertRaisesRegex(ValueError, 'Incomplete HTTP response'):
            self.get('truncated')

    def test_wire_and_decoded_bounds_and_trailing_gzip(self):
        for case in ('large', 'bomb-gzip', 'trailing-gzip', 'redirect'):
            with self.subTest(case=case), self.assertRaises(ValueError): self.get(case)

    def test_late_final_chunk_cannot_escape_absolute_deadline(self):
        with self.assertRaises(TimeoutError): self.get('late-complete', fast_deadline=True)

    def test_all_requests_are_get_to_loopback_with_connection_close(self):
        self.get('length')
        self.assertTrue(self.server.requests)
        self.assertTrue(all(method == 'GET' and path.startswith('/assets/') and headers['Connection'] == 'close'
                            for method, path, headers in self.server.requests))


if __name__ == '__main__': unittest.main()
