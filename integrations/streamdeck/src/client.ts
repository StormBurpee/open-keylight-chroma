export type RGB = { r: number; g: number; b: number };
export type Config = {
  url?: string;
  token?: string;
  step?: number;
  scene?: number;
  color?: string;
  hueStep?: number;
  fadeMs?: number;
  temperature?: number;
  temperatureStep?: number;
};
export type Output = {
  power: boolean;
  brightness: number;
  recording_lock: boolean;
  mode: "white" | "color";
  rgb: RGB;
  effect: string;
  transition_ms: number;
  temperature_k: number;
};
export type State = {
  revision: number;
  desired: Output;
  reported: Partial<Output> & { valid: boolean; confirmed_fields: string[] };
  operation: { status: "idle" | "pending" | "error"; error?: string | null };
};
export type Device = {
  api_version: number;
  name: string;
  controller?: { ready?: boolean };
  capabilities: {
    scenes?: boolean;
    color?: boolean;
    white?: boolean;
    transitions?: boolean;
    white_transitions?: boolean;
  };
};
export type Intent =
  | { kind: "power" }
  | { kind: "lock" }
  | { kind: "brightness"; delta: number }
  | { kind: "color"; rgb: RGB }
  | { kind: "hue"; degrees: number }
  | { kind: "temperature"; kelvin: number }
  | { kind: "scene"; id: number };
export class DeviceError extends Error {
  constructor(
    message: string,
    public readonly status = 0,
  ) {
    super(message);
  }
}
const integer = (v: unknown, lo: number, hi: number): v is number =>
  Number.isInteger(v) && Number(v) >= lo && Number(v) <= hi;
