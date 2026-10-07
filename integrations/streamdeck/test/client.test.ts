import { test } from "node:test";
import assert from "node:assert/strict";
import {
  DeviceClient,
  DeviceError,
  deviceOrigin,
  patchFor,
  stateLabel,
  validateState,
  rotateColor,
  parseColor,
  type State,
} from "../src/client.js";
import { fixture, device, response } from "./fixture.js";
test("fixed origin refuses paths, credentials, queries and non-HTTP schemes", () => {
  assert.equal(
    deviceOrigin("http://light.local:8080/"),
    "http://light.local:8080",
  );
  for (const url of [
    "file:///tmp/x",
    "http://token@light.local",
    "http://light.local/api",
    "http://light.local?x",
    "http://light.local/#x",
    "invalid",
  ])
    assert.throws(() => deviceOrigin(url), DeviceError);
});
test("fresh revision, supported short colour fade, authenticated one-shot PATCH, strict returned intent", async () => {
  const calls: { url: string; options?: RequestInit }[] = [];
  const c = new DeviceClient(
    { url: "http://light.local", token: "private-test-token" },
    async (url, options) => {
      calls.push({ url: String(url), options });
      if (String(url).endsWith("/device")) return response(device);
      const s = fixture();
      if (options?.method === "PATCH") {
        s.revision++;
        s.desired.brightness = 45;
        s.desired.transition_ms = 150;
      }
      return response(s);
    },
  );
  await c.apply({ kind: "brightness", delta: 5 });
  assert.deepEqual(
    calls.map((c) => c.options?.method),
    ["GET", "GET", "PATCH"],
  );
  assert.deepEqual(JSON.parse(String(calls[2].options?.body)), {
    brightness: 45,
    expected_revision: 7,
    transition_ms: 150,
  });
  assert.equal(calls[2].options?.redirect, "error");
  assert.equal(
    (calls[2].options?.headers as any).Authorization,
    "Bearer private-test-token",
  );
  assert.equal(
    (calls[2].options?.headers as any)["X-Keylight-Actor"],
    "streamdeck",
  );
});
test("white transitions remain disabled unless explicitly advertised", async () => {
  let caps = device;
  const c = new DeviceClient({ url: "http://light.local" }, async () =>
    response(caps),
  );
  const s = fixture();
  s.desired.mode = "white";
  assert.equal(
    (await c.prepare(s, { kind: "brightness", delta: 5 })).transition_ms,
    0,
  );
  caps = {
    ...device,
    capabilities: { ...device.capabilities, white_transitions: true },
  };
  assert.equal(
    (await c.prepare(s, { kind: "brightness", delta: 5 })).transition_ms,
    150,
  );
});
test("missing token refuses write without sending it", async () => {
  let n = 0;
  const c = new DeviceClient({ url: "http://light.local" }, async () => {
    n++;
    return response(fixture());
  });
  await assert.rejects(c.apply({ kind: "power" }), { status: 401 });
  assert.equal(n, 1);
});
for (const code of [401, 403, 409, 423, 500])
  test(`HTTP ${code} never retries or echoes response secrets`, async () => {
    let writes = 0;
    const c = new DeviceClient(
      { url: "http://light.local", token: "secret" },
      async (_u, o) => {
        if (o?.method === "GET") return response(fixture());
        writes++;
        return response({ error: "secret" }, code);
      },
    );
    await assert.rejects(
      c.apply({ kind: "power" }),
      (e: unknown) =>
        e instanceof DeviceError &&
        e.status === code &&
        !e.message.includes("secret"),
    );
    assert.equal(writes, 1);
  });
test("lost mutation response is uncertain and never replayed", async () => {
  let writes = 0;
  const c = new DeviceClient(
    { url: "http://light.local", token: "x" },
    async (_u, o) => {
      if (o?.method === "GET") return response(fixture());
      writes++;
      throw Error("network");
    },
  );
  await assert.rejects(c.apply({ kind: "power" }), /may have reached/);
  assert.equal(writes, 1);
});
for (const defect of ["revision", "value", "body"])
  test(`bad mutation ${defect} remains unconfirmed`, async () => {
    const s = fixture();
    const c = new DeviceClient(
      { url: "http://light.local", token: "x" },
      async () => {
        if (defect === "body") return new Response("oops");
        const a = fixture();
        a.revision += defect === "revision" ? 0 : 1;
        a.desired.power = defect === "value";
        return response(a);
      },
    );
    await assert.rejects(c.mutate(s, { power: false, expected_revision: 7 }));
  });
