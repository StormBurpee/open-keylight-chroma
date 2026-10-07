"""JSONL stream tests without any network or device access."""
import io
import json
from pathlib import Path
import queue
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import migration_events as events


class Input:
    def __init__(self): self.lines = queue.Queue()
    def readline(self, maximum):
        line = self.lines.get(timeout=5)
        return line[:maximum]


class Output(io.StringIO):
    def __init__(self):
        super().__init__(); self.rows = queue.Queue(); self.flushes = 0
    def write(self, text):
        result = super().write(text)
        self.rows.put(json.loads(text))
        return result
    def flush(self): self.flushes += 1


class Tests(unittest.TestCase):
    def setUp(self):
        self.block = patch("socket.socket", side_effect=AssertionError("No network"))
        self.block.start(); self.addCleanup(self.block.stop)
        self.input, self.output = Input(), Output()
        self.stream = events.JsonEvents(self.output, self.input)
        self.addCleanup(self.stream.close)
        self.addCleanup(lambda: self.input.lines.put(""))

    def prompt(self, answer):
        result = queue.Queue()
        def run():
            try: result.put(self.stream.prompt("off1_observation", "Was it dark?"))
            except BaseException as error: result.put(error)
        thread = threading.Thread(target=run); thread.start()
        row = self.output.rows.get(timeout=2)
        self.input.lines.put(answer(row))
        value = result.get(timeout=2)
        thread.join(timeout=2)
        self.assertFalse(thread.is_alive())
        return row, value

    def test_concurrent_output_has_exact_sequence_and_flush(self):
        def emit():
            for _ in range(100):
                self.stream.emit("progress", stage_id="identity", scope="program", completed=1, total=448, unit="blocks")
        threads = [threading.Thread(target=emit) for _ in range(4)]
        for thread in threads: thread.start()
        for thread in threads: thread.join()
        rows = [json.loads(line) for line in self.output.getvalue().splitlines()]
        self.assertEqual([row["seq"] for row in rows], list(range(400)))
        self.assertTrue(all(row["v"] == 1 for row in rows))
        self.assertEqual(self.output.flushes, 400)

    def test_output_failure_never_reuses_partial_envelope(self):
        with patch.object(self.output, "flush", side_effect=OSError("failed")):
            with self.assertRaises(OSError): self.stream.emit("status", code="ready")
        with self.assertRaises(OSError): self.stream.emit("status", code="ready")
        self.assertEqual(len(self.output.getvalue().splitlines()), 1)

    def test_short_output_marks_stream_failed(self):
        with patch.object(self.output, "write", return_value=1) as writer:
            with self.assertRaisesRegex(OSError, "Incomplete"):
                self.stream.emit("status", code="ready")
            with self.assertRaises(OSError): self.stream.emit("status", code="ready")
            self.assertEqual(writer.call_count, 1)

    def test_envelope_rejects_unknown_override_and_nonfinite(self):
        for event, fields in (("unknown", {}), ("status", {"v": 2}), ("status", {"seq": 9}),
                              ("progress", {"completed": float("nan")})):
            with self.assertRaises(ValueError): self.stream.emit(event, **fields)
        self.assertEqual(self.output.getvalue(), "")

    def test_exact_prompt_response_and_unique_ids(self):
        self.stream.start_input()
        first, value = self.prompt(lambda row: json.dumps({"v": 1, "id": row["id"], "answer": "yes"}) + "\n")
        self.assertEqual(value, "yes")
        second, value = self.prompt(lambda row: json.dumps({"v": 1, "id": row["id"], "answer": "no"}) + "\n")
        self.assertEqual(value, "no")
        self.assertNotEqual(first["id"], second["id"])
        self.assertEqual(first["choices"], ["yes", "no"])

    def test_wrong_prompt_duplicate_fields_eof_and_oversize_stop(self):
        for response in ('{"v":1,"id":"other","answer":"yes"}\n',
                         '{"v":1,"v":1,"id":"x","answer":"yes"}\n',
                         '{"v":true,"id":"x","answer":"yes"}\n',
                         'yes\n', '', 'x' * 1025, '{"v":1,"command":"cancel","token":"secret"}\n'):
            with self.subTest(response=response[:20]):
                self.stream = events.JsonEvents(self.output, self.input)
                self.stream.start_input()
                _row, value = self.prompt(lambda _row: response)
                self.assertIsInstance(value, events.EventInputError)
                self.assertNotIn('secret', str(value))
                self.stream.close()
                self.input, self.output = Input(), Output()

    def test_cancel_is_only_a_flag_until_executor_checks_safe_boundary(self):
        self.stream.start_input()
        self.input.lines.put('{"v":1,"command":"cancel"}\n')
        deadline = time.monotonic() + 2
        while not self.stream._cancelled and time.monotonic() < deadline: time.sleep(.001)
        self.assertTrue(self.stream._cancelled)
        # Emission and protected work continue; there is no asynchronous throw.
        self.stream.emit("progress", stage_id="identity", scope="quiet", completed=3, total=3, unit="seconds")
        with self.assertRaises(events.EventInputError): self.stream.check_cancelled()

    def test_cancel_unblocks_pending_prompt_without_automatic_yes(self):
        self.stream.start_input()
        _row, value = self.prompt(lambda _row: '{"v":1,"command":"cancel"}\n')
        self.assertIsInstance(value, events.EventInputError)

    def test_deferred_progress_is_bounded_and_preserves_boundary_order(self):
        release, blocked = threading.Event(), threading.Event()
        class Slow(Output):
            def write(self, text):
                blocked.set()
                if not release.wait(3): raise OSError('test reader stopped')
                return super().write(text)
        output = Slow()
        stream = events.JsonEvents(output)
        stream.defer_progress(stage_id='identity', scope='quiet', completed=0, total=3, unit='seconds')
        self.assertTrue(blocked.wait(1))
        for count in range(1000):
            stream.defer_progress(stage_id='identity', scope='controller_program', completed=count % 449,
                                  total=448, unit='blocks')
        self.assertLessEqual(stream._progress_pending, events.MAX_PENDING_PROGRESS)
        release.set()
        stream.emit('stage', id='identity', phase='completed')
        rows = [json.loads(line) for line in output.getvalue().splitlines()]
        self.assertEqual([row['seq'] for row in rows], list(range(len(rows))))
        self.assertEqual(rows[-1]['event'], 'stage')
        self.assertTrue(all(row['event'] == 'progress' for row in rows[:-1]))
        stream.close()

    def child(self, source, directory):
        root = Path(__file__).resolve().parents[2]
        prefix = "import sys; from pathlib import Path; sys.path[:0]=[sys.argv[1],sys.argv[2]]; "
        process = subprocess.Popen([sys.executable, '-c', prefix + source,
                                    str(root / 'tools'), str(root / 'tests/migration'), str(directory)],
                                   stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.addCleanup(lambda: process.kill() if process.poll() is None else None)
        return process

    def test_real_process_exits_with_parent_stdin_still_open(self):
        source = ("from migration_events import JsonEvents; "
                  "s=JsonEvents(sys.stdout,sys.stdin); s.start_input(); "
                  "s.emit('completed',outcome='prepared'); s.close()")
        with tempfile.TemporaryDirectory() as directory:
            process = self.child(source, directory)
            try:
                # Do not communicate()/close stdin: that would hide the real
                # TUI shutdown case by giving the input thread an early EOF.
                self.assertEqual(process.wait(timeout=8), 0)
                self.assertFalse(process.stdin.closed)
                rows = process.stdout.read().decode().splitlines()
                self.assertEqual(json.loads(rows[0])['outcome'], 'prepared')
                self.assertNotIn(b'Fatal Python error', process.stderr.read())
            finally:
                process.stdin.close(); process.stdout.close(); process.stderr.close()

    def test_real_first_boundary_event_times_out_with_full_open_pipe(self):
        for event in ('stage', 'status'):
            with self.subTest(event=event), tempfile.TemporaryDirectory() as directory:
                source = '''
import json, socket, time
from migration_events import JsonEvents
socket.socket=lambda *a,**k: (_ for _ in ()).throw(AssertionError('No network'))
s=JsonEvents(sys.stdout,sys.stdin); s.start_input()
started=time.monotonic(); stopped=False
try: s.emit(EVENT, message='x'*1048576)
except OSError: stopped=True
elapsed=time.monotonic()-started
s.close()
Path(sys.argv[3],'result.json').write_text(json.dumps({'stopped':stopped,'elapsed':elapsed}))
'''.replace('EVENT', repr(event))
                process = self.child(source, directory)
                try:
                    # No deferred progress exists to make flush_progress time
                    # out. The first boundary event itself fills stdout; keep
                    # both parent pipes open and unread until the child exits.
                    self.assertEqual(process.wait(timeout=8), 0)
                    proof = json.loads((Path(directory) / 'result.json').read_text())
                    self.assertTrue(proof['stopped'])
                    self.assertLess(proof['elapsed'], 5)
                    self.assertFalse(process.stdin.closed)
                    self.assertNotIn(b'Fatal Python error', process.stderr.read())
                finally:
                    process.stdin.close(); process.stdout.close(); process.stderr.close()

    def test_real_full_pipe_cannot_stall_flash_or_crash_shutdown(self):
        source = '''
import json, socket
from migration_events import JsonEvents
from stock_migration import transfer_controller
from test_migration import LoaderModel, bank
socket.socket=lambda *a,**k: (_ for _ in ()).throw(AssertionError('No network'))
s=JsonEvents(sys.stdout,sys.stdin); s.start_input()
s.defer_progress(stage_id='identity',scope='quiet',completed=0,total=3,unit='seconds',test_padding='x'*1048576)
session=LoaderModel(); session.events=s; session.stage_id='identity'
result=transfer_controller(session,bank(),deadline=200)
stopped=False
try: s.emit('stage',id='identity',phase='completed')
except OSError: stopped=True
s.close()
Path(sys.argv[3],'result.json').write_text(json.dumps({'blocks':result['verified_blocks'],'quiet':session.quiet_periods,
    'ends':sum(op==5 for _,op,_,_ in session.commands),'stopped_at_boundary':stopped}))
'''
        with tempfile.TemporaryDirectory() as directory:
            process = self.child(source, directory)
            try:
                # Intentionally never drain stdout until exit. The child must
                # finish actual modeled448+448/one End/quiet, then stop, with
                # both OS pipes still open in this parent.
                self.assertEqual(process.wait(timeout=10), 0)
                proof = json.loads((Path(directory) / 'result.json').read_text())
                self.assertEqual(proof, {'blocks': 448, 'quiet': [3], 'ends': 1, 'stopped_at_boundary': True})
                self.assertNotIn(b'Fatal Python error', process.stderr.read())
            finally:
                process.stdin.close(); process.stdout.close(); process.stderr.close()


if __name__ == "__main__": unittest.main()
