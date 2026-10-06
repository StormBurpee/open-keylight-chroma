export type Config = {
  url?: string;
  token?: string;
  step?: number;
  scene?: number;
};
export type State = {
  revision: number;
  desired: { power: boolean; brightness: number; recording_lock: boolean };
  reported: {
    valid: boolean;
    confirmed_fields: string[];
    power?: boolean;
    brightness?: number;
  };
  operation: { status: "idle" | "pending" | "error"; error?: string | null };
};
export type Device = {
  api_version: number;
  name: string;
  capabilities: { scenes?: boolean };
};
export type Intent =
  | { kind: "power" }
  | { kind: "lock" }
  | { kind: "brightness"; delta: number }
  | { kind: "scene"; id: number };
export class DeviceError extends Error {
  constructor(
    message: string,
    public readonly status = 0,
  ) {
    super(message);
  }
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
export function validateState(value: unknown): State {
  const s = value as State;
  if (
    !s ||
    !Number.isSafeInteger(s.revision) ||
    s.revision < 0 ||
    typeof s.desired?.power !== "boolean" ||
    typeof s.desired.recording_lock !== "boolean" ||
    !Number.isInteger(s.desired.brightness) ||
    s.desired.brightness < 0 ||
    s.desired.brightness > 100 ||
    typeof s.reported?.valid !== "boolean" ||
    !Array.isArray(s.reported.confirmed_fields) ||
    !s.reported.confirmed_fields.every((x) => typeof x === "string") ||
    !["idle", "pending", "error"].includes(s.operation?.status)
  )
    throw new DeviceError("The light returned an invalid state.");
  return s;
}
export function patchFor(
  state: State,
  intent: Exclude<Intent, { kind: "scene" }>,
) {
  let patch: { power?: boolean; brightness?: number; recording_lock?: boolean };
  if (intent.kind === "power") patch = { power: !state.desired.power };
  else if (intent.kind === "lock")
    patch = { recording_lock: !state.desired.recording_lock };
  else {
    if (!Number.isSafeInteger(intent.delta))
      throw new DeviceError("Brightness step must be an integer.");
    patch = {
      brightness: Math.max(
        0,
        Math.min(100, state.desired.brightness + intent.delta),
      ),
    };
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
/** Fixed-origin client: no redirects, no mutation retry, no credentials in diagnostics. */
export class DeviceClient {
  readonly origin: string;
  constructor(
    private config: Config,
    private fetcher: typeof fetch = fetch,
  ) {
    this.origin = deviceOrigin(config.url ?? "");
  }
  async request<T>(method: string, path: string, body?: unknown): Promise<T> {
    if (!/^\/(device|state|scenes\/[1-8]\/activate)$/.test(path))
      throw new DeviceError("Unsupported API route.");
    if (method !== "GET" && !this.config.token?.trim())
      throw new DeviceError(
        "Enter a device token in this action’s settings.",
        401,
      );
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), 8000);
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
      });
      let result: unknown;
      try {
        result = await response.json();
      } catch {
        throw new DeviceError(
          "The light returned unreadable JSON.",
          response.status,
        );
      }
      if (!response.ok) {
        const detail =
          typeof result === "object" && result && "error" in result
            ? String(result.error)
            : `HTTP ${response.status}`;
        throw new DeviceError(
          response.status === 409
            ? "State changed elsewhere. Press again after checking the light."
            : detail,
          response.status,
        );
      }
      return result as T;
    } catch (error) {
      if (error instanceof DeviceError) throw error;
      throw new DeviceError(
        method === "GET"
          ? "Light unreachable. Check its address and connection."
          : "Request unconfirmed; it may have reached the light. Check its state before pressing again.",
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
  async apply(intent: Intent): Promise<State> {
    const state = await this.state();
    if (intent.kind === "scene") {
      if (!Number.isInteger(intent.id) || intent.id < 1 || intent.id > 8)
        throw new DeviceError("Choose scene 1–8.");
      if (state.desired.recording_lock)
        throw new DeviceError("Recording lock is on. Unlock separately.", 423);
      if (!(await this.device()).capabilities.scenes)
        throw new DeviceError("This light does not support scenes.");
      return validateState(
        await this.request("POST", `/scenes/${intent.id}/activate`, {}),
      );
    }
    return validateState(
      await this.request("PATCH", "/state", patchFor(state, intent)),
    );
  }
}
/** One operation per device across every key/dial. A second press is never silently queued. */
export class DeviceGate {
  private active = new Set<string>();
  private generations = new Map<string, number>();
  busy(origin: string) {
    return this.active.has(origin);
  }
  generation(origin: string) {
    return this.generations.get(origin) ?? 0;
  }
  async run<T>(origin: string, fn: () => Promise<T>): Promise<T> {
    if (this.active.has(origin))
      throw new DeviceError(
        "Another control is still running. Try again after its readback.",
        429,
      );
    this.active.add(origin);
    this.generations.set(origin, this.generation(origin) + 1);
    try {
      return await fn();
    } finally {
      this.active.delete(origin);
    }
  }
}
export function stateLabel(kind: string, state: State, scene = 1): string {
  if (state.operation.status === "error") return "Check light";
  if (kind === "lock")
    return state.desired.recording_lock ? "LOCKED" : "Unlocked";
  if (kind === "scene") return `Scene ${scene}\nReady`;
  const field = kind === "power" ? "power" : "brightness";
  const confirmed =
    state.operation.status === "idle" &&
    state.reported.valid &&
    state.reported.confirmed_fields.includes(field) &&
    state.reported[field] === state.desired[field];
  const label =
    field === "power"
      ? state.desired.power
        ? "ON"
        : "OFF"
      : `${state.desired.brightness}%`;
  return label + (confirmed ? "" : " *");
}
export function errorLabel(error: unknown): string {
  if (error instanceof DeviceError) {
    if (error.status === 401 || error.status === 403) return "Token needed";
    if (error.status === 423) return "Locked";
    if (error.status === 409) return "Changed";
    if (error.status === 429) return "Busy";
  }
  return "Check light";
}
