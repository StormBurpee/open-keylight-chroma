import { test } from "node:test";
import assert from "node:assert/strict";
import { DeviceControl } from "../src/control.js";
import { Motion } from "../src/dial.js";
import {
  DeviceClient,
  patchFor,
  type Intent,
  type Patch,
  type State,
} from "../src/client.js";
import { fixture, deferred, until, wait } from "./fixture.js";
class Client extends DeviceClient {
  value = fixture();
  writes: Patch[] = [];
  reads = 0;
  inflight = 0;
  maxInflight = 0;
  onRead?: () => Promise<void>;
  onPrepare?: () => Promise<void>;
  onWrite?: () => Promise<void>;
  constructor() {
    super({ url: "http://light.local", token: "test" });
  }
  override async state() {
    this.reads++;
    await this.onRead?.();
    return structuredClone(this.value);
  }
  override async prepare(state: State, intent: Intent) {
    await this.onPrepare?.();
    if (intent.kind === "scene") throw Error("unused");
    return patchFor(state, intent);
  }
  override async mutate(state: State, patch: Patch) {
    this.inflight++;
    this.maxInflight = Math.max(this.maxInflight, this.inflight);
    this.writes.push(patch);
    try {
      await this.onWrite?.();
      assert.equal(patch.expected_revision, this.value.revision);
      const { expected_revision, ...values } = patch;
      this.value = {
        ...this.value,
        revision: (state.revision + 1) >>> 0,
        desired: { ...this.value.desired, ...values },
      };
      return structuredClone(this.value);
    } finally {
      this.inflight--;
    }
  }
}
function setup() {
  const client = new Client(),
    owner = {},
    other = {},
    failures: unknown[] = [],
    previews: State[] = [],
    accepted: State[] = [];
  const control = new DeviceControl(
    {
      changed: (s) => accepted.push(s),
      failed: (_o, e) => failures.push(e),
      pending: (_o, s) => {
        if (s) previews.push(s);
      },
    },
    2,
    2,
  );
  control.observe(client.value);
  return { client, owner, other, control, failures, previews, accepted };
}
test("constant-space clamp composition matches every step across long reversals", () => {
  let seed = 17;
  for (let start = 0; start <= 100; start++) {
    const motion = new Motion("brightness");
    let expected = start;
    for (let n = 0; n < 1000; n++) {
      seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0;
      const delta = ((seed % 21) - 10) * 5;
      motion.add(delta);
      expected = Math.max(0, Math.min(100, expected + delta));
      const s = fixture();
      s.desired.brightness = start;
      assert.equal(motion.preview(s).desired.brightness, expected);
    }
  }
});
test("burst coalesces one write and immediately previews every signed turn", async () => {
  const x = setup();
  x.client.value.desired.brightness = 95;
  x.control.observe(x.client.value);
  x.control.motion(x.owner, x.client, "brightness", 20);
  x.control.motion(x.owner, x.client, "brightness", -5);
  assert.deepEqual(
    x.previews.map((s) => s.desired.brightness),
    [100, 95],
  );
  assert(x.previews.every((s) => s.operation.status === "pending"));
  await until(() => !x.control.busy);
  assert.deepEqual(
    x.client.writes.map((p) => p.brightness),
    [95],
  );
});
test("turns arriving during a slow state read join before sealing", async () => {
  const x = setup(),
    gate = deferred<void>();
  x.client.onRead = () => gate.promise;
  x.control.motion(x.owner, x.client, "brightness", 5);
  await until(() => x.client.reads === 1);
  for (let i = 0; i < 5; i++)
    x.control.motion(x.owner, x.client, "brightness", 5);
  gate.resolve();
  await until(() => !x.control.busy);
  assert.deepEqual(
    x.client.writes.map((p) => p.brightness),
    [70],
  );
});
test("all ticks during an in-flight write survive in a single bounded continuation", async () => {
  const x = setup(),
    gate = deferred<void>();
  x.client.onWrite = () => gate.promise;
  x.control.motion(x.owner, x.client, "brightness", 5);
  await until(() => x.client.writes.length === 1);
  for (let i = 0; i < 30; i++)
    x.control.motion(x.owner, x.client, "brightness", 5);
  x.control.motion(x.owner, x.client, "brightness", -5);
  gate.resolve();
  await until(() => !x.control.busy);
  assert.deepEqual(
    x.client.writes.map((p) => p.brightness),
    [45, 95],
  );
  assert.deepEqual(
    x.client.writes.map((p) => p.expected_revision),
    [7, 8],
  );
  assert.equal(x.client.maxInflight, 1);
});
test("external revision change stops gesture continuation rather than overwriting", async () => {
  const x = setup(),
    gate = deferred<void>();
  x.client.onWrite = () => gate.promise;
  x.control.motion(x.owner, x.client, "brightness", 5);
  await until(() => x.client.writes.length === 1);
  x.control.motion(x.owner, x.client, "brightness", 10);
  let reads = 0;
  x.client.onRead = async () => {
    if (++reads === 1) x.client.value.revision++;
  };
  gate.resolve();
  await until(() => !x.control.busy);
  assert.equal(x.client.writes.length, 1);
  assert.equal((x.failures[0] as any).status, 409);
});
test("power from another action supersedes unsent turns and follows one active write", async () => {
  const x = setup(),
    gate = deferred<void>();
  x.client.onWrite = () => gate.promise;
  x.control.motion(x.owner, x.client, "brightness", 5);
  await until(() => x.client.writes.length === 1);
  x.control.motion(x.owner, x.client, "brightness", 50);
  x.control.command(x.other, x.client, { kind: "power" });
  assert.throws(() => x.control.command(x.other, x.client, { kind: "power" }), {
    status: 429,
  });
  assert.throws(() => x.control.motion(x.owner, x.client, "brightness", 5), {
    status: 429,
  });
  gate.resolve();
  await until(() => !x.control.busy);
  assert.deepEqual(
    x.client.writes.map((p) => [p.brightness, p.power]),
    [
      [45, undefined],
      [undefined, false],
    ],
  );
  assert.equal(x.client.maxInflight, 1);
});
test("power arriving during capability read cancels the not-yet-sent brightness", async () => {
  const x = setup(),
    gate = deferred<void>();
  let prepared = false;
  x.client.onPrepare = async () => {
    prepared = true;
    await gate.promise;
  };
  x.control.motion(x.owner, x.client, "brightness", 5);
  await until(() => prepared);
  x.control.command(x.other, x.client, { kind: "power" });
  gate.resolve();
  await until(() => !x.control.busy);
  assert.equal(x.client.writes.length, 1);
  assert.equal(x.client.writes[0].power, false);
});
for (const where of ["read", "prepare", "write"])
  test(`disappearance/settings cancellation during ${where} never sends a stale follow-up`, async () => {
    const x = setup(),
      gate = deferred<void>();
    let entered = false;
    const blocked = async () => {
      entered = true;
      await gate.promise;
    };
    if (where === "read") x.client.onRead = blocked;
    else if (where === "prepare") x.client.onPrepare = blocked;
    else x.client.onWrite = blocked;
    x.control.motion(x.owner, x.client, "brightness", 5);
    await until(() => entered);
    x.control.motion(x.owner, x.client, "brightness", 10);
    x.control.cancel(x.owner);
    gate.resolve();
    await until(() => !x.control.busy);
    assert.equal(x.client.writes.length, where === "write" ? 1 : 0);
  });
