import {
  ApiError,
  type Transport,
  type Device,
  type LightState,
  type Output,
  type Scene,
  type Settings,
  type HistoryEntry,
  type PairedClient,
} from "./api";
/** Deliberate preview sandbox. No fetch, device connection, or persistent writes. */
export class DemoTransport implements Transport {
  readonly demo = true;
  token = "demo";
  device: Device = {
    id: "preview-01",
    name: "Studio key",
    model: "Open Keylight Chroma",
    firmware: "0.1.0-preview",
    api_version: 1,
    uptime_ms: 86453000,
    network: { connected: true, rssi: -48, ip: "Preview only" },
    controller: {
      connected: true,
      version: "Simulated",
      backend: "original",
      status: "ready",
      ready: true,
      part_id: 0,
      trial_confirmed: true,
      last_health_ms: 86453000,
    },
    capabilities: {
      white: true,
      color: true,
      transitions: true,
      effects: true,
      scenes: true,
      settings: true,
      ota: false,
      effect_names: ["none", "aurora", "breathe"],
    },
  };
  state: LightState = {
    revision: 12,
    desired: {
      power: true,
      mode: "white",
      brightness: 64,
      temperature_k: 4200,
      rgb: { r: 235, g: 172, b: 101 },
      transition_ms: 800,
      effect: "none",
      recording_lock: false,
    },
    reported: {
      power: true,
      mode: "white",
      brightness: 64,
      temperature_k: 4200,
      valid: true,
      confirmed_fields: ["power", "mode", "brightness", "temperature_k"],
    },
    operation: { status: "idle" },
    last_actor: "dashboard",
  };
  scenes: Scene[] = [];
  clients: PairedClient[] = [
    { id: "aaaaaaaaaaaaaaaa", label: "Preview browser" },
  ];
  settings: Settings = {
    name: "Studio key",
    role: "key",
    output_encoding: "srgb",
    mqtt: { enabled: false, uri: "", username: "", connected: false },
    button: { single: "toggle", double: "next_scene", hold: "pair" },
  };
  history: HistoryEntry[] = [
    {
      sequence: 1,
      uptime_ms: 86400000,
      actor: "preview",
      event: "Demo started",
      detail: "Isolated preview. No device is connected.",
    },
  ];
  constructor(showcase = false) {
    if (!showcase) return;
    this.device.name = this.settings.name = "Storm Rim";
    this.state.desired = {
      ...this.state.desired,
      mode: "color",
      brightness: 72,
      rgb: { r: 36, g: 92, b: 255 },
      transition_ms: 1200,
    };
    this.state.reported = {
      ...this.state.desired,
      valid: true,
      confirmed_fields: ["power", "mode", "brightness", "rgb"],
    };
    const base = this.state.desired;
    this.scenes = [
      {
        id: 1,
        name: "Focus",
        state: { ...base, mode: "white", temperature_k: 4200, brightness: 80 },
      },
      { id: 2, name: "Blue hour", state: { ...base } },
      {
        id: 3,
        name: "Ember",
        state: { ...base, brightness: 60, rgb: { r: 255, g: 112, b: 38 } },
      },
      {
        id: 4,
        name: "Afterglow",
        state: { ...base, brightness: 55, rgb: { r: 178, g: 138, b: 255 } },
      },
    ];
  }
  async request<T>(method: string, path: string, body?: unknown): Promise<T> {
    await new Promise((resolve) => setTimeout(resolve, 120));
    let value: unknown;
    if (method === "GET")
      value =
        path === "/device"
          ? this.device
          : path === "/state"
            ? this.state
            : path === "/history"
              ? { entries: this.history }
              : path === "/scenes"
                ? { scenes: this.scenes }
                : path === "/clients"
                  ? { clients: this.clients }
                  : path === "/settings"
                    ? this.settings
                    : null;
    else if (path === "/state" && method === "PATCH") {
      const patch = body as Partial<Output> & { expected_revision?: number };
      if (
        patch.expected_revision !== undefined &&
        patch.expected_revision !== this.state.revision
      )
        throw new ApiError("Revision changed.", 409);
      const fields = Object.keys(patch).filter(
        (k) => k !== "expected_revision",
      );
      if (
        this.state.desired.recording_lock &&
        fields.some(
          (k) =>
            !(k === "power" && patch.power === false) &&
            !(
              k === "recording_lock" &&
              patch.recording_lock === false &&
              fields.length === 1
            ),
        )
      )
        throw new ApiError(
          "Unlock recording mode before changing the light.",
          423,
        );
      const { expected_revision: _, ...output } = patch;
      void _;
      this.state = {
        ...this.state,
        revision: this.state.revision + 1,
        desired: { ...this.state.desired, ...output },
        last_actor: "dashboard",
      };
      this.state.reported = {
        ...this.state.desired,
        valid: true,
        confirmed_fields: ["power", "mode", "brightness", "temperature_k"],
      };
      value = this.state;
      this.history.unshift({
        sequence: this.state.revision,
        uptime_ms: this.device.uptime_ms,
        actor: "dashboard",
        event: "Preview change",
        detail: fields.join(", "),
      });
    } else if (path === "/confirm") {
      this.device.trial_pending = false;
      value = { confirmed: true };
    } else if (path === "/pair") {
      value = { token: "preview-token-not-valid-on-a-device" };
    } else if (path === "/pairing" && method === "POST") {
      value = { pairing_open: true, duration_ms: 180000 };
    } else if (path === "/settings" && method === "PATCH") {
      const {
        ssid: _ssid,
        password: _password,
        ...publicSettings
      } = body as Partial<Settings> & { ssid?: string; password?: string };
      void _ssid;
      void _password;
      if ("output_encoding" in publicSettings) {
        if (
          Object.keys(body as Record<string, unknown>).length !== 1 ||
          !["srgb", "linear"].includes(publicSettings.output_encoding ?? "")
        )
          throw new ApiError("Send only output_encoding: srgb or linear", 400);
        if (publicSettings.output_encoding !== this.settings.output_encoding) {
          if (this.state.desired.recording_lock)
            throw new ApiError("Recording lock is active.", 423);
          if (this.device.controller.ready === false)
            throw new ApiError("Controller is unavailable.", 503);
          this.state.revision++;
        }
      }
      this.settings = { ...this.settings, ...publicSettings };
      value = this.settings;
    } else if (/^\/clients\/[0-9a-f]{16}$/.test(path) && method === "DELETE") {
      const id = path.split("/")[2];
      if (!this.clients.some((c) => c.id === id))
        throw new ApiError("Client not found", 404);
      this.clients = this.clients.filter((c) => c.id !== id);
      value = { revoked: true };
    } else if (/^\/scenes\/[1-8]$/.test(path) && method === "PUT") {
      const scene = { ...(body as Omit<Scene, "id">), id: Number(path.at(-1)) };
      this.scenes = this.scenes.filter((s) => s.id !== scene.id).concat(scene);
      value = scene;
    } else if (/^\/scenes\/[1-8]\/activate$/.test(path)) {
      const scene = this.scenes.find(
        (s) => s.id === Number(path.split("/")[2]),
      );
      if (!scene) throw new ApiError("Scene not found", 404);
      return this.request<T>("PATCH", "/state", scene.state);
    } else
      throw new ApiError(
        "This operation is not available in the isolated preview.",
        501,
      );
    if (value === null || value === undefined)
      throw new ApiError("Endpoint not implemented.", 501);
    return structuredClone(value) as T;
  }
}
