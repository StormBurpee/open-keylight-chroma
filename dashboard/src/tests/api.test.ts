import { describe, it, expect, vi } from "vitest";
import {
  HttpTransport,
  ApiError,
  StudioStore,
  parseHex,
  sha256,
  assertState,
  type Transport,
  type LightState,
} from "../api";
import { DemoTransport } from "../demo";
const tick = () => new Promise((resolve) => setTimeout(resolve, 0));
function deferred<T>() {
  let resolve!: (value: T) => void, reject!: (value: unknown) => void;
  const promise = new Promise<T>((a, b) => {
    resolve = a;
    reject = b;
  });
  return { promise, resolve, reject };
}
function fixture() {
  const demo = new DemoTransport();
  return { device: demo.device, state: structuredClone(demo.state) };
}

describe("HTTP transport", () => {
  it("keeps demo encoding changes separate from network configuration", async () => {
    const api = new DemoTransport();
    const before = structuredClone(api.settings);
    await expect(
      api.request("PATCH", "/settings", {
        output_encoding: "linear",
        ssid: "example",
      }),
    ).rejects.toMatchObject({ status: 400 });
    expect(api.settings).toEqual(before);
  });
  it("calls the default browser fetch with its required global receiver", async () => {
    const original = globalThis.fetch;
    const received: unknown[] = [];
    globalThis.fetch = async function (this: unknown, input, options) {
      if (this !== globalThis) throw new TypeError("Illegal invocation");
      received.push([input, options?.method]);
      return new Response('{"connected":true}', { status: 200 });
    };
    try {
      const api = new HttpTransport();
      await expect(api.request("GET", "/device")).resolves.toEqual({
        connected: true,
      });
      expect(received).toEqual([["/api/v1/device", "GET"]]);
    } finally {
      globalThis.fetch = original;
    }
  });
  it("sends same-origin bearer mutations and exact actor metadata", async () => {
    const fetcher = vi
      .fn()
      .mockResolvedValue(new Response("{}", { status: 202 }));
    const api = new HttpTransport(fetcher);
    api.token = "secret";
    await api.request("PATCH", "/state", {
      brightness: 25,
      expected_revision: 2,
    });
    expect(fetcher).toHaveBeenCalledOnce();
    const [url, options] = fetcher.mock.calls[0];
    expect(url).toBe("/api/v1/state");
    expect(options.headers).toMatchObject({
      Authorization: "Bearer secret",
      "X-Keylight-Actor": "dashboard",
      "Content-Type": "application/json",
    });
    expect(JSON.parse(options.body)).toEqual({
      brightness: 25,
      expected_revision: 2,
    });
    expect(options.credentials).toBe("same-origin");
  });
  it("never retries timed-out mutations", async () => {
    const fetcher = vi
      .fn()
      .mockRejectedValue(new DOMException("aborted", "AbortError"));
    const api = new HttpTransport(fetcher);
    await expect(
      api.request("PATCH", "/state", { power: false }),
    ).rejects.toThrow("may have reached");
    expect(fetcher).toHaveBeenCalledOnce();
  });
  it("retains HTTP conflict and lock errors", async () => {
    const api = new HttpTransport(
      vi
        .fn()
        .mockResolvedValue(new Response('{"error":"Locked"}', { status: 423 })),
    );
    await expect(api.request("PATCH", "/state", {})).rejects.toMatchObject({
      status: 423,
      message: "Locked",
    });
  });
  it("cannot send tokens to a different origin or arbitrary path", async () => {
    const fetcher = vi.fn();
    const api = new HttpTransport(fetcher);
    await expect(api.request("GET", "https://example.com")).rejects.toThrow(
      "Invalid API path",
    );
    expect(fetcher).not.toHaveBeenCalled();
  });
  it("uploads binary bytes with their digest, never JSON encoding", async () => {
    const fetcher = vi
      .fn()
      .mockResolvedValue(new Response('{"accepted":true}', { status: 202 }));
    const api = new HttpTransport(fetcher),
      bytes = new Uint8Array([233, 0, 1]).buffer;
    await api.request("POST", "/update", bytes, { "X-SHA256": "digest" });
    expect(fetcher.mock.calls[0][1].body).toBe(bytes);
    expect(fetcher.mock.calls[0][1].headers).toMatchObject({
      "Content-Type": "application/octet-stream",
      "X-SHA256": "digest",
    });
  });
  it("hashes in a plain HTTP context without SubtleCrypto", async () => {
    expect(await sha256(new TextEncoder().encode("abc").buffer)).toBe(
      "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
    );
    expect(await sha256(new ArrayBuffer(0))).toBe(
      "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
    );
  });
  it("maps RGB bytes exactly without hidden gamma or channel swapping", () => {
    expect(parseHex("#ff0020")).toEqual({ r: 255, g: 0, b: 32 });
    expect(parseHex("GG0020")).toBeNull();
    expect(parseHex("fff")).toBeNull();
  });
  it("rejects incomplete and unsafe state before rendering it", () => {
    const { state } = fixture();
    expect(() =>
      assertState({ ...state, operation: undefined } as unknown as LightState),
    ).toThrow("invalid device state");
    expect(() =>
      assertState({
        ...state,
        desired: { ...state.desired, rgb: { r: 999, g: 0, b: 0 } },
      }),
    ).toThrow("invalid device state");
  });
});
describe("state serialization", () => {
  it("refreshes controller readiness without reconnecting or replaying output", async () => {
    const api = new DemoTransport(),
      store = new StudioStore(api);
    await store.connect();
    const revision = store.snapshot.state?.revision;
    api.device.controller = {
      ...api.device.controller,
      ready: false,
      status: "fault",
    };
    await store.refresh();
    expect(store.snapshot.device?.controller).toMatchObject({
      ready: false,
      status: "fault",
    });
    api.device.controller = {
      ...api.device.controller,
      ready: true,
      status: "ready",
    };
    await store.refresh();
    expect(store.snapshot.device?.controller.ready).toBe(true);
    expect(store.snapshot.state?.revision).toBe(revision);
  });

  it("waits for both poll responses before releasing queued controls after a read failure", async () => {
    const data = fixture(),
      gate = deferred<LightState>();
    let delayed = false,
      writes = 0;
    const transport: Transport = {
      demo: false,
      token: "token",
      request: async <T>(method: string, path: string) => {
        if (method !== "GET") {
          writes++;
          return data.state as T;
        }
        if (delayed && path === "/device") throw Error("Identity unavailable");
        if (delayed && path === "/state") return gate.promise as Promise<T>;
        return structuredClone(
          path === "/device" ? data.device : data.state,
        ) as T;
      },
    };
    const store = new StudioStore(transport);
    await store.connect();
    delayed = true;
    const reading = store.refresh();
    await tick();
    store.patch({ brightness: 10 });
    expect(writes).toBe(0);
    delayed = false;
    gate.resolve(data.state);
    await reading;
    await tick();
    expect(writes).toBe(1);
  });

  it("discards a poll begun before a scene mutation instead of rolling its response backward", async () => {
    const data = fixture(),
      gate = deferred<LightState>();
    let delayRead = false;
    const transport: Transport = {
      demo: false,
      token: "",
      request: async <T>(method: string, path: string) => {
        if (method !== "GET")
          return {
            ...data.state,
            revision: 13,
            desired: { ...data.state.desired, brightness: 20 },
          } as T;
        if (path === "/state" && delayRead) return gate.promise as Promise<T>;
        return structuredClone(
          path === "/device" ? data.device : data.state,
        ) as T;
      },
    };
    const store = new StudioStore(transport);
    await store.connect();
    delayRead = true;
    const polling = store.refresh();
    await store.write("POST", "/scenes/1/activate", {});
    gate.resolve(data.state);
    await polling;
    expect(store.snapshot.state?.revision).toBe(13);
    expect(store.snapshot.state?.desired.brightness).toBe(20);
  });
  it("coalesces pending controls and keeps one mutation in flight", async () => {
    const data = fixture(),
      gate = deferred<LightState>(),
      writes: unknown[] = [];
    let active = 0,
      max = 0;
    const transport: Transport = {
      demo: false,
      token: "token",
      request: async <T>(method: string, path: string, body?: unknown) => {
        if (method === "GET")
          return structuredClone(
            path === "/device" ? data.device : data.state,
          ) as T;
        writes.push(body);
        max = Math.max(max, ++active);
        try {
          if (writes.length === 1) await gate.promise;
          data.state = {
            ...data.state,
            revision: data.state.revision + 1,
            desired: { ...data.state.desired, ...(body as object) },
          };
          return structuredClone(data.state) as T;
        } finally {
          active--;
        }
      },
    };
    const store = new StudioStore(transport);
    await store.connect();
    store.patch({ brightness: 20 });
    store.patch({ brightness: 40 });
    store.patch({ brightness: 60 });
    expect(writes).toHaveLength(1);
    gate.resolve(data.state);
    await tick();
    await tick();
    expect(max).toBe(1);
    expect(writes).toHaveLength(2);
    expect(writes[1]).toMatchObject({ brightness: 60, expected_revision: 13 });
  });
  it("drops queued writes after a conflict and refreshes only reads", async () => {
    const data = fixture(),
      gate = deferred<never>();
    let writes = 0,
      reads = 0;
    const transport: Transport = {
      demo: false,
      token: "",
      request: async <T>(method: string, path: string) => {
        if (method === "GET") {
          reads++;
          return structuredClone(
            path === "/device" ? data.device : data.state,
          ) as T;
        }
        writes++;
        return gate.promise;
      },
    };
    const store = new StudioStore(transport);
    await store.connect();
    store.patch({ brightness: 10 });
    store.patch({ brightness: 30 });
    gate.reject(new ApiError("Stale revision", 409));
    await tick();
    expect(writes).toBe(1);
    expect(reads).toBe(4);
    expect(store.snapshot.notice).toContain("Review");
    expect(store.snapshot.error).toBe("Stale revision");
  });
  it("retains last report and visibly marks it stale on disconnection", async () => {
    const data = fixture();
    let fail = false;
    const transport: Transport = {
      demo: false,
      token: "",
      request: async <T>(_method: string, path: string) => {
        if (fail) throw Error("Offline");
        return structuredClone(
          path === "/device" ? data.device : data.state,
        ) as T;
      },
    };
    const store = new StudioStore(transport);
    await store.connect();
    const old = store.snapshot.state;
    fail = true;
    await store.refresh();
    expect(store.snapshot.state).toBe(old);
    expect(store.snapshot.stale).toBe(true);
    expect(store.snapshot.error).toBe("Offline");
  });
  it("does not poll state while a mutation is in flight", async () => {
    const data = fixture(),
      gate = deferred<LightState>();
    let gets = 0;
    const transport: Transport = {
      demo: false,
      token: "",
      request: async <T>(method: string, path: string) => {
        if (method !== "GET") return gate.promise as Promise<T>;
        gets++;
        return structuredClone(
          path === "/device" ? data.device : data.state,
        ) as T;
      },
    };
    const store = new StudioStore(transport);
    await store.connect();
    store.patch({ power: false });
    await store.refresh();
    expect(gets).toBe(2);
    gate.resolve(data.state);
    await tick();
  });
});
describe("isolated demo", () => {
  it("never contacts a device or invents saved scenes", async () => {
    const fetchSpy = vi.spyOn(globalThis, "fetch");
    const demo = new DemoTransport();
    expect(await demo.request("GET", "/scenes")).toEqual({ scenes: [] });
    await demo.request("PATCH", "/state", { brightness: 33 });
    expect(fetchSpy).not.toHaveBeenCalled();
    expect(demo.state.desired.brightness).toBe(33);
  });
  it("enforces lock semantics including separate unlock and manual Off", async () => {
    const demo = new DemoTransport();
    await demo.request("PATCH", "/state", { recording_lock: true });
    await expect(
      demo.request("PATCH", "/state", { brightness: 30 }),
    ).rejects.toMatchObject({ status: 423 });
    await expect(
      demo.request("PATCH", "/state", {
        recording_lock: false,
        brightness: 30,
      }),
    ).rejects.toMatchObject({ status: 423 });
    await demo.request("PATCH", "/state", { power: false });
    await demo.request("PATCH", "/state", { recording_lock: false });
    expect(demo.state.desired.recording_lock).toBe(false);
  });
});
