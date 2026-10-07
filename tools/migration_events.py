"""Version 1 JSONL presentation/input transport for the guided installer.

This module performs no network or device operations. Cancellation is a flag:
the executor must consult it only at its documented safe stage boundaries.
It must never use the reader thread to interrupt a flash or quiet interval.
"""
from __future__ import annotations

import json
import os
import queue
import secrets
import threading
import time
from typing import TextIO

EVENTS = frozenset(("stage", "progress", "prompt", "status", "action", "completed", "stopped"))
MAX_INPUT_BYTES = 1024
MAX_PENDING_PROGRESS = 64
OUTPUT_WAIT_SECONDS = 2


class EventInputError(ValueError):
    """Cancelled, ended, or invalid structured input; never echoes its contents."""


def _response(line: str) -> dict:
    if not line.endswith("\n") or len(line.encode("utf-8")) > MAX_INPUT_BYTES:
        raise EventInputError("Installer response must be one bounded JSON line")

    def pairs(items):
        result = {}
        for key, value in items:
            if key in result: raise EventInputError("Duplicate installer response field")
            result[key] = value
        return result

    def constant(_value):
        raise EventInputError("Non-finite installer response value")

    try:
        value = json.loads(line, object_pairs_hook=pairs, parse_constant=constant)
    except (ValueError, RecursionError) as error:
        raise EventInputError("Invalid installer response JSON") from error
    if not isinstance(value, dict) or type(value.get("v")) is not int or value["v"] != 1:
        raise EventInputError("Unsupported installer response version")
    if set(value) == {"v", "command"} and value["command"] == "cancel":
        return value
    if set(value) != {"v", "id", "answer"} or not isinstance(value["id"], str) \
            or not 1 <= len(value["id"]) <= 80 or value["answer"] not in ("yes", "no"):
        raise EventInputError("Invalid installer prompt response")
    return value