test("response bound and strict desired-state validation", async () => {
  const c = new DeviceClient(
    { url: "http://light.local" },
    async () => new Response("x".repeat(65537)),
  );
  await assert.rejects(c.state(), /too much/);
  for (const changes of [
    { revision: -1 },
    { desired: { ...fixture().desired, rgb: { r: 256, g: 0, b: 0 } } },
    { reported: { valid: true, confirmed_fields: [42] } },
  ])
    assert.throws(() => validateState({ ...fixture(), ...changes }));
});
test("lock permits only explicit unlock and Off; brightness and colour never power on", () => {
  const s = fixture();
  s.desired.recording_lock = true;
  assert.equal(patchFor(s, { kind: "power" }).power, false);
  assert.equal(patchFor(s, { kind: "lock" }).recording_lock, false);
  for (const i of [
    { kind: "brightness", delta: 5 },
    { kind: "color", rgb: { r: 0, g: 0, b: 255 } },
    { kind: "hue", degrees: 5 },
  ] as const)
    assert.throws(() => patchFor(s, i), { status: 423 });
  s.desired.recording_lock = false;
  s.desired.power = false;
  assert.equal(patchFor(s, { kind: "brightness", delta: 100 }).brightness, 100);
  assert.equal(patchFor(s, { kind: "brightness", delta: -100 }).brightness, 0);
  assert.equal(
    patchFor(s, { kind: "color", rgb: { r: 0, g: 0, b: 255 } }).power,
    undefined,
  );
});
test("hue wraps, preserves value and makes neutral input usable", () => {
  assert.deepEqual(rotateColor({ r: 128, g: 0, b: 0 }, 120), {
    r: 0,
    g: 128,
    b: 0,
  });
  assert.deepEqual(rotateColor({ r: 255, g: 0, b: 0 }, -120), {
    r: 0,
    g: 0,
    b: 255,
  });
  assert.deepEqual(rotateColor({ r: 40, g: 40, b: 40 }, 0), {
    r: 40,
    g: 0,
    b: 0,
  });
  assert.deepEqual(parseColor("#00aAFF"), { r: 0, g: 170, b: 255 });
  assert.throws(() => parseColor("blue"));
});
test("scene recall uses actual stored values with CAS, never unconditional activate", async () => {
  const calls: string[] = [];
  const s = fixture(),
    scene = { ...s.desired, brightness: 23, recording_lock: false };
  const c = new DeviceClient(
    { url: "http://light.local", token: "x" },
    async (u, o) => {
      calls.push(String(u));
      if (String(u).endsWith("/device")) return response(device);
      if (String(u).endsWith("/scenes"))
        return response({ scenes: [{ id: 2, state: scene }] });
      if (o?.method === "PATCH")
        return response({ ...s, revision: 8, desired: scene });
      return response(s);
    },
  );
  await c.apply({ kind: "scene", id: 2 });
  assert.equal(calls.at(-1), "http://light.local/api/v1/state");
  assert(!calls.some((s) => s.includes("activate")));
  await assert.rejects(c.prepare(s, { kind: "scene", id: 3 }), /not saved/);
});
test("unsupported scenes and colour stop before mutation", async () => {
  const c = new DeviceClient({ url: "http://light.local" }, async () =>
    response({ ...device, capabilities: {} }),
  );
  await assert.rejects(
    c.prepare(fixture(), { kind: "scene", id: 2 }),
    /does not support/,
  );
  await assert.rejects(
    c.prepare(fixture(), { kind: "color", rgb: { r: 0, g: 0, b: 0 } }),
    /does not support/,
  );
});
test("labels require idle matching readback and tolerate RGB member order", () => {
  const s = fixture();
  assert.equal(stateLabel("brightness", s), "40%");
  s.desired.brightness = 45;
  assert.equal(stateLabel("brightness", s), "45% *");
  s.reported.rgb = { b: 0, g: 0, r: 255 };
  assert.equal(stateLabel("color", s), "#FF0000");
  s.operation.status = "pending";
  assert.equal(stateLabel("color", s), "#FF0000 *");
  s.operation.status = "error";
  assert.equal(stateLabel("power", s), "Check light");
});

test("accepted controller error stops continuation instead of declaring success", async () => {
  const state = fixture();
  const accepted = fixture();
  accepted.revision++;
  accepted.desired.power = false;
  accepted.operation.status = "error";
  const client = new DeviceClient(
    { url: "http://light.local", token: "x" },
    async () => response(accepted),
  );
  await assert.rejects(
    client.mutate(state, { power: false, expected_revision: 7 }),
    /could not apply/,
  );
  state.desired.recording_lock = true;
  assert.equal(stateLabel("scene", state, 2), "Scene 2\nLocked");
});
