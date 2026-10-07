import { sha256 as digest256 } from "@noble/hashes/sha2.js";
export type RGB = { r: number; g: number; b: number };
export type Output = {
  power: boolean;
  mode: "white" | "color";
  brightness: number;
  temperature_k: number;
  rgb: RGB;
  transition_ms: number;
  effect: string;
  recording_lock: boolean;
};
export type LightState = {
  revision: number;
  desired: Output;
  reported: Partial<Output> & { valid: boolean; confirmed_fields: string[] };
  operation: { status: "idle" | "pending" | "error"; error?: string | null };
  last_actor: string;
};
export type Device = {
  id: string;
  name: string;
  model: string;
  firmware: string;
  api_version: 1;
  trial_pending?: boolean;
  uptime_ms: number;
  network: { connected: boolean; rssi: number; ip: string };
  controller: {
    connected: boolean;
    version: string;
    backend?: "unknown" | "legacy" | "original";
    status?: "starting" | "ready" | "diagnostic" | "unsupported" | "fault";
    ready?: boolean;
    part_id?: number;
    trial_confirmed?: boolean;
    last_health_ms?: number;
  };
  capabilities: {
    white?: boolean;
    color?: boolean;
    transitions?: boolean;
    white_transitions?: boolean;
    effects?: boolean;
    scenes?: boolean;
    settings?: boolean;
    ota?: boolean;
    controller_ota?: boolean;
    effect_names?: string[];
  };
};
export type Scene = { id: number; name: string; state: Output };
export type PairedClient = { id: string; label: string };
export type HistoryEntry = {
  sequence: number;
  uptime_ms: number;
  actor: string;
  event: string;
  detail: string;
};
export type Settings = {
  name: string;
  role: "key" | "fill" | "background" | "other";
  mqtt: { enabled: boolean; uri: string; username: string; connected: boolean };
  button: { single: string; double: string; hold: string };
};
export class ApiError extends Error {
  constructor(
    message: string,
    public status = 0,
  ) {
    super(message);
    this.name = "ApiError";
  }
}
export interface Transport {
  readonly demo: boolean;
  token: string;
  request<T>(
    method: string,
    path: string,
    body?: unknown,
    headers?: Record<string, string>,
  ): Promise<T>;
}
export function assertState(value: LightState): void {
  const d = value?.desired,
    r = value?.reported;
  const inRange = (n: unknown, lo: number, hi: number) =>
    typeof n === "number" && Number.isInteger(n) && n >= lo && n <= hi;
  if (
    !Number.isSafeInteger(value?.revision) ||
    value.revision < 0 ||
    !d ||
    typeof d.power !== "boolean" ||
    !["white", "color"].includes(d.mode) ||
    !inRange(d.brightness, 0, 100) ||
    !inRange(d.temperature_k, 3000, 7000) ||
    !d.rgb ||
    ![d.rgb.r, d.rgb.g, d.rgb.b].every((n) => inRange(n, 0, 255)) ||
    !inRange(d.transition_ms, 0, 10000) ||
    typeof d.effect !== "string" ||
    typeof d.recording_lock !== "boolean" ||
    !r ||
    typeof r.valid !== "boolean" ||
    !Array.isArray(r.confirmed_fields) ||
    !r.confirmed_fields.every((field) => typeof field === "string") ||
    !["idle", "pending", "error"].includes(value.operation?.status)
  )
    throw new ApiError("Incomplete or invalid device state.");
}

export class HttpTransport implements Transport {
  readonly demo = false;
  token = "";
  constructor(private fetcher: typeof fetch = fetch) {}
  async request<T>(
    method: string,
    path: string,
    body?: unknown,
    extra: Record<string, string> = {},
  ): Promise<T> {
    if (
      !/^\/[a-z]+(?:\/[1-8](?:\/activate)?)?$/.test(path) &&
      !/^\/clients\/[0-9a-f]{16}$/.test(path) &&
      path !== "/controller/update"
    )
      throw new ApiError("Invalid API path");
    const controller = new AbortController(),
      timeout = setTimeout(
        () => controller.abort(),
        path === "/update"
          ? 120000
          : path === "/controller/update" && method === "POST"
            ? 35000
            : 8000,
      );
    try {
      const headers: Record<string, string> = { ...extra };
      if (this.token) headers.Authorization = `Bearer ${this.token}`;
      if (method !== "GET") headers["X-Keylight-Actor"] = "dashboard";
      const binary = body instanceof ArrayBuffer;
      if (body !== undefined)
        headers["Content-Type"] = binary
          ? "application/octet-stream"
          : "application/json";
      const response = await this.fetcher("/api/v1" + path, {
        method,
        headers,
        body:
          body === undefined ? undefined : binary ? body : JSON.stringify(body),
        signal: controller.signal,
        cache: "no-store",
        credentials: "same-origin",
      });
      let result: unknown;
      try {
        result = await response.json();
      } catch {
        throw new ApiError(
          "The device returned an unreadable response.",
          response.status,
        );
      }
      if (!response.ok)
        throw new ApiError(
          typeof result === "object" && result && "error" in result
            ? String(result.error)
            : `Device returned ${response.status}`,
          response.status,
        );
      return result as T;
    } catch (error) {
      if (error instanceof DOMException && error.name === "AbortError")
        throw new ApiError(
          method === "GET"
            ? "The device did not respond."
            : "Request timed out. It may have reached the light; inspect its state before trying again.",
        );
      throw error;
    } finally {
      clearTimeout(timeout);
    }
  }
}