class JsonEvents:
    """Serialized stdout events and a bounded, asynchronous stdin reader.

    Only one optical prompt may be pending. Its unpredictable per-run ID binds
    the response to that prompt. No input contents, credentials or tokens are
    emitted. Input errors and EOF request a stop; they do not kill the executor.
    """
    def __init__(self, output: TextIO, input: TextIO | None = None):
        self.output, self.input = output, input
        self._lock = threading.RLock()
        self._condition = threading.Condition(self._lock)
        self._progress_condition = threading.Condition()
        self._progress_queue = queue.Queue()
        self._progress_pending = 0
        self._progress_writer = None
        self._output_fd = self._descriptor(output)
        self._input_fd = self._descriptor(input)
        self._sequence, self._prompt_number = 0, 0
        self._prefix = secrets.token_hex(8)
        self._pending = self._answer = self._input_error = None
        self._cancelled = self._closed = self._failed = False
        self._reader = None

    @staticmethod
    def _descriptor(stream):
        try:
            return stream.fileno()
        except (AttributeError, OSError):
            return None

    def emit(self, event: str, **fields) -> None:
        """Queue a boundary event and wait at most two seconds for its write.

        Only the daemon writer touches stdout: even the first event can meet
        a full pipe. Queue order makes this a barrier for prior progress too.
        Call only outside protected device work.
        """
        payload = self._payload(event, fields)
        deadline = time.monotonic() + OUTPUT_WAIT_SECONDS
        completed, errors = threading.Event(), []
        with self._progress_condition:
            while self._progress_pending >= MAX_PENDING_PROGRESS and not self._failed and not self._closed:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    self._failed = True
                    raise OSError("Installer event output is not responding")
                self._progress_condition.wait(remaining)
            if self._failed or self._closed:
                raise OSError("Installer event output is unavailable")
            self._queue_output(payload, completed, errors)
        if not completed.wait(max(0, deadline - time.monotonic())):
            with self._progress_condition:
                self._failed = True
                self._progress_condition.notify_all()
            raise OSError("Installer event output is not responding")
        if errors:
            raise errors[0]

    @staticmethod
    def _payload(event: str, fields: dict) -> str:
        if event not in EVENTS or {"v", "seq", "event"} & fields.keys():
            raise ValueError("Invalid installer event envelope")
        # Snapshot mutable caller fields before handing them to another thread.
        # Validation failures do not write or poison an otherwise valid stream.
        return json.dumps({"event": event, **fields}, ensure_ascii=False,
                          separators=(",", ":"), allow_nan=False)

    def _emit(self, payload: str) -> None:
        """Writer-thread only; raw writes may block without blocking executor."""
        if self._failed or self._closed:
            raise OSError("Installer event output is unavailable")
        encoded = '{"v":1,"seq":' + str(self._sequence) + ',' + payload[1:]
        if self._output_fd is not None:
            # A blocked daemon must never retain Python's buffered stdout
            # lock during interpreter shutdown. Pipes use raw UTF-8 writes;
            # StringIO/test streams retain their API.
            remaining = (encoded + "\n").encode("utf-8")
            while remaining:
                count = os.write(self._output_fd, remaining)
                if count <= 0:
                    raise OSError("Incomplete installer event output")
                remaining = remaining[count:]
        else:
            if self.output.write(encoded + "\n") != len(encoded) + 1:
                raise OSError("Incomplete installer event output")
            self.output.flush()
        self._sequence += 1

    def _queue_output(self, payload: str, completed=None, errors=None) -> None:
        """Enqueue under _progress_condition; this never writes to stdout."""
        self._progress_pending += 1
        self._progress_queue.put_nowait((payload, completed, errors))
        if self._progress_writer is None:
            self._progress_writer = threading.Thread(target=self._write_progress,
                                                     name="migration-events", daemon=True)
            self._progress_writer.start()

    def defer_progress(self, **fields) -> None:
        """Never wait for stdout during flash/SPI or required quiet intervals.

        At most 64 updates are pending. Intermediate presentation updates may
        be dropped under backpressure; stage completion still requires actual
        verification, and its bounded flush detects an unresponsive consumer.
        """
        with self._progress_condition:
            if self._failed or self._closed:
                return
            if self._progress_pending >= MAX_PENDING_PROGRESS:
                return
            try:
                payload = self._payload("progress", fields)
            except (ValueError, TypeError):
                self._failed = True
                return
            self._queue_output(payload)

    def _write_progress(self) -> None:
        while True:
            item = self._progress_queue.get()
            if item is None:
                return
            payload, completed, errors = item
            try:
                self._emit(payload)
            except BaseException as error:
                self._failed = True
                if errors is not None:
                    errors.append(error)
            finally:
                with self._progress_condition:
                    self._progress_pending -= 1
                    self._progress_condition.notify_all()
                if completed is not None:
                    completed.set()

    def flush_progress(self) -> None:
        """Call at a safe boundary; a stalled UI stops further device stages."""
        deadline = time.monotonic() + OUTPUT_WAIT_SECONDS
        with self._progress_condition:
            while self._progress_pending:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    self._failed = True
                    raise OSError("Installer progress output is not responding; protected stage completed")
                self._progress_condition.wait(remaining)

    def start_input(self) -> None:
        with self._lock:
            if self.input is None: raise ValueError("Structured input stream is required")
            if self._reader is not None: raise ValueError("Input reader already started")
            self._reader = threading.Thread(target=self._read, name="migration-input", daemon=True)
            self._reader.start()

    def _read(self) -> None:
        try:
            while True:
                if self._input_fd is None:
                    line = self.input.readline(MAX_INPUT_BYTES + 1)
                else:
                    # Raw fd reads avoid the fatal buffered-stdin lock at
                    # interpreter exit when the parent intentionally keeps its
                    # pipe open. The daemon never closes caller-owned stdin.
                    raw = bytearray()
                    while len(raw) <= MAX_INPUT_BYTES:
                        value = os.read(self._input_fd, 1)
                        if not value:
                            break
                        raw.extend(value)
                        if value == b"\n":
                            break
                    line = raw.decode("utf-8")
                with self._condition:
                    if self._closed: return
                    if not line: raise EventInputError("Installer input ended; stop after the current safe stage")
                    value = _response(line)
                    if value.get("command") == "cancel":
                        self._cancelled = True
                    elif value["id"] != self._pending or self._answer is not None:
                        raise EventInputError("Response does not match the pending installer prompt")
                    else:
                        self._answer = value["answer"]
                    self._condition.notify_all()
        except BaseException:
            with self._condition:
                # Do not expose an input line or exception text that could
                # contain unrelated private data accidentally sent to stdin.
                self._input_error = "Installer input ended or was invalid; stop after the current safe stage"
                self._condition.notify_all()

    def check_cancelled(self) -> None:
        """Call between complete stages, or at a read-only observer boundary."""
        with self._lock:
            if self._input_error: raise EventInputError(self._input_error)
            if self._cancelled: raise EventInputError("Installer cancelled after completing its current safe stage")

    def prompt(self, kind: str, message: str) -> str:
        if kind not in ("off1_observation", "low1_observation"):
            raise ValueError("Unknown installer optical prompt")
        with self._condition:
            self.check_cancelled()
            if self._reader is None or self._pending is not None:
                raise ValueError("One started input reader and one prompt are required")
            self._prompt_number += 1
            self._pending = f"{self._prefix}-{self._prompt_number}"
            prompt_id = self._pending
        try:
            self.emit("prompt", id=prompt_id, kind=kind, message=message, choices=["yes", "no"])
            with self._condition:
                while self._answer is None:
                    self.check_cancelled()
                    self._condition.wait()
                self.check_cancelled()
                return self._answer
        finally:
            with self._condition:
                self._pending = self._answer = None

    def close(self) -> None:
        """Stop presentation without closing caller-owned stdio or killing work."""
        with self._condition:
            if self._closed:
                return
            self._closed = True
            self._input_error = "Installer presentation closed"
            self._condition.notify_all()
        if self._progress_writer is not None:
            self._progress_queue.put_nowait(None)
