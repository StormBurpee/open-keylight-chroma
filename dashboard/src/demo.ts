import {
  ApiError,
  type Transport,
  type Device,
  type LightState,
  type Output,
  type Scene,
  type Settings,
  type HistoryEntry,
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
    controller: { connected: true, version: "Simulated" },
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
  settings: Settings = {
    name: "Studio key",
    role: "key",
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
    } else if (path === "/settings" && method === "PATCH") {
      this.settings = { ...this.settings, ...(body as Partial<Settings>) };
      value = this.settings;
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
