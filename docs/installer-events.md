# Installer event protocol

The Ink installer runs `tools/stock_migration.py` with `--events-jsonl`. The
backend writes UTF-8 JSON lines to stdout and accepts JSON lines on stdin.
Human CLI mode remains available without that flag. Neither mode resumes an
old audit, retries a mutation, or restores firmware automatically.

First run `prepare --manifest PLAN --events-jsonl`. Execution additionally
requires `--execute --exclusive-control --audit NEW_AUDIT` and
`--expected-manifest-sha256` matching the prepared plan. Restore also requires
the explicit `--power-cycled` acknowledgment. Every artifact is read and checked
before the exclusive audit is created or a device connection is opened.

Every output has `v: 1`, a contiguous zero-based `seq`, and an `event`:

| Event | Fields and meaning |
| --- | --- |
| `status` | `code`, `message`; `plan_validated` also carries the complete prepared `summary` |
| `stage` | `id`, one-based `index`, `total`, `phase: started/completed`, `message` |
| `progress` | `stage_id`, `scope`, `completed`, `total`, `unit`; trial updates include `remaining_ms` |
| `prompt` | Unique `id`, `kind`, `message`, and `choices: [yes, no]` |
| `action` | `kind: native_acceptance`, exact URL/device/image/manifest/controller identity, `pairing_open: true`, `remaining_ms` |
| `completed` | `outcome: prepared/installed/restored` |
| `stopped` | Error type/message, `automatic_retry: false`, `automatic_restore: false`, recovery hint and possible pending ESP trial |

Install stages are `stock`, `identity`, `off1`, `low1`, `lighting`, `esp`, and
`native`. Controller programming/readback progress counts actual acknowledged
or verified blocks. Upload progress counts bytes sent; device processing is
reported separately. Quiet intervals are reported at their beginning and end,
without inventing intermediate hardware progress.

Optical prompts occur only after the diagnostic application has returned to
the resident loader. The client sends exactly one response for the current ID:

```json
{"v":1,"id":"the-current-prompt-id","answer":"yes"}
```

`no`, EOF, malformed input, duplicate fields, or an unexpected prompt ID stops
the workflow. There is no default Yes. Input lines are limited to 1024 UTF-8
bytes. A cooperative cancellation request is:

```json
{"v":1,"command":"cancel"}
```

Cancellation is checked between complete stages, including required commit
quiet and diagnostic recovery waits. It never interrupts programming or ends
the quiet interval early. The native read-only observer checks it at request
boundaries. Clients must keep the child alive until its safe stop; killing the
process is not a cancellation mechanism.

Protected stages enqueue bounded progress without waiting for stdout. A full
queue may drop intermediate presentation updates; verification and the audit
remain authoritative. Stage boundaries drain progress with a two-second bound.
A broken or unresponsive event pipe prevents another stage from starting. Raw
stdio descriptors avoid retaining Python buffered-I/O locks in daemon threads
when the parent keeps its pipes open during child exit.

The native action is emitted once, after exact device/controller/embedded asset
checks and a fresh same-boot read showing pairing open. If pairing is closed,
`pairing_required` asks for the physical button and continues observing inside
the unchanged trial deadline. That deadline only tightens from measured uptime.
The Python backend does not pair, obtain credentials, test output, or confirm.
The separate native client owns those operations and their private evidence;
Python records only that independent confirmation was observed. Credentials
must never appear in this stream.

A successful child exit alone is insufficient: the client requires the matching
terminal event and its own native acceptance result. A stopped native observer
may leave an unconfirmed ESP application running its fallback timer; it does
not establish that fallback or recovery already occurred.

Offline regressions include actual HTTP servers and actual child processes.
`tests/migration/jsonl_fixture.py` provides synthetic stages with real stdin and
stdout for the Ink composition test; it forbids socket construction and is not
shipped as an installer backend.