export type Snapshot = {
  device: Device | null;
  state: LightState | null;
  busy: boolean;
  stale: boolean;
  error: string;
  notice: string;
  lastSync: number;
};
/** Serializes mutations; slider intents coalesce, uncertain writes never replay. */
export class StudioStore {
  snapshot: Snapshot = {
    device: null,
    state: null,
    busy: false,
    stale: true,
    error: "",
    notice: "Connecting to your light…",
    lastSync: 0,
  };
  private listeners = new Set<() => void>();
  private pending: Partial<Output> | null = null;
  private reading = false;
  private writeGeneration = 0;
  constructor(public transport: Transport) {}
  subscribe = (listener: () => void) => {
    this.listeners.add(listener);
    return () => {
      this.listeners.delete(listener);
    };
  };
  getSnapshot = () => this.snapshot;
  private set(patch: Partial<Snapshot>) {
    this.snapshot = { ...this.snapshot, ...patch };
    this.listeners.forEach((fn) => fn());
  }
  async connect() {
    await this.refresh();
  }
  async refresh() {
    if (this.reading || this.snapshot.busy) return;
    this.reading = true;
    const generation = this.writeGeneration;
    try {
      const [deviceResult, stateResult] = await Promise.allSettled([
        this.transport.request<Device>("GET", "/device"),
        this.transport.request<LightState>("GET", "/state"),
      ]);
      if (deviceResult.status === "rejected") throw deviceResult.reason;
      if (stateResult.status === "rejected") throw stateResult.reason;
      const device = deviceResult.value,
        state = stateResult.value;
      if (device.api_version !== 1)
        throw Error("This dashboard needs API version 1.");
      assertState(state);
      if (generation === this.writeGeneration)
        this.set({ device, state, stale: false, lastSync: Date.now() });
    } catch (error) {
      if (generation === this.writeGeneration)
        this.set({ stale: true, error: message(error) });
    } finally {
      this.reading = false;
      if (this.pending && !this.snapshot.busy) void this.drain();
    }
  }
  patch(patch: Partial<Output>) {
    this.pending = { ...this.pending, ...patch };
    if (!this.reading && !this.snapshot.busy) void this.drain();
  }
  private async drain() {
    while (this.pending) {
      const patch = this.pending;
      this.pending = null;
      try {
        await this.write<LightState>(
          "PATCH",
          "/state",
          { ...patch, expected_revision: this.snapshot.state?.revision },
          "Change accepted; waiting for controller report.",
        );
      } catch {
        this.pending = null;
        break;
      }
    }
    await this.refresh();
  }
  async write<T>(
    method: string,
    path: string,
    body?: unknown,
    notice = "Request accepted.",
    headers?: Record<string, string>,
  ): Promise<T> {
    if (this.snapshot.busy)
      throw new ApiError("Another operation is still running.");
    this.writeGeneration++;
    this.set({ busy: true, error: "", notice: "Sending request…" });
    try {
      const result = await this.transport.request<T>(
        method,
        path,
        body,
        headers,
      );
      const state = (
        result && typeof result === "object" ? result : {}
      ) as Partial<LightState>;
      if (state.desired || state.reported) assertState(state as LightState);
      this.set({
        notice,
        ...(state.desired && state.reported
          ? { state: state as LightState }
          : {}),
      });
      return result;
    } catch (error) {
      this.set({
        error: message(error),
        notice:
          error instanceof ApiError && error.status === 409
            ? "State changed elsewhere. Review it before retrying."
            : "Request not confirmed.",
      });
      throw error;
    } finally {
      this.set({ busy: false });
    }
  }
  clearError() {
    this.set({ error: "" });
  }
}
export function message(error: unknown) {
  return error instanceof Error ? error.message : String(error);
}
export function rgbHex(rgb: RGB) {
  return (
    "#" +
    [rgb.r, rgb.g, rgb.b].map((v) => v.toString(16).padStart(2, "0")).join("")
  );
}
export function parseHex(hex: string): RGB | null {
  if (!/^#?[0-9a-f]{6}$/i.test(hex)) return null;
  const value = hex.replace("#", "");
  return {
    r: parseInt(value.slice(0, 2), 16),
    g: parseInt(value.slice(2, 4), 16),
    b: parseInt(value.slice(4), 16),
  };
}
// ESP-hosted plain HTTP has no SubtleCrypto in many browsers. Hash locally in JS.
export async function sha256(data: ArrayBuffer) {
  return Array.from(digest256(new Uint8Array(data)), (v) =>
    v.toString(16).padStart(2, "0"),
  ).join("");
}
