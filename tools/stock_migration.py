"""Stock-migration inspection and bounded, explicitly invoked transport.

An artifact match is not a live identity, fresh-loader proof, authorization or
electrical qualification. Inspection/prepare are offline. Guided install and
restore require explicit execution, reviewed local artifacts and a new audit;
they never scan, resume an old audit, restore automatically or confirm an ESP.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import struct
import ipaddress
import os
import socket
import threading
import time
import select
import sys
import re
import zlib
import http.client
import secrets
from contextlib import contextmanager
from dataclasses import dataclass

ESP_SLOT_BYTES = 0x180000
ESP_CHUNK_BYTES = 1013
NXP_LOADER_BYTES = 8192
NXP_BANK_BYTES = 28672
NXP_FLAG_OFFSET = 0x1F00
NXP_NORMALIZED_LOADER_SHA = "b0fcbf098d6355f45d3592868d71ecb37c14b51c6d585829c84c3ea16cccf482"
NXP_INFORMATION = bytes.fromhex("03180102000000025d")
HELLO = bytes.fromhex("aa000013020912022026010900110000401500")
REFERENCE_PARTITIONS = (
    ("nvs", 1, 2, 0x9000, 0x4000),
    ("otadata", 1, 0, 0xD000, 0x2000),
    ("phy_init", 1, 1, 0xF000, 0x1000),
    ("ota_0", 0, 0x10, 0x10000, ESP_SLOT_BYTES),
    ("ota_1", 0, 0x11, 0x190000, ESP_SLOT_BYTES),
)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def inspect_esp_application(data: bytes) -> dict:
    """Check a complete, unsigned ESP32 app image, not a merged flash image.

    The simple appended SHA is integrity only. This intentionally accepts a
    narrow reference profile; it does not reproduce the ROM image verifier.
    """
    require(isinstance(data, bytes) and 32 <= len(data) <= ESP_SLOT_BYTES,
            "Application must fit one reference 1.5 MiB slot")
    require(data[0] == 0xE9 and 1 <= data[1] <= 16, "Invalid ESP image header")
    require(data[2] == 2 and data[3] in (0x20, 0x2F), "Require DIO/4 MiB and 40 or 80 MHz")
    require(struct.unpack_from("<H", data, 12)[0] == 0, "Require ESP32 chip ID")
    require(data[23] == 1, "Require an appended image SHA-256")
    require(data[19:23] == bytes(4), "Unknown extended-header features")
    offset = 24
    checksum = 0xEF
    segments = []
    first = b""
    for index in range(data[1]):
        require(offset + 8 <= len(data), "Truncated segment header")
        address, length = struct.unpack_from("<II", data, offset)
        offset += 8
        require(length % 4 == 0 and length <= len(data) - offset, "Invalid segment length")
        require(address + length <= 0x100000000, "Segment address wraps")
        regions = ((0x3F400000, 0x3F800000), (0x3FFAE000, 0x40000000),
                   (0x40080000, 0x400A0000), (0x400C0000, 0x400C2000),
                   (0x400D0000, 0x40400000), (0x50000000, 0x50002000))
        require(length > 0 and any(low <= address < address + length <= high for low, high in regions),
                "Segment is outside the reviewed ESP32 memory regions")
        require(all(address + length <= previous["address"] or
                    address >= previous["address"] + previous["bytes"] for previous in segments),
                "Overlapping ESP image segments")
        if 0x3F400000 <= address < 0x3F800000 or 0x400D0000 <= address < 0x40400000:
            require(address % 65536 == offset % 65536, "Flash-mapped segment alignment differs")
        content = data[offset:offset + length]
        if index == 0:
            first = content
        for value in content:
            checksum ^= value
        segments.append({"address": address, "bytes": length})
        offset += length
    entry = struct.unpack_from("<I", data, 4)[0]
    require(any(segment["address"] <= entry < segment["address"] + segment["bytes"]
                and 0x40080000 <= segment["address"] < 0x40400000 for segment in segments),
            "ESP entry point is outside its executable segments")
    checksum_at = offset | 15
    require(checksum_at + 33 == len(data), "Trailing, signed or truncated application data")
    require(not any(data[offset:checksum_at]), "Nonzero image padding")
    require(data[checksum_at] == checksum, "Segment XOR checksum differs")
    require(hashlib.sha256(data[:checksum_at + 1]).digest() == data[-32:], "Image SHA-256 differs")
    require(len(first) >= 176 and struct.unpack_from("<I", first)[0] == 0xABCD5432,
            "Application descriptor absent; do not upload a bootloader")

    def text_at(start: int) -> str:
        field = first[start:start + 32]
        require(b"\0" in field, "Unterminated app descriptor string")
        try:
            return field.split(b"\0", 1)[0].decode("ascii")
        except UnicodeDecodeError as error:
            raise ValueError("Non-ASCII app descriptor string") from error

    project = text_at(48)
    require(project == "open_keylight", "Require an original Open Keylight application")
    return {"bytes": len(data), "sha256": digest(data), "project": project,
            "version": text_at(16), "idf_version": text_at(112),
            "elf_sha256": first[144:176].hex(), "segments": segments,
            "min_revision": struct.unpack_from("<H", data, 15)[0],
            "max_revision": struct.unpack_from("<H", data, 17)[0],
            "integrity_verified": True, "authenticity_verified": False,
            "target_compatibility_verified": False}


def inspect_partition_table(data: bytes) -> dict:
    """Require the complete reference partition sector, checksum and erased tail."""
    require(len(data) == 4096, "Partition capture must be exactly 4096 bytes")
    entries = []
    offset = 0
    while offset + 32 <= len(data) and data[offset:offset + 2] == b"\xaa\x50":
        magic, kind, subtype, start, size, label, flags = struct.unpack_from("<HBBII16sI", data, offset)
        del magic
        require(b"\0" in label and flags == 0, "Unsupported partition label/flags")
        try:
            name = label.split(b"\0", 1)[0].decode("ascii")
        except UnicodeDecodeError as error:
            raise ValueError("Non-ASCII partition label") from error
        entries.append((name, kind, subtype, start, size))
        offset += 32
    require(tuple(entries) == REFERENCE_PARTITIONS, "Partition layout is not the reference layout")
    require(data[offset:offset + 16] == b"\xeb\xeb" + b"\xff" * 14, "Partition MD5 record absent")
    require(hashlib.md5(data[:offset]).digest() == data[offset + 16:offset + 32], "Partition MD5 differs")
    require(all(value == 0xFF for value in data[offset + 32:]), "Unexpected partition-table tail")
    return {"reference_layout_matches": True, "sha256": digest(data),
            "entries": [{"label": n, "type": k, "subtype": s, "offset": a, "bytes": z}
                        for n, k, s, a, z in entries]}


def inspect_nxp_loader(data: bytes) -> dict:
    """Match resident bytes except its three reviewed mutable boot flags.

    Read83 can flush a previously buffered sector. The caller must establish a
    reset boundary and no programming before capturing; these bytes cannot
    establish that history. Flag normalization also means their raw values
    are observations, not independently authenticated boot policy.
    """
    require(len(data) == NXP_LOADER_BYTES, "Resident capture must be exactly 8192 bytes")
    flags = list(data[NXP_FLAG_OFFSET:NXP_FLAG_OFFSET + 3])
    require(all(value in (0, 1) for value in flags), "Unknown resident flag encoding")
    normalized = bytearray(data)
    normalized[NXP_FLAG_OFFSET:NXP_FLAG_OFFSET + 3] = bytes(3)
    require(digest(normalized) == NXP_NORMALIZED_LOADER_SHA, "Unknown resident loader code/data")
    require(sum(struct.unpack_from("<8I", data)) & 0xFFFFFFFF == 0, "ROM vector checksum differs")
    return {"reference_code_matches": True, "sha256": digest(data),
            "normalized_sha256": digest(normalized), "raw_flags": flags,
            "actual_part_id": None, "fresh_loader_proven": False,
            "safe_to_program": False}


def inspect_staging_bank(data: bytes) -> dict:
    require(len(data) == NXP_BANK_BYTES, "Staging capture must include the complete 28 KiB bank")
    return {"bytes": len(data), "sha256": digest(data), "capture_kind": "staging_snapshot",
            "active_application_backup": False, "restore_qualified": False}


def esp_transfer_frames(image: bytes):
    """Yield protocol bytes offline; validating the image precedes every yield."""
    inspect_esp_application(image)
    yield b"\xaa\x00\x00\x0a\x01\x00" + struct.pack(">I", len(image))
    for offset in range(0, len(image), ESP_CHUNK_BYTES):
        chunk = image[offset:offset + ESP_CHUNK_BYTES]
        yield b"\xaa\x00" + struct.pack(">H", len(chunk) + 6) + b"\x01\x01" + chunk
    yield b"\xaa\x00\x00\x06\x01\x02"


class ResponseFrames:
    """Bounded incremental framing; TCP read boundaries have no significance."""
    def __init__(self) -> None:
        self.pending = bytearray()
        self.count = 0
        self.total = 0

    def feed(self, data: bytes) -> list[bytes]:
        require(isinstance(data, bytes), "Response chunk must be bytes")
        self.total += len(data)
        require(self.total <= 65536, "Excessive session response")
        self.pending.extend(data)
        frames = []
        while len(self.pending) >= 4:
            length = int.from_bytes(self.pending[2:4], "big")
            require(self.pending[0] == 0xAA and 5 <= length <= 4096, "Malformed response frame")
            if len(self.pending) < length:
                break
            self.count += 1
            require(self.count <= 64, "Excessive session frames")
            frames.append(bytes(self.pending[:length]))
            del self.pending[:length]
        return frames


class HelloSession:
    """Pure receive-state model for one absolute five-second HELLO deadline.

    Events must be supplied with a monotonic timestamp; this class performs no
    clock or network I/O. Unrelated notifications never shorten or extend the
    deadline. A partial tail after a matching echo is not acceptance.
    """
    def __init__(self, started_ms: int) -> None:
        require(type(started_ms) is int and started_ms >= 0, "Invalid start time")
        self.deadline = started_ms + 5000
        self.now = started_ms
        self.frames = ResponseFrames()
        self.echo = False
        self.failed = False

    def feed(self, now_ms: int, data: bytes) -> bool:
        try:
            require(not self.failed, "Session already failed")
            require(type(now_ms) is int and self.now <= now_ms < self.deadline,
                    "HELLO deadline expired or clock reversed")
            self.now = now_ms
            for packet in self.frames.feed(data):
                require(packet[1] == 2, "Session response rejected")
                if packet[4] == 2:
                    require(len(packet) >= len(HELLO) and packet[5:len(HELLO)] == HELLO[5:],
                            "Unexpected HELLO echo")
                    self.echo = True
            return self.echo and not self.frames.pending
        except ValueError:
            self.failed = True
            raise


class MigrationError(RuntimeError):
    """A stage stopped. Neither this exception nor an audit authorizes retry."""


class RemoteError(MigrationError):
    """A complete, correlated negative reply; framing remains synchronized."""


class StockEntryError(MigrationError):
    """Entry is unresolved before any controller image operation."""
    code = "stock_loader_entry_unconfirmed"


class NativePairingDeadlineError(MigrationError):
    """The separate acceptance client cannot safely start within this trial."""
    code = "native_pairing_deadline"


class Audit:
    """Exclusive, append-only JSONL journal; flush intent before any mutation.

    Hashes describe integrity, not authenticity. Firmware payloads, Wi-Fi
    passwords and bearer tokens are never written to this journal.
    """
    def __init__(self, path: Path, target: str, *, clock=time.monotonic):
        self.clock, self.lock, self.sequence, self.previous = clock, threading.Lock(), 0, "0" * 64
        self.file = path.open("x", encoding="utf-8", newline="\n")
        self.failed = False
        try:
            self.record("created", target=target, schema=1, automatic_resume=False)
        except BaseException:
            self.file.close()
            raise

    def record(self, event: str, **details) -> None:
        with self.lock:
            if self.failed:
                raise MigrationError("Audit is no longer writable; no further mutation permitted")
            row = {"sequence": self.sequence, "monotonic_s": self.clock(),
                   "event": event, "previous_sha256": self.previous, **details}
            encoded = json.dumps(row, sort_keys=True, separators=(",", ":"), allow_nan=False)
            try:
                self.file.write(encoded + "\n")
                self.file.flush()
                os.fsync(self.file.fileno())
            except BaseException:
                self.failed = True
                raise
            self.previous = digest(encoded.encode())
            self.sequence += 1

    def close(self) -> None:
        self.file.close()


@dataclass(frozen=True)
class Target:
    ip: str
    name: str
    esp_version: bytes = bytes([1, 0, 13, 0])

    def __post_init__(self):
        address = ipaddress.IPv4Address(self.ip)
        require(address.is_private and not address.is_loopback and not address.is_multicast
                and not address.is_unspecified and not address.is_reserved,
                "Select one explicit private IPv4 light; discovery is not performed")
        require(isinstance(self.name, str) and 1 <= len(self.name.encode("utf-8")) <= 63,
                "Expected light name is required")
        require(type(self.esp_version) is bytes and len(self.esp_version) == 4,
                "Expected ESP version must be four exact bytes")


def report_request(cls: int, opcode: int, payload: bytes = b"") -> bytes:
    require(type(cls) is int and 0 <= cls <= 255 and type(opcode) is int and 0 <= opcode <= 255,
            "Invalid report command")
    require(type(payload) is bytes and len(payload) <= 80, "Invalid report payload")
    report = bytearray(90)
    report[5:8] = bytes([len(payload), cls, opcode])
    report[8:8 + len(payload)] = payload
    for byte in report[2:88]:
        report[88] ^= byte
    return b"\xaa\0\0\x5f\0" + report


def decode_report(frame: bytes, cls: int, opcode: int) -> bytes:
    require(len(frame) == 95 and frame[:5] == b"\xaa\x02\0\x5f\0", "Invalid report envelope")
    report = frame[5:]
    require(report[1:5] == bytes(4) and report[6:8] == bytes([cls, opcode])
            and report[5] <= 80 and not report[89], "Uncorrelated report metadata")
    checksum = 0
    for byte in report[2:88]:
        checksum ^= byte
    require(checksum == report[88], "Invalid report checksum")
    if report[0] != 2:
        raise RemoteError(f"Controller rejected class {cls:02x}/opcode {opcode:02x}: status {report[0]}")
    return report[8:8 + report[5]]


class StockSession:
    """One TCP connection, one request at a time, no reconnect or mutation retry.

    A timeout, malformed response or send error poisons the session. A complete
    correlated NACK is distinguishable and may allow one explicit precommit
    Abort. A new connection itself generates controller traffic, so close is
    forbidden until the recorded commit quiet interval has elapsed.
    """
    def __init__(self, target: Target, audit: Audit, *, connector=socket.create_connection,
                 clock=time.monotonic, sleep=time.sleep):
        self.target, self.audit = target, audit
        self.connector, self.clock, self.sleep = connector, clock, sleep
        self.sock = None
        self.poisoned, self.quiet_until, self.connected_once = False, 0.0, False
        self.routing_identity = None

    def _remaining(self, deadline):
        value = deadline - self.clock()
        if value <= 0:
            raise TimeoutError("Absolute operation deadline expired")
        return value

    def send(self, packet: bytes, label: str, deadline: float, *, mutation=False) -> None:
        require(self.sock is not None and not self.poisoned, "Session is not synchronized")
        require(self.clock() >= self.quiet_until, "Controller commit quiet interval is active")
        # Failure to persist intent occurs before any send, including sendall
        # that might raise after delivering some or all of the command.
        self.audit.record("send_intent", label=label, mutation=mutation,
                          bytes=len(packet), sha256=digest(packet), delivery="possibly_sent_after_this_record")
        try:
            self.sock.settimeout(self._remaining(deadline))
            self.sock.sendall(packet)
        except BaseException:
            self.poisoned = True
            raise

    def receive(self, predicate, deadline: float) -> bytes:
        decoder, match = ResponseFrames(), None
        try:
            while True:
                self.sock.settimeout(self._remaining(deadline))
                chunk = self.sock.recv(4096)
                self._remaining(deadline)
                if not chunk:
                    raise ConnectionError("Peer closed before a complete expected reply")
                for frame in decoder.feed(chunk):
                    require(frame[1] == 2, "Transport rejected the request")
                    self.audit.record("received", bytes=len(frame), command=frame[4], sha256=digest(frame))
                    if predicate(frame):
                        require(match is None, "Duplicate correlated reply")
                        match = frame
                if match is not None:
                    require(not decoder.pending, "Partial trailing frame makes the result ambiguous")
                    self._remaining(deadline)
                    return match
        except BaseException:
            self.poisoned = True
            raise

    def connect(self, *, require_owner=True):
        require(not self.connected_once, "Sessions never reconnect automatically")
        self.connected_once = True
        self.audit.record("connect_intent", ip=self.target.ip, port=10003)
        self.sock = self.connector((self.target.ip, 10003), timeout=5)
        deadline = self.clock() + 5
        self.send(HELLO, "hello", deadline)
        def hello(frame):
            if len(frame) == 95 and frame[4] == 0 and frame[11:13] == b"\0\x49":
                owner = decode_report(frame, 0, 0x49)
                require(len(owner) == 80 and owner[0] == 1 and any(owner[1:7])
                        and not owner[1] & 1 and owner[7:] == bytes(73), "Unexpected connection ownership notification")
                require(self.routing_identity is None, "Duplicate connection ownership notification")
                self.routing_identity = owner[1:7]
            return frame[4] == 2
        try:
            reply = self.receive(hello, deadline)
            require(len(reply) >= len(HELLO) and reply[5:len(HELLO)] == HELLO[5:], "HELLO echo differs")
            require(not require_owner or self.routing_identity is not None, "Connection routing identity was not observed")
        except BaseException:
            self.poisoned = True
            raise

    def network_get(self, opcode: int) -> bytes:
        require(opcode in (0x84, 0x88), "Only name and ESP-version getters are exposed")
        deadline = self.clock() + 5
        self.send(bytes([0xAA, 0, 0, 5, opcode]), "network_get", deadline)
        return self.receive(lambda frame: frame[4] == opcode, deadline)[5:]

    def exchange(self, cls: int, opcode: int, payload=b"", *, mutation=False, deadline=None) -> bytes:
        deadline = self.clock() + 5 if deadline is None else min(deadline, self.clock() + 5)
        self.send(report_request(cls, opcode, payload), f"{cls:02x}/{opcode:02x}", deadline, mutation=mutation)
        reply = self.receive(lambda frame: len(frame) == 95 and frame[4] == 0
                             and frame[11:13] == bytes([cls, opcode]), deadline)
        try:
            return decode_report(reply, cls, opcode)
        except RemoteError:
            raise
        except BaseException:
            self.poisoned = True
            raise

    def quiet(self, seconds: float) -> None:
        require(0 <= seconds <= 40, "Quiet interval outside reviewed bounds")
        self.quiet_until = max(self.quiet_until, self.clock() + seconds)
        _event_progress(self, "quiet", 0, seconds, "seconds")
        cancelled = False
        # Cleanup cannot close/reconnect through an interrupt during IAP.
        while self.clock() < self.quiet_until:
            try:
                self.sleep(self.quiet_until - self.clock())
            except KeyboardInterrupt:
                cancelled = True
        self.audit.record("quiet_completed", deadline=self.quiet_until, seconds=seconds)
        _event_progress(self, "quiet", seconds, seconds, "seconds")
        if cancelled:
            raise KeyboardInterrupt("Cancelled after preserving controller quiet interval")

    def close(self):
        if self.sock:
            # Even an audit failure cannot bypass the physical quiet interval.
            while self.clock() < self.quiet_until:
                try:
                    self.sleep(self.quiet_until - self.clock())
                except KeyboardInterrupt:
                    pass
            self.sock.close()
            self.sock = None


def read_stock_mac(target: Target, device_id: str, audit: Audit, *,
                   socket_factory=socket.socket, clock=time.monotonic) -> bytes:
    """One ESP-local UDP identity read; no HELLO or controller transaction.

    The public device ID contains the final three MAC bytes. The complete MAC
    is recorded, while the manifest name and stock version are checked on TCP.
    A connected UDP socket accepts replies only from the selected IP/port.
    """
    require(isinstance(device_id, str) and re.fullmatch(r"keylight-[0-9a-f]{6}", device_id),
            "Invalid expected device ID")
    request = bytes.fromhex("aa00000589")
    audit.record("stock_mac_read_intent", opcode=0x89, request_sha256=digest(request))
    deadline = clock() + 2
    with socket_factory(socket.AF_INET, socket.SOCK_DGRAM) as peer:
        peer.settimeout(2)
        peer.connect((target.ip, 10005))
        require(clock() < deadline, "Stock UDP identity deadline expired")
        peer.settimeout(deadline - clock())
        require(peer.send(request) == len(request), "Incomplete UDP identity request")
        reply = peer.recv(256)
    require(clock() < deadline, "Stock UDP identity completed after deadline")
    require(len(reply) == 11 and reply[:5] == bytes.fromhex("aa02000b89"),
            "Stock UDP identity envelope differs")
    mac = reply[5:]
    require(any(mac) and not mac[0] & 1 and device_id == "keylight-" + mac[-3:].hex(),
            "Selected device ID and stock MAC differ")
    audit.record("stock_mac_verified", mac=mac.hex(), device_id=device_id, reply_sha256=digest(reply))
    return mac


def inspect_controller_package(package: bytes) -> dict:
    require(len(package) == 64 + NXP_BANK_BYTES, "Require one complete original controller package")
    require(package[:8] == b"OKLCNXP\0" and package[8:24] ==
            struct.pack(">HHII4B", 1, 64, NXP_BANK_BYTES, 0xBC40, 1, 0, package[22], 0)
            and package[22] in (1, 2) and package[60:64] == bytes(4), "Unknown controller package profile")
    require(any(package[24:28]) and digest(package[64:]) == package[28:60].hex(), "Invalid controller package digest/version")
    vectors = struct.unpack_from("<48I", package, 64)
    require(vectors[0] == 0x10001000 and all(v & 1 and 0x20C1 <= v <= 0x8FFF for v in vectors[1:]),
            "Controller vectors are outside the reviewed application layout")
    return {"sha256": digest(package), "bank_sha256": digest(package[64:]),
            "role": package[22], "version": package[24:28].hex()}


def decode_controller_status(data: bytes) -> dict:
    require(len(data) == 24 and data[:6] == b"OKLC\x01\0" and data[6] in (0, 1, 2)
            and not data[7] & ~3, "Unsupported original controller ABI")
    caps, part, uptime, reset = struct.unpack_from(">4I", data, 8)
    require(not caps & ~3, "Unknown controller capabilities")
    return {"role": data[6], "confirmed": bool(data[7] & 1), "boot_requested": bool(data[7] & 2),
            "capabilities": caps, "part_id": part, "uptime_ms": uptime, "reset_cause": reset}


def _event_progress(session, scope, completed, total, unit):
    events = getattr(session, "events", None)
    if events is not None:
        try:
            events.defer_progress(stage_id=session.stage_id, scope=scope,
                                  completed=completed, total=total, unit=unit)
        except OSError:
            # Losing the TUI must not cut short IAP or its protected quiet
            # interval. The failed event stream stops execution at the next
            # complete stage boundary instead.
            pass


def transfer_controller(session: StockSession, bank: bytes, *, deadline: float) -> dict:
    """Full bank transaction on an already proven fresh resident session.

    This primitive is private to Migration below: it does not establish target,
    bank provenance, resident identity or pending-buffer freshness itself.
    """
    require(type(bank) is bytes and len(bank) == NXP_BANK_BYTES, "Invalid controller bank")
    audit = {"program_blocks": 0, "verified_blocks": 0, "commit_attempted": False}
    session.audit.record("controller_transfer", bank_sha256=digest(bank))
    _event_progress(session, "controller_program", 0, 448, "blocks")
    try:
        bounds = struct.pack(">II", 0x2000, 0x8FFF)
        require(session.exchange(16, 1, bounds, mutation=True, deadline=deadline) == bounds, "Erase ACK mismatch")
        for offset in range(0, NXP_BANK_BYTES, 64):
            data = b"\x40" + struct.pack(">I", 0x2000 + offset) + bank[offset:offset + 64]
            require(session.exchange(16, 2, data, mutation=True, deadline=deadline) == data, "Program ACK mismatch")
            audit["program_blocks"] += 1
            if audit["program_blocks"] % 16 == 0:
                _event_progress(session, "controller_program", audit["program_blocks"], 448, "blocks")
        _event_progress(session, "controller_verify", 0, 448, "blocks")
        for offset in range(0, NXP_BANK_BYTES, 64):
            prefix = b"\x40" + struct.pack(">I", 0x2000 + offset)
            # This read flushes the last pending program sector. It is not a
            # generic read-only probe and is only used inside this transaction.
            actual = session.exchange(16, 0x83, prefix + bytes(64), deadline=deadline)
            require(actual == prefix + bank[offset:offset + 64], "Staging readback mismatch")
            audit["verified_blocks"] += 1
            if audit["verified_blocks"] % 16 == 0:
                _event_progress(session, "controller_verify", audit["verified_blocks"], 448, "blocks")
        session._remaining(deadline)
        audit["commit_attempted"] = True
        session.audit.record("commit_intent", **audit, bank_sha256=digest(bank), delivery="maybe_sent")
        # Establish the guard before attempting sendall, even if it fails.
        try:
            session.send(report_request(16, 5), "end_commit_once", deadline, mutation=True)
        finally:
            session.quiet(3)
        audit["commit_delivery"] = "complete"
        session.audit.record("commit_requested", **audit)
        return audit
    except BaseException:
        # No Abort after End, no uncertain-phase traffic, no reconnect/retry.
        if not audit["commit_attempted"] and not session.poisoned:
            try:
                session.send(report_request(16, 4), "abort_once", session.clock() + 5, mutation=True)
            finally:
                session.quiet(3)
        raise


class EspNotifications:
    """Pure OTA receive model; acceptance is never inferred from bytes sent."""
    def __init__(self):
        self.frames = ResponseFrames()
        self.frames.count = 0
        self.end_attempted = False
        self.success = False
        self.percent = None

    def feed(self, data: bytes):
        # An ESP image can produce more than 64 notifications. Bound each
        # accumulated session separately without loosening normal exchanges.
        self.frames.count = 0
        for frame in self.frames.feed(data):
            require(frame[1] == 2, "ESP transport rejected update")
            if frame[4] != 1:
                continue
            require(len(frame) == 7 and frame[5] <= 100 and frame[6] == 1,
                    "Malformed or failed ESP update notification")
            require(frame[5] != 100 or self.end_attempted, "Premature ESP success before End")
            self.success |= frame[5] == 100
            self.percent = frame[5]


def transfer_esp(session: StockSession, image: bytes, *, selector=select.select) -> dict:
    """Send one ESP application; receive concurrently to avoid TCP deadlock.

    A 100% notification AND clean peer close are required. Late errors or
    incomplete tails override success. This does not confirm a new ESP trial.
    """
    inspect_esp_application(image)
    require(not session.poisoned and session.sock is not None, "Unusable ESP session")
    model, stopped = EspNotifications(), threading.Event()
    state = {"error": None, "closed": False}

    def receive():
        try:
            while not stopped.is_set():
                readable, _, _ = selector([session.sock], [], [], 0.1)
                if not readable:
                    continue
                data = session.sock.recv(4096)
                if not data:
                    state["closed"] = True
                    break
                session.audit.record("esp_notification_bytes", bytes=len(data), sha256=digest(data))
                model.feed(data)
                if model.percent is not None:
                    _event_progress(session, "esp_processing", model.percent, 100, "percent")
            require(not model.frames.pending, "Incomplete trailing ESP notification")
        except BaseException as error:
            state["error"] = f"{type(error).__name__}: {error}"

    worker = threading.Thread(target=receive, name="stock-esp-receive", daemon=True)
    session.audit.record("esp_transfer_intent", sha256=digest(image), bytes=len(image))
    worker.start()
    sent = 0
    _event_progress(session, "esp_upload", 0, len(image), "bytes")
    try:
        deadline = session.clock() + 180
        packets = iter(esp_transfer_frames(image))
        session.send(next(packets), "esp_start_once", deadline, mutation=True)
        for packet in packets:
            require(state["error"] is None and not state["closed"], state["error"] or "ESP closed before End")
            is_end = packet == b"\xaa\0\0\x06\x01\x02"
            if is_end:
                model.end_attempted = True
            session.send(packet, "esp_end_once" if is_end else "esp_data", deadline, mutation=True)
            if not is_end:
                sent += len(packet) - 6
                if sent == len(image) or sent // 32768 != (sent - len(packet) + 6) // 32768:
                    _event_progress(session, "esp_upload", sent, len(image), "bytes")
                session.sleep(0.003)
        finish = session.clock() + 45
        while not state["closed"] and state["error"] is None:
            session._remaining(finish)
            stopped.wait(0.05)
        require(state["error"] is None and state["closed"] and model.success,
                state["error"] or "ESP closed without final success")
    except BaseException:
        session.poisoned = True
        raise
    finally:
        stopped.set()
        worker.join(timeout=2)
        if worker.is_alive():
            session.poisoned = True
            raise MigrationError("ESP receiver did not settle; update outcome is unknown")
    require(state["error"] is None and not model.frames.pending, state["error"] or "Trailing notification")
    session.audit.record("esp_transfer_accepted", bytes_sent=sent, trial_confirmed=False)
    return {"bytes_sent": sent, "device_reported_success": True, "trial_confirmed": False}


class Migration:
    """Non-resumable migration stages, retaining the stock ESP until last.

    The caller is a reviewed installer, not an untrusted manifest interpreter.
    Each supplied SHA binds an independently reviewed original artifact; a
    digest alone is not source/behavior attestation. Only a reviewed no-PWM
    30-second role-1 identity image may precede a real ROM-IAP FE read.
    OFF1/LOW1 are separately invoked experiments, never auto-confirmed.
    """
    def __init__(self, session: StockSession, packages: dict[str, bytes], pins: dict[str, str],
                 *, restore_bank: bytes, restore_sha256: str):
        require(set(packages) == set(pins) == {"identity", "OFF1", "LOW1", "lighting"},
                "Require all four independently reviewed original packages")
        self.metadata = {}
        for name, package in packages.items():
            metadata = inspect_controller_package(package)
            require(metadata["sha256"] == pins[name], f"Changed {name} package")
            require(metadata["role"] == (2 if name == "lighting" else 1), "Stage and declared role differ")
            self.metadata[name] = metadata
        require(type(restore_bank) is bytes and len(restore_bank) == NXP_BANK_BYTES
                and digest(restore_bank) == restore_sha256, "Owner-local restore artifact differs")
        # The restore image is checked and retained, never automatically sent
        # or included in the public distribution/audit. A staging read is not
        # accepted as an active-image backup or proof it can restore this board.
        require(any(restore_bank), "Empty restore artifact")
        self.packages, self.restore_bank = dict(packages), restore_bank
        self.session, self.phase, self.part_verified = session, "new", False
        self.fresh_resident, self.current, self.record_verified = False, None, False
        self.completed_profiles, self.lighting_approved = set(), False
        self.cold_recovery = False
        self.existing_lifecycle = None
        session.audit.record("artifacts_validated", packages=self.metadata, restore_sha256=restore_sha256)

    @contextmanager
    def _stage(self, allowed, name):
        require(self.phase in allowed, f"Stage {name} is not permitted from {self.phase}")
        self.session.audit.record("stage_intent", stage=name)
        try:
            yield
        except BaseException as error:
            self.phase = "failed"
            # A persistence failure must not be hidden by an attempted log.
            if not self.session.audit.failed:
                self.session.audit.record("stage_failed", stage=name, outcome="requires_explicit_recovery",
                                          error_type=type(error).__name__, error=str(error)[:240])
            raise

    def open_stock(self):
        with self._stage({"new"}, "stock_identity"):
            s = self.session
            s.connect()
            name = s.network_get(0x88)
            require(name and name[0] == len(name) - 1 and name[1:].decode("utf-8") == s.target.name,
                    "Selected light name differs")
            version = s.network_get(0x84)
            require(version == s.target.esp_version, "Stock ESP version differs")
            require(s.exchange(0, 0x87) == bytes([1, 3, 0, 0]), "Unsupported stock controller")
            require(s.exchange(0, 0x84) == b"\0", "Stock controller is not in normal mode")
            self.phase = "stock"
            s.audit.record("stock_identity_matched", name=s.target.name, esp_version=version.hex(),
                           silicon_verified=False, cryptographic_identity=False)

    def _existing_status(self, deadline: float, *, confirmed=None):
        """Correlated original status with a monotonically intersected boot window."""
        s = self.session
        before = s.clock()
        require(before < deadline, "Existing controller admission deadline expired; no flash")
        try:
            status = decode_controller_status(s.exchange(0, 0xFC, deadline=deadline))
        except ValueError as error:
            raise ValueError("Controller is not a ready Open Keylight lighting application; no flash") from error
        after = s.clock()
        require(after < deadline, "Existing controller status arrived after deadline; no flash")
        require(status["role"] == 2 and status["capabilities"] == 3
                and status["part_id"] == 0xBC40 and not status["boot_requested"]
                and (confirmed is None or status["confirmed"] is confirmed),
                "Controller is not a ready Open Keylight lighting application; no flash")
        low, high = before - status["uptime_ms"] / 1000 - .05, after - status["uptime_ms"] / 1000 + .05
        if self.existing_lifecycle is not None:
            previous = self.existing_lifecycle
            low, high = max(low, previous["low"]), min(high, previous["high"])
            require(low <= high and status["reset_cause"] == previous["status"]["reset_cause"]
                    and status["uptime_ms"] >= previous["status"]["uptime_ms"],
                    "Existing controller lifecycle changed; no flash")
        self.existing_lifecycle = {"low": low, "high": high, "status": status}
        return status

    def open_existing(self, device_id: str, *, mac_reader=read_stock_mac,
                      nonce_factory=lambda: secrets.token_hex(8)):
        """Admit a retained original lighting app without reflashing its bank.

        This is not diagnostic qualification or a continuation of an old flash.
        Original 00/49 takes ownership from the real SPI tag, not payload MAC.
        A fresh nonce label and C9 prove that explicit claim even when HELLO has
        no unsolicited owner notification. Legacy claim rules stay unchanged.
        """
        with self._stage({"new"}, "existing_controller"):
            s = self.session
            mac_reader(s.target, device_id, s.audit)
            s.connect(require_owner=False)
            name = s.target.name.encode("utf-8")
            require(s.network_get(0x88) == bytes([len(name)]) + name
                    and s.network_get(0x84) == s.target.esp_version, "Existing target/stock ESP differs; no flash")
            deadline = s.clock() + 15
            mismatch = "Controller is not a ready Open Keylight lighting application; no flash"
            require(s.exchange(0, 0x87, deadline=deadline).hex() == self.metadata["lighting"]["version"]
                    and s.exchange(0, 0xFE, deadline=deadline) == b"\0\0\xbc\x40", mismatch)
            sampled_at = s.clock()
            initial = self._existing_status(deadline)
            if not initial["confirmed"]:
                require(initial["uptime_ms"] < 15000, "Original controller trial is too old; no flash")
                deadline = min(deadline, sampled_at + (20000 - initial["uptime_ms"]) / 1000)
            nonce = nonce_factory()
            require(isinstance(nonce, str) and re.fullmatch(r"[0-9a-f]{16}", nonce), "Invalid fresh ownership nonce")
            label = b"OKL finish " + nonce.encode("ascii")
            claim = b"\1" + bytes(6) + bytes([len(label)]) + label + bytes(64 - len(label))
            # Recheck the same boot and confirmation state immediately before
            # the first mutation; every exchange shares the original deadline.
            self._existing_status(deadline, confirmed=initial["confirmed"])
            require(s.clock() < deadline, "Existing controller claim deadline expired; no flash")
            require(s.exchange(0, 0x49, claim, mutation=True, deadline=deadline) == claim,
                    "Original ownership ACK differs; no retry")
            owner = s.exchange(0, 0xC9, deadline=deadline)
            require(len(owner) == 8 + len(label) and owner[0] == 1 and any(owner[1:7])
                    and not owner[1] & 1 and owner[7] == len(label) and owner[8:] == label,
                    "Original ownership nonce/readback differs; no flash")
            s.routing_identity = owner[1:7]
            require(s.exchange(15, 0x82, bytes(2), deadline=deadline) == bytes(6)
                    and s.exchange(3, 0x83, b"\0\x20\0", deadline=deadline) == b"\0\x20\0\0",
                    "Existing original controller is not verified Off; no flash")
            self._existing_status(deadline, confirmed=initial["confirmed"])
            if not initial["confirmed"]:
                require(s.clock() < deadline, "Original confirmation deadline expired; no flash")
                require(s.exchange(0, 0xFD, b"OKLC", mutation=True, deadline=deadline) == b"\1",
                        "Original confirmation ACK differs; no retry")
            final = self._existing_status(deadline, confirmed=True)
            require(s.exchange(15, 0x82, bytes(2), deadline=deadline) == bytes(6)
                    and s.exchange(3, 0x83, b"\0\x20\0", deadline=deadline) == b"\0\x20\0\0",
                    "Existing original Off readback changed; no flash")
            self.current, self.phase = "lighting", "controller_confirmed"
            s.audit.record("existing_controller_admitted", controller=final, native_off_verified=True,
                           controller_flash=False, diagnostics_performed=False, optical_observation=False,
                           confirmation_sent=not initial["confirmed"])

    def _claim(self):
        # The stock bridge supplies the routing tag, not the request's copied
        # MAC field. Bind it to the complete connection notification observed
        # before HELLO acceptance; do not fabricate a local tag.
        identity, label = self.session.routing_identity, b"Open Keylight migration"
        require(identity is not None, "No observed routing identity")
        data = b"\x01" + identity + bytes([len(label)]) + label + bytes(64 - len(label))
        result = self.session.exchange(0, 0x49, data, mutation=True)
        require(result == data, "Ownership ACK differs")
        expected = data[:8 + len(label)]
        require(self.session.exchange(0, 0xC9) == expected, "Ownership readback differs")

    def _dark(self):
        effect = self.session.exchange(15, 0x82, bytes(2))
        white = self.session.exchange(3, 0x83, b"\0\x20\0")
        require(effect == bytes(6) and white == b"\0\x20\0\0", "Native Off settings were not verified")

    def enter_stock_loader(self):
        with self._stage({"stock"}, "fresh_stock_loader"):
            self._claim()
            # Stock 1.3 normalizes the effect's second argument to 5 in its
            # 12-byte setter ACK. The independent getter below remains the
            # exact six-zero Off record; this is not the original app's echo.
            require(self.session.exchange(15, 2, bytes(12), mutation=True) == b"\0\x05" + bytes(10),
                    "RGB Off ACK differs")
            # Switching RGB off can restore remembered white-only brightness.
            # Clear white afterwards, then require both independent getters.
            require(self.session.exchange(3, 3, b"\0\x20\0\0", mutation=True) == b"\0\x20\0\0",
                    "White Off ACK differs")
            self._dark()
            # Follow the stock updater's fixed two-frame entry recipe. The
            # app resets immediately on 00/04=1 without replying; class 00 is
            # a no-op in the resident loader. This is a planned sequence,
            # never a retry selected from a missing reply. Keep one socket.
            step = "entry sequence"
            try:
                self.session.audit.record("stock_entry_sequence", frames=2, gap_ms=100,
                                          adaptive_retry=False, image_operations=0)
                for index in (1, 2):
                    step = f"fixed entry frame {index}"
                    try:
                        self.session.send(report_request(0, 4, b"\x01\0"), f"stock_entry_fixed_{index}",
                                          self.session.clock() + 5, mutation=True)
                    finally:
                        self.session.quiet(.1)
                # TCP dispatch is synchronous. An ESP-local getter queued
                # after both frames proves those bridge calls have returned,
                # without another controller command or reconnect. It does
                # not prove loader entry; the exact Info/fingerprint do that.
                step = "ESP queue barrier"
                require(self.session.network_get(0x84) == self.session.target.esp_version,
                        "Stock ESP version changed across entry")
                self.session.audit.record("stock_entry_queue_barrier", esp_version=self.session.target.esp_version.hex())
                step = "resident information"
                self._information()
                step = "resident fingerprint"
                self._capture_resident()
            except (ValueError, OSError, MigrationError) as error:
                raise StockEntryError(
                    f"Stock loader entry was not confirmed at {step}. No controller image was erased, "
                    f"programmed or committed. Preserve the audit; no automatic retry. {type(error).__name__}: {error}"
                ) from error
            self.fresh_resident, self.phase = True, "resident"

    def _capture_resident(self):
        # Only after an explicit known reset and exact information. Read83 is
        # not used to guess freshness: it can flush pre-existing pending data.
        captured = bytearray()
        deadline = self.session.clock() + 60
        for address in range(0, NXP_LOADER_BYTES, 64):
            prefix = b"\x40" + struct.pack(">I", address)
            reply = self.session.exchange(16, 0x83, prefix + bytes(64), deadline=deadline)
            require(len(reply) == 69 and reply[:5] == prefix, "Resident capture echo differs")
            captured.extend(reply[5:])
        match = inspect_nxp_loader(bytes(captured))
        self.session.audit.record("resident_fingerprint", **match)

    def open_cold_recovery(self, *, whole_light_power_cycled: bool):
        """Separate explicit operation after a real whole-light power cycle.

        The acknowledgment supplies the physical reset premise; neither a new
        TCP connection nor Info80 proves it. Requires the resident loader to be
        presently responding; an active application is rejected, not reset.
        """
        with self._stage({"new"}, "cold_recovery"):
            require(whole_light_power_cycled is True, "Whole-light power-cycle acknowledgment required")
            self.session.audit.record("operator_reset_premise", whole_light_power_cycled=True)
            self.session.connect(require_owner=False)
            name = self.session.network_get(0x88)
            require(name and name[0] == len(name) - 1 and name[1:].decode() == self.session.target.name,
                    "Recovery target name differs")
            require(self.session.network_get(0x84) == self.session.target.esp_version, "Recovery ESP differs")
            self._information()
            self._capture_resident()
            self.fresh_resident, self.cold_recovery, self.phase = True, True, "resident"

    def restore_owner_bank(self, expected_version: bytes):
        """Explicit recovery only. Never called from any failure handler."""
        with self._stage({"resident"}, "owner_requested_restore"):
            require(self.cold_recovery and self.fresh_resident, "Restore requires a new explicit cold recovery session")
            require(type(expected_version) is bytes and len(expected_version) == 4, "Exact restore version required")
            self.fresh_resident = False
            result = transfer_controller(self.session, self.restore_bank, deadline=self.session.clock() + 180)
            require(self.session.exchange(0, 0x87) == expected_version, "Restored application version was not observed")
            self.phase = "restored_application_seen"
            self.session.audit.record("restored_application_seen", version=expected_version.hex(),
                                      output_verified=False, optical_observation=False)
            return result

    def _information(self):
        require(self.session.exchange(16, 0x80, bytes(80)) == NXP_INFORMATION + bytes(71),
                "Resident information differs")

    def _original(self, stage):
        version = self.session.exchange(0, 0x87)
        part = self.session.exchange(0, 0xFE)
        status = decode_controller_status(self.session.exchange(0, 0xFC))
        expected = self.metadata[stage]
        require(version.hex() == expected["version"] and part == b"\0\0\xbc\x40"
                and status["part_id"] == 0xBC40 and status["role"] == expected["role"]
                and status["capabilities"] == (3 if stage == "lighting" else 1)
                and not status["boot_requested"], "Original controller identity/readiness differs")
        return status

    def install(self, stage: str):
        with self._stage({"resident"}, f"install_{stage}"):
            require(stage in self.packages and self.fresh_resident, "No fresh resident proof")
            require(self.part_verified or stage == "identity", "Actual ROM-IAP part identity required before PWM-capable stages")
            require(stage != "identity" or not self.part_verified, "Identity probe is not repeated")
            require(stage != "lighting" or self.lighting_approved, "Explicit diagnostic/physical qualification is required")
            require(stage != "LOW1" or "OFF1" in self.completed_profiles, "OFF1 must pass before nonzero LOW1")
            self.fresh_resident = False
            transfer_controller(self.session, self.packages[stage][64:], deadline=self.session.clock() + 180)
            status = self._original(stage)
            require(not status["confirmed"] and status["uptime_ms"] < 20000, "Expected a fresh unconfirmed trial")
            self._claim()
            self._dark()
            self.current, self.phase, self.record_verified = stage, "trial", stage == "identity"
            if stage == "identity":
                self.part_verified = True
            self.session.audit.record("original_trial_observed", stage=stage, controller=status,
                                      native_off_verified=True, optical_observation=False)
            return status

    def run_diagnostic(self):
        with self._stage({"trial"}, "diagnostic"):
            require(self.current in ("OFF1", "LOW1") and not self.record_verified, "No unused diagnostic profile")
            require(self._original(self.current)["uptime_ms"] < 20000,
                    "Diagnostic trigger window is no longer fresh")
            # Kept in a separate module to test exact register predicates against
            # firmware fixtures without coupling them to sockets or mutation.
            from migration_profiles import validate_initial, validate_record
            name = self.current
            magic = name.encode("ascii")
            pages, opcode, pause = (14, 0x70, 0.6) if name == "OFF1" else (16, 0x71, 6.7)
            suffix = b"\0\x01\x90" if name == "OFF1" else b"\x05\0\x64"
            require(self.session.exchange(0, 0xF0) == magic + b"\0" + suffix, "Diagnostic already used or profile differs")

            def page(number):
                reply = self.session.exchange(0, 0xF1, bytes([number]))
                require(len(reply) == 72 and reply[:8] == magic + bytes([number, pages, 64, 0]),
                        "Diagnostic page envelope differs")
                return reply[8:]

            generation = validate_initial(name, page(0))
            require(self.session.exchange(0, opcode, magic, mutation=True) == magic, "Diagnostic trigger ACK differs")
            self.session.quiet(pause)
            require(self.session.exchange(0, 0xF0) == magic + b"\x03" + suffix, "Diagnostic did not complete")
            record = b"".join(page(number) for number in range(pages))
            require(page(0) == record[:64], "Diagnostic snapshot changed while reading")
            result = validate_record(name, record, generation)
            self.session.audit.record("diagnostic_registers_verified", **result)
            self.record_verified = True
            self.completed_profiles.add(name)
            return record

    def accept_physical_checks(self, *, off_was_dark: bool, low_channels_expected: bool):
        """Explicit operator observation, never manufactured from MMIO records."""
        with self._stage({"resident"}, "physical_qualification"):
            require(self.completed_profiles == {"OFF1", "LOW1"} and self.part_verified,
                    "Both register diagnostics and actual part identity are required")
            require(off_was_dark is True and low_channels_expected is True,
                    "Operator must verify darkness and the five bounded channels")
            self.lighting_approved = True
            self.session.audit.record("operator_physical_observations", off_was_dark=True,
                                      low_channels_expected=True, instrumented_electrical_measurement=False)

    def await_recovery(self):
        with self._stage({"trial"}, "diagnostic_recovery"):
            require(self.current != "lighting" and self.record_verified, "Diagnostic evidence is incomplete")
            self.session.quiet(33)
            self._information()
            self.fresh_resident, self.phase = True, "resident"
            self.session.audit.record("fresh_resident_after_reviewed_trial", source=self.current,
                                      bank_sha256=self.metadata[self.current]["bank_sha256"])

    def confirm_controller(self):
        with self._stage({"trial"}, "controller_confirmation"):
            require(self.current == "lighting" and self.part_verified, "Diagnostic images cannot be confirmed")
            status = self._original("lighting")
            require(not status["confirmed"] and status["uptime_ms"] < 20000, "Controller trial expired or already confirmed")
            self._dark()
            # Unknown confirmation outcome is not retried. Only a fresh getter
            # after a successful exact ACK can advance this staged installer.
            require(self.session.exchange(0, 0xFD, b"OKLC", mutation=True) == b"\x01", "Confirmation ACK differs")
            require(self._original("lighting")["confirmed"], "Fresh controller confirmation absent")
            self._dark()
            self.phase = "controller_confirmed"
            self.session.audit.record("controller_confirmed", optical_observation=False)

    def install_esp(self, image: bytes, expected_sha256: str, *,
                    stock_profile="keylight-chroma-1.0.13", partition_capture: bytes | None = None,
                    expected_partition_sha256: str | None = None):
        with self._stage({"controller_confirmed"}, "esp_last"):
            require(digest(image) == expected_sha256, "ESP image pin differs")
            inspect_esp_application(image)
            require(stock_profile == "keylight-chroma-1.0.13" and
                    self.session.target.esp_version == bytes([1,0,13,0]), "Unsupported stock update profile")
            if partition_capture is not None:
                layout = inspect_partition_table(partition_capture)
                require(layout["sha256"] == expected_partition_sha256, "Owner's target partition evidence differs")
            else:
                require(expected_partition_sha256 is None, "Partition digest without capture")
            # The reviewed stock application selects/validates the actual
            # inactive partition and bounds every write against its size.
            # The host supplies no flash address. A smaller slot fails before
            # boot selection; the raw table is useful evidence, not required.
            # Matching a stock version is a supported-profile assumption, not
            # per-device bootloader/eFuse attestation or guaranteed first boot.
            self.session.audit.record("esp_supported_profile", profile=stock_profile,
                                      raw_partition_captured=partition_capture is not None,
                                      target_slot_selected_by_device=True, bootloader_attested=False)
            require(self.session.network_get(0x84) == self.session.target.esp_version, "Stock ESP changed")
            require(self._original("lighting")["confirmed"], "Original controller is no longer confirmed")
            self._dark()
            if self.existing_lifecycle is not None:
                self._existing_status(self.session.clock() + 5, confirmed=True)
            result = transfer_esp(self.session, image)
            self.phase = "esp_trial_pending"
            self.session.audit.record("independent_esp_acceptance_required", image_sha256=expected_sha256)
            return result


def bounded_read(path: Path, maximum: int) -> bytes:
    require(path.stat().st_size <= maximum, f"Input exceeds {maximum} bytes")
    with path.open("rb") as source:
        result = source.read(maximum + 1)
    require(len(result) <= maximum, "Input grew beyond bound")
    return result


def _json(data: bytes):
    def unique(items):
        result = {}
        for key, value in items:
            require(key not in result, "Duplicate JSON key")
            result[key] = value
        return result
    return json.loads(data, object_pairs_hook=unique,
                      parse_constant=lambda _: (_ for _ in ()).throw(ValueError("Non-finite JSON")))


def _sha(value):
    require(isinstance(value, str) and re.fullmatch(r"[0-9a-f]{64}", value), "Require a lowercase SHA-256 pin")
    return value


def _version(value):
    require(isinstance(value, str) and re.fullmatch(r"\d{1,3}(\.\d{1,3}){3}", value), "Require four version bytes")
    values = tuple(map(int, value.split(".")))
    require(all(v <= 255 for v in values), "Invalid version byte")
    return bytes(values)


def _asset_path(path):
    return isinstance(path, str) and bool(re.fullmatch(r"/(?:index\.html|assets/[A-Za-z0-9_./-]+)", path)) \
        and ".." not in path and "//" not in path


def load_plan(path: Path) -> dict:
    """Read and pin every artifact before opening a journal or any connection."""
    raw = bounded_read(path, 65536)
    document = _json(raw)
    require(isinstance(document, dict) and set(document) ==
            {"format", "profile", "source_commit", "target", "packages", "restore", "esp", "assets"},
            "Manifest fields differ from format 1")
    require(type(document["format"]) is int and document["format"] == 1 and document["profile"] == "keylight-chroma-1.0.13",
            "Unsupported migration profile")
    require(isinstance(document["source_commit"], str) and
            re.fullmatch(r"[0-9a-f]{40}", document["source_commit"]), "Full reviewed source commit required")
    target_data = document["target"]
    require(isinstance(target_data, dict) and set(target_data) == {"ip", "name", "device_id"}, "Target fields differ")
    target = Target(target_data["ip"], target_data["name"])
    require(isinstance(target_data["device_id"], str) and
            re.fullmatch(r"keylight-[0-9a-f]{6}", target_data["device_id"]), "Expected device ID required")

    def artifact(spec, maximum, extra=()):
        require(isinstance(spec, dict) and set(spec) == {"path", "sha256", *extra}, "Artifact fields differ")
        require(isinstance(spec["path"], str) and spec["path"], "Artifact path required")
        local = (path.parent / spec["path"]).resolve()
        content = bounded_read(local, maximum)
        require(digest(content) == _sha(spec["sha256"]), f"Artifact changed: {local.name}")
        return content

    require(isinstance(document["packages"], dict) and set(document["packages"]) ==
            {"identity", "OFF1", "LOW1", "lighting"}, "Require four stage packages")
    packages, pins = {}, {}
    for name, spec in document["packages"].items():
        packages[name] = artifact(spec, NXP_BANK_BYTES + 64)
        info = inspect_controller_package(packages[name])
        require(info["role"] == (2 if name == "lighting" else 1), "Package stage/role mismatch")
        pins[name] = spec["sha256"]
    restore = document["restore"]
    restore_bank = artifact(restore, NXP_BANK_BYTES, ("version", "provenance"))
    require(len(restore_bank) == NXP_BANK_BYTES and any(restore_bank), "Complete restore bank required")
    require(isinstance(restore["provenance"], str) and 16 <= len(restore["provenance"]) <= 1000,
            "Document the owner-local restore artifact's provenance; a staging capture is not an active backup")
    restore_version = _version(restore["version"])
    image = artifact(document["esp"], ESP_SLOT_BYTES)
    esp_metadata = inspect_esp_application(image)
    assets = _json(artifact(document["assets"], 131072))
    require(isinstance(assets, dict) and type(assets.get("version")) is int and assets.get("version") == 1 and
            isinstance(assets.get("files"), list) and 1 <= len(assets["files"]) <= 8, "Invalid dashboard manifest")
    paths = set()
    for item in assets["files"]:
        require(isinstance(item, dict) and _asset_path(item.get("path")) and item["path"] not in paths,
                "Invalid or duplicate dashboard asset path")
        paths.add(item["path"])
        _sha(item.get("sha256"))
        require(type(item.get("size")) is int and 0 < item["size"] <= 1048576, "Invalid asset size")
    require("/index.html" in paths, "Dashboard index is absent")
    embedded, offset, candidates = [], 0, 0
    while True:
        offset = image.find(b"\x1f\x8b\x08", offset)
        if offset < 0: break
        candidates += 1
        require(candidates <= 32, "Too many embedded gzip candidates")
        try:
            decoder = zlib.decompressobj(31)
            decoded = decoder.decompress(image[offset:], 1048577)
            if decoder.eof and len(decoded) <= 1048576:
                embedded.append((digest(decoded), len(decoded)))
        except zlib.error:
            pass
        offset += 3
    require(all(embedded.count((item["sha256"], item["size"])) == 1 for item in assets["files"]),
            "Dashboard manifest does not match exactly one copy of each embedded asset")
    return {"target": target, "device_id": target_data["device_id"], "profile": document["profile"],
            "source_commit": document["source_commit"], "manifest_sha256": digest(raw),
            "packages": packages, "pins": pins, "restore_bank": restore_bank,
            "restore_sha256": restore["sha256"], "restore_version": restore_version,
            "restore_provenance": restore["provenance"], "esp_image": image,
            "esp_metadata": esp_metadata, "assets": assets["files"]}


def http_get(ip: str, path: str, maximum: int) -> bytes:
    """Bounded same-device GET only: no redirects, proxies, tokens or mutations."""
    require(path == "/api/v1/device" or _asset_path(path), "Read-only path outside installer allowlist")
    connection = http.client.HTTPConnection(ip, 80, timeout=5)
    deadline = time.monotonic() + 5
    response = None
    try:
        connection.request("GET", path, headers={"Accept-Encoding":"gzip", "Connection":"close"})
        active_socket = connection.sock
        response = connection.getresponse()
        require(response.status == 200, f"Device GET returned {response.status}")
        chunks, size = [], 0
        # read1() closes the response file (and a Connection: close socket)
        # as soon as its declared Content-Length reaches zero. Do not touch
        # that socket again merely to request an additional EOF read.
        while not response.isclosed():
            remaining = deadline - time.monotonic()
            if remaining <= 0: raise TimeoutError("Device GET deadline expired")
            active_socket.settimeout(remaining)
            chunk = response.read1(min(65536, maximum + 1 - size))
            if not chunk: break
            size += len(chunk); chunks.append(chunk)
            require(size <= maximum, "Device response exceeds bound")
        data = b"".join(chunks)
        if time.monotonic() >= deadline: raise TimeoutError("Device GET completed after deadline")
        require(response.length in (None, 0), "Incomplete HTTP response body")
        require(len(data) <= maximum, "Device response exceeds bound")
        encoding = response.getheader("Content-Encoding", "identity").lower()
        require(encoding in ("identity", "gzip"), "Unknown response encoding")
        if encoding == "gzip":
            decoder = zlib.decompressobj(31)
            data = decoder.decompress(data, maximum + 1)
            require(decoder.eof and not decoder.unused_data and len(data) <= maximum, "Invalid/oversized gzip response")
        return data
    finally:
        try:
            if response is not None: response.close()
        finally:
            connection.close()


def console_output(message):
    """Keep stage/countdown messages visible when the CLI stdout is piped."""
    print(message, flush=True)


def await_native_confirmation(plan, audit, *, get_http=http_get, clock=time.monotonic,
                              sleep=time.sleep, output_fn=console_output, events=None):
    """Read-only acceptance observer; a separate client checks and confirms."""
    target, expected = plan["target"], plan["esp_metadata"]
    started_at = clock()
    deadline, first, previous_uptime = started_at + 175, None, None
    boot_window, reset_reason = None, None
    last_progress = -float("inf")
    expected_controller = ".".join(str(v) for v in plan["packages"]["lighting"][24:28])

    def progress(message, *, force=False):
        nonlocal last_progress
        now = clock()
        if force or now - last_progress >= 10:
            output_fn(message)
            last_progress = now

    def remaining():
        return max(0, round(deadline - clock()))

    def check_cancelled():
        if events is not None: events.check_cancelled()

    def event_progress(scope, completed, remaining_ms=None):
        if events is not None:
            fields = {} if remaining_ms is None else {"remaining_ms": remaining_ms}
            events.emit("progress", stage_id="native", scope=scope,
                        completed=min(175, max(0, completed)), total=175, unit="seconds", **fields)

    def checked_device():
        nonlocal boot_window, reset_reason
        requested_at = clock()
        value = _json(get_http(target.ip, "/api/v1/device", 65536))
        received_at = clock()
        require(isinstance(value, dict) and value.get("id") == plan["device_id"]
                and value.get("firmware") == expected["version"] and value.get("api_version") == 1
                and value.get("firmware_elf_sha256") == expected["elf_sha256"],
                "Native device identity/image differs; no confirmation sent")
        require(type(value.get("uptime_ms")) is int and value["uptime_ms"] >= 0
                and type(value.get("trial_pending")) is bool and type(value.get("reset_reason")) is int,
                "Invalid native lifecycle fields")
        # Uptime need not decrease after a restart if a slow poll misses the
        # early boot. Intersect request-time boot-origin intervals instead.
        # 50 ms covers timer quantization and short acceptance-window drift;
        # a overlapping interval is consistency evidence, not a boot nonce.
        window = (requested_at - value["uptime_ms"] / 1000 - 0.05,
                  received_at - value["uptime_ms"] / 1000 + 0.05)
        if boot_window is not None:
            window = (max(window[0], boot_window[0]), min(window[1], boot_window[1]))
            require(window[0] <= window[1] and value["reset_reason"] == reset_reason,
                    "ESP boot origin/reset cause changed during acceptance")
        boot_window, reset_reason = window, value["reset_reason"]
        return value

    def ready(value):
        c = value.get("controller", {})
        health = c.get("last_health_ms") if isinstance(c, dict) else None
        return isinstance(c, dict) and c.get("ready") is True and c.get("connected") is True \
            and c.get("backend") == "original" and c.get("status") == "ready" \
            and c.get("part_id") == 0xBC40 and c.get("trial_confirmed") is True \
            and c.get("version") == expected_controller and type(health) is int \
            and 0 <= value["uptime_ms"] - health <= 5000

    while clock() < deadline:
        check_cancelled()
        event_progress("startup_wait", clock() - started_at)
        progress(f"Waiting for the original ESP dashboard ({round(clock() - started_at)}s elapsed). "
                 "No update will be retried.")
        try:
            value = checked_device()
        except (OSError, http.client.HTTPException):
            sleep(1)
            continue
        require(clock() < deadline, "Native observation exceeded its startup deadline")
        check_cancelled()
        require(value["trial_pending"], "New image was not observed in trial; do not infer acceptance")
        # Before first contact the host can only bound its own wait. Once
        # uptime is available, use the earliest consistent boot time with a
        # five-second margin before application rollback. Never extend it.
        deadline = min(deadline, boot_window[0] + 175)
        require(clock() < deadline, "Native first boot reached the end of its trial window")
        if ready(value):
            first = value
            break
        sleep(1)
    require(first is not None, "New original ESP/controller did not become ready; preserve audit and inspect recovery")
    progress(f"Exact original image and controller are ready. About {remaining()}s remain to verify assets, "
             "check controls and explicitly confirm the trial.", force=True)
    output_fn(f"Verifying {len(plan['assets'])} embedded dashboard assets…")
    for asset in plan["assets"]:
        check_cancelled()
        content = get_http(target.ip, asset["path"], 1048576)
        require(len(content) == asset["size"] and digest(content) == asset["sha256"], "Served dashboard asset differs")
        require(clock() < deadline, "Asset verification exceeded the ESP trial window")
    audit.record("native_trial_verified", id=first["id"], firmware=first["firmware"],
                 elf_sha256=first["firmware_elf_sha256"], authenticated_controls_checked=False)
    pairing = ("Pair this browser while the pairing window is open. " if first.get("pairing_open") is True
               else "Hold the light's button for three seconds to open pairing, then pair this browser. ")
    require(clock() < deadline, "Trial evidence persistence exceeded the ESP trial window")
    check_cancelled()
    if events is None:
        output_fn(f"Open http://{target.ip}/ now. About {remaining()}s remain. {pairing}"
                  "Check the controls at low brightness, return to Off, then explicitly confirm the trial in the dashboard. "
                  "This installer does not obtain a token or confirm for you.")
    else:
        output_fn(f"Native acceptance pending: about {remaining()}s remain. This backend only observes confirmation; "
                  "the acceptance client must verify controls and preserve its credential privately.")
    previous_uptime = first["uptime_ms"]
    action_sent, pairing_requested = False, False
    while clock() < deadline:
        check_cancelled()
        milliseconds = max(0, int((deadline - clock()) * 1000))
        event_progress("trial", 175 - milliseconds / 1000, milliseconds)
        progress(f"Trial confirmation pending: about {remaining()}s remain. " +
                 ("The acceptance client must verify controls before confirmation." if events is not None else
                  "Confirm in the dashboard after checking controls; the installer will not confirm automatically."))
        value = checked_device()
        check_cancelled()
        deadline = min(deadline, boot_window[0] + 175)
        require(clock() < deadline, "Native confirmation observation exceeded the trial deadline")
        require(value["uptime_ms"] >= previous_uptime, "ESP restarted during acceptance; stop and inspect")
        previous_uptime = value["uptime_ms"]
        require(ready(value), "Controller lost readiness during acceptance")
        if not value["trial_pending"]:
            audit.record("independent_client_confirmation_observed" if events is not None else
                         "human_dashboard_confirmation_observed", authenticated_controls_checked_by_installer=False)
            return value
        if events is not None and not action_sent:
            milliseconds = max(0, int((deadline - clock()) * 1000))
            # The separate native client requires 30 seconds for its bounded
            # checks. Do not dispatch an action it must reject immediately.
            if milliseconds < 30000:
                audit.record("native_acceptance_not_started", reason="pairing_deadline",
                             remaining_ms=milliseconds, pairing_open=value.get("pairing_open") is True)
                raise NativePairingDeadlineError(
                    "Not enough trial time remains to start automatic pairing and verification (30 seconds required). "
                    "No pairing or confirmation was sent by this backend; preserve the audit.")
            is_open = value.get("pairing_open") is True
            if is_open or not pairing_requested:
                audit.record("native_acceptance_action_intent", pairing_open=is_open, remaining_ms=milliseconds,
                             uptime_ms=value["uptime_ms"], authenticated_controls_checked=False)
                require(clock() < deadline, "Acceptance action persistence exceeded the trial window")
                milliseconds = max(0, int((deadline - clock()) * 1000))
                if milliseconds < 30000:
                    raise NativePairingDeadlineError(
                        "Not enough trial time remains after saving acceptance evidence; no acceptance action was sent.")
                # A closed-window action makes the physical pairing step
                # persistent in the UI. Only a later fresh open-window action
                # authorizes the native client to pair; neither is a command.
                events.emit("action", kind="native_acceptance", url=f"http://{target.ip}/", device_id=value["id"],
                            firmware=value["firmware"], elf_sha256=value["firmware_elf_sha256"],
                            manifest_sha256=plan["manifest_sha256"], controller_version=expected_controller,
                            pairing_open=is_open, remaining_ms=milliseconds)
                action_sent = is_open
            if not is_open and not pairing_requested:
                events.emit("status", code="pairing_required", message=
                            "Hold the light's button for three seconds to open pairing. "
                            "The trial countdown continues; no pairing request has been sent.")
                pairing_requested = True
        sleep(1)
    raise TimeoutError("No explicit dashboard confirmation observed before the trial deadline; no automatic confirmation or retry")


def run_cli(argv=None, *, input_fn=input, output_fn=console_output, session_factory=StockSession,
            audit_factory=Audit, get_http=http_get, clock=time.monotonic, sleep=time.sleep,
            event_input=None, event_output=None, mac_reader=read_stock_mac) -> int:
    parser = argparse.ArgumentParser(description="Guided, experimental migration for the reviewed Key Light Chroma stock profile")
    parser.add_argument("command", choices=("prepare", "install", "finish", "restore"))
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--audit", type=Path)
    parser.add_argument("--execute", action="store_true")
    parser.add_argument("--events-jsonl", action="store_true", help="Versioned JSONL events and structured optical responses")
    parser.add_argument("--expected-manifest-sha256", help="Bind execution to the exact previously prepared manifest")
    parser.add_argument("--exclusive-control", action="store_true",
                        help="All other light controllers, polling pages and updaters are closed")
    parser.add_argument("--power-cycled", action="store_true",
                        help="Restore only: the whole light was unplugged/replugged and allowed at least 35 seconds to recover")
    args = parser.parse_args(argv)
    audit, session, migration, events = None, None, None, None
    if args.events_jsonl:
        from migration_events import JsonEvents
        events = JsonEvents(event_output if event_output is not None else sys.stdout,
                            event_input if event_input is not None else sys.stdin)
        output_fn = lambda message: events.emit("status", code="message", message=message)

    def stage(id, index, message, *, phase="started", total=7):
        if session is not None: session.stage_id = id
        if events is not None:
            if phase == "started": events.check_cancelled()
            events.emit("stage", id=id, index=index, total=total, phase=phase, message=message)
            if phase == "completed": events.check_cancelled()
        elif phase == "started":
            output_fn(f"Stage {index}/{total}: {message}")

    def prompt(kind, message):
        if events is not None:
            return events.prompt(kind, message)
        return input_fn(message).strip().lower()

    try:
        plan = load_plan(args.manifest)
        summary = {"profile": plan["profile"], "target_ip": plan["target"].ip, "target_name": plan["target"].name,
                   "device_id": plan["device_id"], "esp": plan["esp_metadata"], "packages": plan["pins"],
                   "controller_version": ".".join(str(v) for v in plan["packages"]["lighting"][24:28]),
                   "restore_sha256": plan["restore_sha256"], "restore_provenance": plan["restore_provenance"],
                   "manifest_sha256": plan["manifest_sha256"], "device_operations": 0}
        if events is not None:
            events.emit("status", code="plan_validated", message="Offline artifact and target plan checks passed", summary=summary)
        else:
            output_fn(json.dumps(summary, indent=2))
        if args.expected_manifest_sha256 is not None:
            require(args.expected_manifest_sha256 == plan["manifest_sha256"], "Prepared manifest digest changed")
        if args.command == "prepare":
            require(not args.execute and not args.power_cycled, "Prepare is strictly offline")
            if events is not None: events.emit("completed", outcome="prepared")
            return 0
        require(args.execute and args.exclusive_control and args.audit is not None,
                "Install/finish/restore require --execute, --exclusive-control and a new --audit path")
        require(args.command == "restore" or not args.power_cycled, "Power-cycle flag is only for explicit restore")
        require(args.command != "restore" or args.power_cycled, "Restore requires the acknowledged whole-light power cycle")
        require(events is None or args.expected_manifest_sha256 is not None,
                "Event-mode execution requires the previously prepared manifest digest")
        audit = audit_factory(args.audit, plan["target"].ip, clock=clock)
        audit.record("plan", source_commit=plan["source_commit"], manifest_sha256=plan["manifest_sha256"],
                     restore_provenance=plan["restore_provenance"], exclusive_control_acknowledged=True)
        if events is not None:
            events.start_input()
            events.check_cancelled()
        session = session_factory(plan["target"], audit, clock=clock, sleep=sleep)
        session.events = events
        migration = Migration(session, plan["packages"], plan["pins"], restore_bank=plan["restore_bank"],
                              restore_sha256=plan["restore_sha256"])
        if args.command == "restore":
            stage("restore", 1, "Verifying a fresh resident controller and restoring the reviewed bank once.", total=1)
            migration.open_cold_recovery(whole_light_power_cycled=True)
            migration.restore_owner_bank(plan["restore_version"])
            stage("restore", 1, "Restored application version observed.", phase="completed", total=1)
            output_fn("Restore application version observed. Independently check normal operation; no lighting was replayed.")
            if events is not None: events.emit("completed", outcome="restored")
            return 0
        finishing = args.command == "finish"
        total, esp_index, native_index = (3, 2, 3) if finishing else (7, 6, 7)
        if finishing:
            if events is not None:
                events.emit("status", code="installation_workflow", workflow="finish", total=3,
                            message="Finish the ESP installation while retaining the verified original controller.",
                            stages=[{"id": "existing", "index": 1, "label": "Verify installed controller"},
                                    {"id": "esp", "index": 2, "label": "Install ESP firmware"},
                                    {"id": "native", "index": 3, "label": "Verify and accept native setup"}])
            stage("existing", 1, "Verifying the installed Open Keylight controller and Off; its firmware is retained.", total=3)
            migration.open_existing(plan["device_id"], mac_reader=mac_reader)
            stage("existing", 1, "Existing original controller confirmed and Off; no controller flash or diagnostic qualification performed.",
                  phase="completed", total=3)
        else:
            stage("stock", 1, "Checking the selected stock light, verifying Off, and entering its controller loader.")
            migration.open_stock(); migration.enter_stock_loader()
            stage("stock", 1, "Fresh resident controller verified.", phase="completed")
            stage("identity", 2, "Installing the no-PWM identity trial, then waiting 33s for resident recovery.")
            migration.install("identity"); migration.await_recovery()
            stage("identity", 2, "ROM part identity and resident recovery verified.", phase="completed")
            stage("off1", 3, "Installing OFF1 and checking its all-low register record, then waiting for recovery.")
            migration.install("OFF1"); migration.run_diagnostic(); migration.await_recovery()
            require(prompt("off1_observation", "Did the light remain completely dark during OFF1? Type yes to proceed to five low pulses: ") == "yes",
                    "OFF1 physical observation was not accepted; no next image or automatic restore")
            stage("off1", 3, "All-low register evidence and physical observation accepted.", phase="completed")
            stage("low1", 4, "Watch the light: one short low pulse each of red, green, blue, warm white and cool white, with dark gaps.")
            migration.install("LOW1"); migration.run_diagnostic(); migration.await_recovery()
            require(prompt("low1_observation", "Were those five low pulses correct, with dark gaps and no unexpected output? Type yes: ") == "yes",
                    "LOW1 physical observation was not accepted; no production image or automatic restore")
            migration.accept_physical_checks(off_was_dark=True, low_channels_expected=True)
            stage("low1", 4, "Five-channel record and physical observation accepted.", phase="completed")
            stage("lighting", 5, "Installing and verifying the original lighting controller, then confirming it once.")
            migration.install("lighting"); migration.confirm_controller()
            stage("lighting", 5, "Original lighting controller confirmation read back.", phase="completed")
        stage("esp", esp_index, "Uploading the ESP application once. Keep power connected; acceptance and first boot are separate checks.", total=total)
        migration.install_esp(plan["esp_image"], plan["esp_metadata"]["sha256"], stock_profile=plan["profile"])
        session.close(); session = None
        stage("esp", esp_index, "Stock ESP accepted the image; independent first-boot acceptance remains.", phase="completed", total=total)
        stage("native", native_index, "Waiting for native HTTP, exact image/assets, and the acceptance client's verified setup."
              if events is not None else
              "Waiting for native HTTP, exact image/assets, and your explicit dashboard confirmation.", total=total)
        await_native_confirmation(plan, audit, get_http=get_http, clock=clock, sleep=sleep, output_fn=output_fn, events=events)
        stage("native", native_index, "Exact native image and explicit confirmation observed.", phase="completed", total=total)
        output_fn("Original controller and ESP confirmations observed; no credential was emitted by this backend."
                  if events is not None else
                  "Original controller and ESP are confirmed. The browser holds its own pairing token; no token was exported.")
        if events is not None: events.emit("completed", outcome="installed")
        return 0
    except (ValueError, OSError, MigrationError, EOFError, KeyboardInterrupt, http.client.HTTPException) as error:
        if audit is not None and not audit.failed:
            audit.record("installer_stopped", error_type=type(error).__name__, error=str(error)[:240],
                         automatic_retry=False, automatic_restore=False)
        if events is not None:
            pending = migration is not None and migration.phase == "esp_trial_pending"
            try:
                events.emit("stopped", message=str(error), error_type=type(error).__name__,
                            code=getattr(error, "code", "installer_stopped"),
                            automatic_retry=False, automatic_restore=False, native_trial_may_be_pending=pending,
                            recovery_hint=("The ESP trial may still be pending; an unconfirmed running application uses its timed fallback."
                                           if pending else "Preserve the audit; no automatic retry or restore."))
            except OSError:
                pass
        else:
            output_fn(f"Stopped: {error}. Preserve the audit. No mutation will be retried automatically.")
        return 1
    finally:
        try:
            if session is not None: session.close()
        finally:
            try:
                if audit is not None: audit.close()
            finally:
                if events is not None: events.close()


def inspect_main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--esp-image", type=Path, required=True, help="Original standalone application .bin")
    parser.add_argument("--partition-table", type=Path, help="Owner's local 4096-byte capture")
    parser.add_argument("--nxp-loader", type=Path, help="Owner's local 8192-byte fresh resident capture")
    parser.add_argument("--nxp-staging", type=Path, help="Owner's complete 28 KiB staging snapshot")
    args = parser.parse_args()
    try:
        result = {"device_operations": 0, "network_execution_available": False,
                  "esp_application": inspect_esp_application(bounded_read(args.esp_image, ESP_SLOT_BYTES)),
                  "migration_ready": False,
                  "remaining_gates": ["Prepare a target-bound manifest and explicitly select the guided install command",
                                      "Live device/profile checks and independent first-boot acceptance",
                                      "Fresh NXP entry, resident fingerprint and original no-PWM identity probe",
                                      "Actual ROM part ID, OFF1/LOW1 observations, then production acceptance"]}
        if args.partition_table:
            result["partition_table"] = inspect_partition_table(bounded_read(args.partition_table, 4096))
        if args.nxp_loader:
            result["nxp_loader"] = inspect_nxp_loader(bounded_read(args.nxp_loader, NXP_LOADER_BYTES))
        if args.nxp_staging:
            result["nxp_snapshot"] = inspect_staging_bank(bounded_read(args.nxp_staging, NXP_BANK_BYTES))
    except (OSError, ValueError) as error:
        parser.error(str(error))
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] in ("prepare", "install", "finish", "restore"):
        raise SystemExit(run_cli())
    inspect_main()