test("uncertain response drops accumulated work and queued power without retry", async () => {
  const x = setup(),
    gate = deferred<void>();
  x.client.onWrite = () => gate.promise;
  x.control.motion(x.owner, x.client, "brightness", 5);
  await until(() => x.client.writes.length === 1);
  x.control.command(x.other, x.client, { kind: "power" });
  gate.reject(Error("lost response"));
  await until(() => !x.control.busy);
  await wait(20);
  assert.equal(x.client.writes.length, 1);
  assert.equal(x.failures.length, 2);
});
test("conflicting dials cannot silently replace each other's intent", async () => {
  const x = setup();
  x.control.motion(x.owner, x.client, "brightness", 5);
  assert.throws(() => x.control.motion(x.other, x.client, "hue", 5), {
    status: 429,
  });
  x.control.cancel(x.owner);
  await wait();
  assert.equal(x.client.writes.length, 0);
});
test("hue input remains bounded, cyclic and does not turn the light on", async () => {
  const x = setup();
  x.client.value.desired.power = false;
  x.control.motion(x.owner, x.client, "hue", 120);
  x.control.motion(x.owner, x.client, "hue", 36000);
  await until(() => !x.control.busy);
  assert.deepEqual(x.client.writes[0].rgb, { r: 0, g: 255, b: 0 });
  assert.equal(x.client.writes[0].power, undefined);
});