export function validRgb(v: unknown): v is RGB {
  const rgb = v as RGB;
  return !!rgb && [rgb.r, rgb.g, rgb.b].every((n) => integer(n, 0, 255));
}
export function rgbHex(rgb: RGB) {
  return (
    "#" +
    [rgb.r, rgb.g, rgb.b]
      .map((n) => n.toString(16).padStart(2, "0"))
      .join("")
      .toUpperCase()
  );
}
export function parseColor(hex: string): RGB {
  if (!/^#[0-9a-f]{6}$/i.test(hex))
    throw new DeviceError("Choose a six-digit colour.");
  return {
    r: parseInt(hex.slice(1, 3), 16),
    g: parseInt(hex.slice(3, 5), 16),
    b: parseInt(hex.slice(5, 7), 16),
  };
}
export function hue(rgb: RGB): number {
  const [r, g, b] = [rgb.r, rgb.g, rgb.b].map((n) => n / 255),
    max = Math.max(r, g, b),
    delta = max - Math.min(r, g, b);
  if (!delta) return 0;
  return (
    ((max === r
      ? (g - b) / delta
      : max === g
        ? (b - r) / delta + 2
        : (r - g) / delta + 4) *
      60 +
      360) %
    360
  );
}
/** Hue rotation preserves saturation/value; neutral colours become vivid at their current nonzero value. */
export function rotateColor(rgb: RGB, degrees: number): RGB {
  if (!Number.isSafeInteger(degrees))
    throw new DeviceError("Hue step must be an integer.");
  const max = Math.max(rgb.r, rgb.g, rgb.b) / 255,
    min = Math.min(rgb.r, rgb.g, rgb.b) / 255;
  const h = (((hue(rgb) + degrees) % 360) + 360) % 360,
    v = max || 1,
    s = max > min ? (max - min) / max : 1;
  const c = v * s,
    x = c * (1 - Math.abs(((h / 60) % 2) - 1)),
    m = v - c;
  const channels =
    h < 60
      ? [c, x, 0]
      : h < 120
        ? [x, c, 0]
        : h < 180
          ? [0, c, x]
          : h < 240
            ? [0, x, c]
            : h < 300
              ? [x, 0, c]
              : [c, 0, x];
  const [r, g, b] = channels.map((n) => Math.round((n + m) * 255));
  return { r, g, b };
}
export function deviceOrigin(value: string): string {
  let url: URL;
  try {
    url = new URL(value);
  } catch {
    throw new DeviceError(
      "Enter the light’s full http:// or https:// address.",
    );
  }
  if (
    !["http:", "https:"].includes(url.protocol) ||
    url.username ||
    url.password ||
    url.search ||
    url.hash ||
    url.pathname !== "/"
  )
    throw new DeviceError(
      "Use only the light’s origin, without a path, credentials or query.",
    );
  return url.origin;
}
export function validateOutput(value: unknown): Output {
  const d = value as Output;
  if (
    !d ||
    typeof d.power !== "boolean" ||
    typeof d.recording_lock !== "boolean" ||
    !integer(d.brightness, 0, 100) ||
    !["white", "color"].includes(d.mode) ||
    !validRgb(d.rgb) ||
    !integer(d.transition_ms, 0, 10000) ||
    !integer(d.temperature_k, 3000, 7000) ||
    !["none", "aurora", "breathe"].includes(d.effect)
  )
    throw new DeviceError("The light returned an invalid output state.");
  return d;
}
export function validateState(value: unknown): State {
  const s = value as State;
  if (
    !s ||
    !integer(s.revision, 0, 0xffffffff) ||
    typeof s.reported?.valid !== "boolean" ||
    !Array.isArray(s.reported.confirmed_fields) ||
    !s.reported.confirmed_fields.every((x) => typeof x === "string") ||
    !["idle", "pending", "error"].includes(s.operation?.status)
  )
    throw new DeviceError("The light returned an invalid state.");
  validateOutput(s.desired);
  return s;
}
export function patchFor(
  state: State,
  intent: Exclude<Intent, { kind: "scene" }>,
) {
  let patch: Partial<Output>;
  if (intent.kind === "power") patch = { power: !state.desired.power };
  else if (intent.kind === "lock")
    patch = { recording_lock: !state.desired.recording_lock };
  else if (intent.kind === "brightness") {
    if (!Number.isSafeInteger(intent.delta))
      throw new DeviceError("Brightness step must be an integer.");
    patch = {
      brightness: Math.max(
        0,
        Math.min(100, state.desired.brightness + intent.delta),
      ),
    };
  } else if (intent.kind === "temperature") {
    if (!integer(intent.kelvin, 3000, 7000))
      throw new DeviceError("Choose a white temperature from 3000 to 7000 K.");
    patch = {
      mode: "white",
      temperature_k: intent.kelvin,
      effect: "none",
      transition_ms: 0,
    };
  } else {
    const rgb =
      intent.kind === "hue"
        ? rotateColor(state.desired.rgb, intent.degrees)
        : intent.rgb;
    if (!validRgb(rgb)) throw new DeviceError("Choose a valid colour.");
    patch = { mode: "color", rgb, effect: "none" };
  }
  if (
    state.desired.recording_lock &&
    intent.kind !== "lock" &&
    patch.power !== false
  )
    throw new DeviceError(
      "Recording lock is on. Unlock separately before changing output.",
      423,
    );
  return { ...patch, expected_revision: state.revision };
}
export function fadeFor(config: Config, device: Device, mode: Output["mode"]) {
  const ms = integer(config.fadeMs, 0, 1000) ? config.fadeMs : 150;
  return device.capabilities.transitions &&
    (mode !== "white" || device.capabilities.white_transitions)
    ? ms
    : 0;
}
export type Patch = Partial<Output> & { expected_revision: number };
export function sameValue(a: unknown, b: unknown) {
  return validRgb(a) && validRgb(b)
    ? a.r === b.r && a.g === b.g && a.b === b.b
    : a === b;
}
/** Fixed origin, bounded response, no redirects, no mutation retry or credential diagnostics. */
export class DeviceClient {
  readonly origin: string;
  constructor(
    readonly config: Config,
    private fetcher: typeof fetch = (...args) => globalThis.fetch(...args),
  ) {
    this.origin = deviceOrigin(config.url ?? "");
  }
  async request<T>(method: string, path: string, body?: unknown): Promise<T> {
    if (!/^\/(device|state|scenes)$/.test(path))
      throw new DeviceError("Unsupported API route.");
    if (method !== "GET" && !this.config.token?.trim())
      throw new DeviceError(
        "Enter a device token in this action’s settings.",
        401,
      );
    const controller = new AbortController(),
      timer = setTimeout(() => controller.abort(), 5000);
    try {
      const headers: Record<string, string> = { Accept: "application/json" };
      if (this.config.token)
        headers.Authorization = `Bearer ${this.config.token.trim()}`;
      if (method !== "GET") {
        headers["Content-Type"] = "application/json";
        headers["X-Keylight-Actor"] = "streamdeck";
      }
      const response = await this.fetcher(this.origin + "/api/v1" + path, {
        method,
        headers,
        body: body === undefined ? undefined : JSON.stringify(body),
        signal: controller.signal,
        redirect: "error",
        cache: "no-store",
      });
      const reader = response.body?.getReader();
      let text = "",
        count = 0;
      const decoder = new TextDecoder();
      if (!reader)
        throw new DeviceError(
          "The light returned an empty response.",
          response.status,
        );
      try {
        for (;;) {
          const chunk = await reader.read();
          if (chunk.done) break;
          count += chunk.value.length;
          if (count > 65536)
            throw new DeviceError(
              "The light returned too much data.",
              response.status,
            );
          text += decoder.decode(chunk.value, { stream: true });
        }
        text += decoder.decode();
      } finally {
        await reader.cancel().catch(() => {});
      }
      let result: unknown;
      try {
        result = JSON.parse(text);
      } catch {
        throw new DeviceError(
          "The light returned unreadable JSON.",
          response.status,
        );
      }
      if (!response.ok)
        throw new DeviceError(
          response.status === 409
            ? "State changed elsewhere. Check the light before trying again."
            : response.status === 423
              ? "Recording lock is on. Unlock separately."
              : response.status === 401 || response.status === 403
                ? "Device token was refused."
                : `The light refused the request (HTTP ${response.status}).`,
          response.status,
        );
      return result as T;
    } catch (error) {
      if (error instanceof DeviceError) throw error;
      throw new DeviceError(
        method === "GET"
          ? "Light unreachable. Check its address and connection."
          : "Request unconfirmed; it may have reached the light. Check its state before trying again.",
      );
    } finally {
      clearTimeout(timer);
    }
  }
  async state() {
    return validateState(await this.request("GET", "/state"));
  }
  async device() {
    const d = await this.request<Device>("GET", "/device");
    if (d?.api_version !== 1 || typeof d.name !== "string" || !d.capabilities)
      throw new DeviceError("Open Keylight API v1 is required.");
    return d;
  }
  async prepare(state: State, intent: Intent): Promise<Patch> {
    if (intent.kind === "scene") {
      if (!integer(intent.id, 1, 8)) throw new DeviceError("Choose scene 1–8.");
      if (state.desired.recording_lock)
        throw new DeviceError("Recording lock is on. Unlock separately.", 423);
      if (!(await this.device()).capabilities.scenes)
        throw new DeviceError("This light does not support scenes.");
      const result = await this.request<{
        scenes: { id: number; state: Output }[];
      }>("GET", "/scenes");
      if (!Array.isArray(result?.scenes) || result.scenes.length > 8)
        throw new DeviceError("The light returned invalid scenes.");
      const matches = result.scenes.filter((s) => s.id === intent.id);
      if (matches.length !== 1)
        throw new DeviceError("That scene is not saved on the light.");
      const scene = validateOutput(matches[0].state);
      // Use the saved values with a revision precondition; scene/activate has no CAS field.
      return {
        ...scene,
        recording_lock: false,
        expected_revision: state.revision,
      };
    }
    const patch = patchFor(state, intent);
    if (
      intent.kind === "temperature" &&
      (await this.device()).capabilities.white !== true
    )
      throw new DeviceError("This light does not support white temperature.");
    if (["brightness", "color", "hue"].includes(intent.kind)) {
      const device = await this.device();
      if (intent.kind !== "brightness" && !device.capabilities.color)
        throw new DeviceError("This light does not support colour.");
      patch.transition_ms = fadeFor(
        this.config,
        device,
        patch.mode ?? state.desired.mode,
      );
    }
    return patch;
  }
  async mutate(state: State, patch: Patch): Promise<State> {
    const accepted = validateState(
      await this.request("PATCH", "/state", patch),
    );
    const matches = Object.entries(patch).every(
      ([key, value]) =>
        key === "expected_revision" ||
        sameValue(accepted.desired[key as keyof Output], value),
    );
    if (accepted.revision !== (state.revision + 1) >>> 0 || !matches)
      throw new DeviceError(
        "Request unconfirmed: the returned revision or requested values differ. Check the light.",
      );
    if (accepted.operation.status === "error")
      throw new DeviceError(
        "The controller could not apply the request. Check the light before trying again.",
      );
    return accepted;
  }
  async apply(intent: Intent) {
    const state = await this.state();
    return this.mutate(state, await this.prepare(state, intent));
  }
}
export function stateLabel(kind: string, state: State, scene = 1): string {
  if (state.operation.status === "error") return "Check light";
  if (kind === "lock")
    return state.desired.recording_lock ? "LOCKED" : "Unlocked";
  if (kind === "scene")
    return `Scene ${scene}\n${state.desired.recording_lock ? "Locked" : "Ready"}`;
  const fields: (keyof Output)[] =
    kind === "color"
      ? ["mode", "rgb", "effect"]
      : kind === "temperature"
        ? ["mode", "temperature_k", "effect"]
        : [kind === "power" ? "power" : "brightness"];
  const confirmed =
    state.operation.status === "idle" &&
    state.reported.valid &&
    fields.every(
      (field) =>
        state.reported.confirmed_fields.includes(field) &&
        sameValue(state.reported[field], state.desired[field]),
    );
  const label =
    kind === "color"
      ? state.desired.mode === "color"
        ? rgbHex(state.desired.rgb)
        : "White"
      : kind === "temperature"
        ? state.desired.mode === "white"
          ? `${state.desired.temperature_k} K`
          : "Colour"
        : kind === "power"
          ? state.desired.power
            ? "ON"
            : "OFF"
          : `${state.desired.brightness}%`;
  return label + (confirmed ? "" : " *");
}
export function errorLabel(error: unknown) {
  if (error instanceof DeviceError) {
    if (error.status === 401 || error.status === 403) return "Token needed";
    if (error.status === 423) return "Locked";
    if (error.status === 409) return "Changed";
    if (error.status === 429) return "Busy";
  }
  return "Check light";
}
