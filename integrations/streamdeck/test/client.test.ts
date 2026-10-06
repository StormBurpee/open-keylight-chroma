import { test } from "node:test";
import assert from "node:assert/strict";
import {
  DeviceClient,
  DeviceError,
  DeviceGate,
  deviceOrigin,
  patchFor,
  stateLabel,
  validateState,
  type State,
} from "../src/client.js";
import { DialQueue } from "../src/dial.js";
export const fixture = (): State => ({
  revision: 7,
  desired: { power: true, brightness: 40, recording_lock: false },
  reported: {
    valid: true,
    confirmed_fields: ["power", "brightness"],
    power: true,
    brightness: 40,
  },
  operation: { status: "idle" },
});
const response = (value: unknown, status = 200) =>
  new Response(JSON.stringify(value), { status });
const wait = (n = 10) => new Promise((r) => setTimeout(r, n));

test("normalizes a fixed origin and rejects credential-bearing, non-HTTP and path URLs", () => {
  assert.equal(
    deviceOrigin("http://light.local:8080/"),
    "http://light.local:8080",
  );
  for (const url of [
    "file:///tmp/x",
    "http://token@light.local",
    "http://light.local/api",
    "http://light.local?token=x",
    "http://light.local/#x",
    "not a url",
  ])
    assert.throws(() => deviceOrigin(url), DeviceError);
});
test("reads a fresh revision before one authenticated mutation, blocks redirects", async () => {
  const calls: Array<{ url: unknown; options: RequestInit | undefined }> = [];
  const client = new DeviceClient(
    { url: "http://light.local", token: "test-secret" },
    async (url, options) => {
      calls.push({ url, options });
      return response(fixture(), options?.method === "PATCH" ? 202 : 200);
    },
  );
  await client.apply({ kind: "brightness", delta: 5 });
  assert.equal(calls.length, 2);
  assert.equal(calls[0].options?.method, "GET");
  assert.equal(calls[1].url, "http://light.local/api/v1/state");
  assert.deepEqual(JSON.parse(String(calls[1].options?.body)), {
    brightness: 45,
    expected_revision: 7,
  });
  assert.equal(calls[1].options?.redirect, "error");
  assert.equal(
    (calls[1].options?.headers as Record<string, string>)["X-Keylight-Actor"],
    "streamdeck",
  );
  assert.equal(
    (calls[1].options?.headers as Record<string, string>).Authorization,
    "Bearer test-secret",
  );
});
test("missing token refuses mutation without sending it", async () => {
  let calls = 0;
  const c = new DeviceClient({ url: "http://light.local" }, async () => {
    calls++;
    return response(fixture());
  });
  await assert.rejects(c.apply({ kind: "power" }), { status: 401 });
  assert.equal(calls, 1);
});
for (const status of [409, 423, 401, 500])
  test(`HTTP ${status} is surfaced without mutation retry`, async () => {
    let calls = 0;
    const c = new DeviceClient(
      { url: "http://light.local", token: "x" },
      async (_url, o) => {
        calls++;
        return o?.method === "GET"
          ? response(fixture())
          : response({ error: "refused" }, status);
      },
    );
    await assert.rejects(c.apply({ kind: "power" }), { status });
    assert.equal(calls, 2);
  });
test("network loss after sending is explicitly uncertain and never replayed", async () => {
  let writes = 0;
  const c = new DeviceClient(
    { url: "http://light.local", token: "x" },
    async (_url, o) => {
      if (o?.method === "GET") return response(fixture());
      writes++;
      throw new DOMException("aborted", "AbortError");
    },
  );
  await assert.rejects(c.apply({ kind: "power" }), /may have reached/);
  assert.equal(writes, 1);
});
test("unreadable response and invalid state do not imply success", async () => {
  const c = new DeviceClient(
    { url: "http://light.local" },
    async () => new Response("<html>oops</html>"),
  );
  await assert.rejects(c.state(), /unreadable/);
  assert.throws(() => validateState({ ...fixture(), revision: -1 }));
  assert.throws(() =>
    validateState({
      ...fixture(),
      reported: { valid: true, confirmed_fields: ["power", 42] },
    }),
  );
});
test("recording lock allows only Off and separate explicit unlock", () => {
  const locked = fixture();
  locked.desired.recording_lock = true;
  assert.deepEqual(patchFor(locked, { kind: "power" }), {
    power: false,
    expected_revision: 7,
  });
  assert.deepEqual(patchFor(locked, { kind: "lock" }), {
    recording_lock: false,
    expected_revision: 7,
  });
  assert.throws(() => patchFor(locked, { kind: "brightness", delta: 5 }), {
    status: 423,
  });
  locked.desired.power = false;
  assert.throws(() => patchFor(locked, { kind: "power" }), { status: 423 });
});
test("brightness clamps without turning an off light on", () => {
  const s = fixture();
  s.desired.power = false;
  assert.deepEqual(patchFor(s, { kind: "brightness", delta: 100 }), {
    brightness: 100,
    expected_revision: 7,
  });
  assert.deepEqual(patchFor(s, { kind: "brightness", delta: -100 }), {
    brightness: 0,
    expected_revision: 7,
  });
});
test("scene recall checks capability and never invents a saved scene", async () => {
  const calls: string[] = [];
  const c = new DeviceClient(
    { url: "http://light.local", token: "x" },
    async (url) => {
      calls.push(String(url));
      return String(url).endsWith("/device")
        ? response({
            api_version: 1,
            name: "Light",
            capabilities: { scenes: false },
          })
        : response(fixture());
    },
  );
  await assert.rejects(c.apply({ kind: "scene", id: 2 }), /does not support/);
  assert.equal(calls.length, 2);
});
test("accepted values are marked until the controller confirms the same field", () => {
  const s = fixture();
  assert.equal(stateLabel("brightness", s), "40%");
  s.desired.brightness = 45;
  assert.equal(stateLabel("brightness", s), "45% *");
  s.reported.brightness = 45;
  s.reported.confirmed_fields = [];
  assert.equal(stateLabel("brightness", s), "45% *");
  s.operation.status = "error";
  assert.equal(stateLabel("power", s), "Check light");
});
test("global device gate rejects overlapping writes across different actions and always releases", async () => {
  const gate = new DeviceGate();
  let finish!: () => void;
  const first = gate.run(
    "light",
    () =>
      new Promise<void>((r) => {
        finish = r;
      }),
  );
  await assert.rejects(
    gate.run("light", async () => {}),
    { status: 429 },
  );
  await gate.run("other", async () => {});
  finish();
  await first;
  await assert.rejects(
    gate.run("light", async () => {
      throw Error("no");
    }),
  );
  assert.equal(gate.busy("light"), false);
});
test("dial merges ticks with one submission in flight and drops queued ticks after uncertain failure", async () => {
  const deltas: number[] = [];
  let reject!: (e: unknown) => void;
  let failures = 0;
  const q = new DialQueue(
    async (d) => {
      deltas.push(d);
      await new Promise<void>((_r, j) => {
        reject = j;
      });
    },
    () => {
      failures++;
    },
    2,
  );
  q.add(5);
  q.add(10);
  await wait();
  assert.deepEqual(deltas, [15]);
  q.add(20);
  q.add(30);
  reject(Error("uncertain"));
  await wait();
  assert.deepEqual(deltas, [15]);
  assert.equal(failures, 1);
  q.cancel();
});
test("dial disappearance cancels unsent ticks", async () => {
  let called = false;
  const q = new DialQueue(
    async () => {
      called = true;
    },
    () => {},
    2,
  );
  q.add(5);
  q.cancel();
  await wait();
  assert.equal(called, false);
});