test("Kelvin clamp composition preserves every step and reversal across both temperature limits", () => {
  let seed = 29;
  for (let start = 3000; start <= 7000; start += 50) {
    const motion = new Motion("temperature");
    let expected = start;
    for (let n = 0; n < 1000; n++) {
      seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0;
      const delta = ((seed % 41) - 20) * 250;
      motion.add(delta);
      expected = Math.max(3000, Math.min(7000, expected + delta));
      const s = fixture();
      s.desired.temperature_k = start;
      const preview = motion.preview(s);
      assert.equal(preview.desired.temperature_k, expected);
      assert.equal(preview.desired.mode, "white");
      assert.equal(preview.desired.brightness, s.desired.brightness);
      assert.equal(preview.desired.power, s.desired.power);
    }
  }
});
test("temperature burst during an active write keeps reversal and CAS without changing brightness or power", async () => {
  const x = setup(),
    gate = deferred<void>();
  x.client.value.desired.temperature_k = 6900;
  x.client.value.desired.power = false;
  x.client.onWrite = () => gate.promise;
  x.control.motion(x.owner, x.client, "temperature", 100);
  await until(() => x.client.writes.length === 1);
  x.control.motion(x.owner, x.client, "temperature", 1000);
  x.control.motion(x.owner, x.client, "temperature", -100);
  assert.equal(x.previews.at(-1)?.desired.temperature_k, 6900);
  gate.resolve();
  await until(() => !x.control.busy);
  assert.deepEqual(
    x.client.writes.map((p) => p.temperature_k),
    [7000, 6900],
  );
  assert.deepEqual(
    x.client.writes.map((p) => p.expected_revision),
    [7, 8],
  );
  assert(
    x.client.writes.every(
      (p) =>
        p.mode === "white" &&
        p.effect === "none" &&
        p.transition_ms === 0 &&
        p.power === undefined &&
        p.brightness === undefined,
    ),
  );
  assert.equal(x.client.maxInflight, 1);
});
test("temperature continuation stops on an external revision, and power supersedes pending Kelvin work", async () => {
  const x = setup(),
    gate = deferred<void>();
  x.client.onWrite = () => gate.promise;
  x.control.motion(x.owner, x.client, "temperature", 100);
  await until(() => x.client.writes.length === 1);
  x.control.motion(x.owner, x.client, "temperature", 100);
  x.client.onRead = async () => {
    x.client.value.revision++;
  };
  gate.resolve();
  await until(() => !x.control.busy);
  assert.equal(x.client.writes.length, 1);
  assert.equal((x.failures[0] as any).status, 409);
  const y = setup(),
    held = deferred<void>();
  y.client.onWrite = () => held.promise;
  y.control.motion(y.owner, y.client, "temperature", 100);
  await until(() => y.client.writes.length === 1);
  y.control.motion(y.owner, y.client, "temperature", 500);
  y.control.command(y.other, y.client, { kind: "power" });
  held.resolve();
  await until(() => !y.control.busy);
  assert.deepEqual(
    y.client.writes.map((p) => [p.temperature_k, p.power]),
    [
      [4600, undefined],
      [undefined, false],
    ],
  );
});
