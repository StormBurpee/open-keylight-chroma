import streamDeck, {
  SingletonAction,
  type Action,
  type WillAppearEvent,
  type WillDisappearEvent,
  type DidReceiveSettingsEvent,
  type KeyDownEvent,
  type DialRotateEvent,
  type DialDownEvent,
  type TouchTapEvent,
  type SendToPluginEvent,
} from "@elgato/streamdeck";
import {
  DeviceClient,
  DeviceGate,
  stateLabel,
  errorLabel,
  type Config,
  type Intent,
  type State,
} from "./client.js";
import { DialQueue } from "./dial.js";

type Kind = "power" | "brightness" | "scene" | "lock";
type Entry = {
  action: Action<Config>;
  settings: Config;
  version: number;
  dial?: DialQueue;
  errorUntil: number;
};
const gate = new DeviceGate();
const sharedReads = new Map<string, Promise<State>>();
function read(client: DeviceClient, token: string) {
  const key = client.origin + "\n" + token;
  let reading = sharedReads.get(key);
  if (!reading) {
    reading = client.state();
    sharedReads.set(key, reading);
    void reading.finally(() => sharedReads.delete(key)).catch(() => {});
  }
  return reading;
}
function step(settings: Config) {
  return Number.isInteger(settings.step) &&
    settings.step &&
    Math.abs(settings.step) <= 25
    ? settings.step
    : 5;
}
class Control extends SingletonAction<Config> {
  override readonly manifestId: string;
  private entries = new Map<string, Entry>();
  constructor(private kind: Kind) {
    super();
    this.manifestId = "org.openkeylight.chroma." + kind;
  }
  override async onWillAppear(ev: WillAppearEvent<Config>) {
    this.configure(ev.action, ev.payload.settings);
    await this.poll();
  }
  override onWillDisappear(ev: WillDisappearEvent<Config>) {
    this.entries.get(ev.action.id)?.dial?.cancel();
    this.entries.delete(ev.action.id);
  }
  override async onDidReceiveSettings(ev: DidReceiveSettingsEvent<Config>) {
    this.configure(ev.action, ev.payload.settings);
    await this.poll();
  }
  private configure(action: Action<Config>, settings: Config) {
    this.entries.get(action.id)?.dial?.cancel();
    const entry: Entry = { action, settings, version: 0, errorUntil: 0 };
    if (this.kind === "brightness")
      entry.dial = new DialQueue(
        (delta) => this.execute(entry, { kind: "brightness", delta }),
        (error) => {
          void this.fail(entry, error);
        },
      );
    this.entries.set(action.id, entry);
  }
  private async label(entry: Entry, label: string, value?: number) {
    if (entry.action.isDial())
      await entry.action.setFeedback({
        title: "Open Keylight",
        value: label,
        indicator: value ?? 0,
      });
    else if (entry.action.isKey()) await entry.action.setTitle(label);
  }
  private async display(entry: Entry, state: State) {
    await this.label(
      entry,
      stateLabel(this.kind, state, entry.settings.scene ?? 1),
      state.desired.brightness,
    );
    if (entry.action.isKey() && this.kind === "lock")
      await entry.action.setState(state.desired.recording_lock ? 1 : 0);
  }
  private async fail(entry: Entry, error: unknown) {
    entry.errorUntil = Date.now() + 7000;
    await this.label(entry, errorLabel(error));
    if (entry.action.isKey()) await entry.action.showAlert();
    if (streamDeck.ui.action?.id === entry.action.id)
      await streamDeck.ui.sendToPropertyInspector({
        error: error instanceof Error ? error.message : "Request failed.",
      });
  }
  async poll() {
    await Promise.allSettled(
      Array.from(this.entries.values()).map(async (entry) => {
        if (!entry.settings.url) {
          await this.label(entry, "Set address");
          return;
        }
        if (entry.errorUntil > Date.now()) return;
        const version = entry.version;
        try {
          const client = new DeviceClient(entry.settings);
          if (gate.busy(client.origin)) return;
          const generation = gate.generation(client.origin);
          const state = await read(client, entry.settings.token ?? "");
          if (
            version === entry.version &&
            generation === gate.generation(client.origin) &&
            this.entries.get(entry.action.id) === entry &&
            !gate.busy(client.origin)
          )
            await this.display(entry, state);
        } catch (error) {
          if (version === entry.version)
            await this.label(entry, errorLabel(error));
        }
      }),
    );
  }
  private async execute(entry: Entry, intent: Intent) {
    entry.version++;
    entry.errorUntil = 0;
    const client = new DeviceClient(entry.settings);
    await gate.run(client.origin, async () => {
      await this.label(entry, "Sending…");
      const accepted = await client.apply(intent);
      if (this.entries.get(entry.action.id) !== entry) return;
      if (intent.kind === "scene")
        await this.label(entry, `Scene ${intent.id}\nAccepted`);
      else await this.display(entry, accepted);
      // Read once; this never resends a mutation, and may still honestly show pending.
      await new Promise((resolve) => setTimeout(resolve, 300));
      const current = await client.state();
      if (this.entries.get(entry.action.id) === entry)
        await this.display(entry, current);
    });
  }
  override async onKeyDown(ev: KeyDownEvent<Config>) {
    const entry = this.entries.get(ev.action.id);
    if (!entry) return;
    const intent: Intent =
      this.kind === "brightness"
        ? { kind: "brightness", delta: step(entry.settings) }
        : this.kind === "scene"
          ? { kind: "scene", id: entry.settings.scene ?? 1 }
          : { kind: this.kind };
    try {
      await this.execute(entry, intent);
    } catch (error) {
      await this.fail(entry, error);
    }
  }
  override onDialRotate(ev: DialRotateEvent<Config>) {
    const entry = this.entries.get(ev.action.id);
    if (entry)
      entry.dial?.add(ev.payload.ticks * Math.abs(step(entry.settings)));
  }
  override async onDialDown(ev: DialDownEvent<Config>) {
    await this.dialPower(ev.action.id);
  }
  override async onTouchTap(ev: TouchTapEvent<Config>) {
    await this.dialPower(ev.action.id);
  }
  private async dialPower(id: string) {
    const entry = this.entries.get(id);
    if (!entry) return;
    entry.dial?.cancel();
    try {
      await this.execute(entry, { kind: "power" });
    } catch (error) {
      await this.fail(entry, error);
    }
  }
  override async onSendToPlugin(
    ev: SendToPluginEvent<{ event: string }, Config>,
  ) {
    if (
      typeof ev.payload !== "object" ||
      !ev.payload ||
      !("event" in ev.payload) ||
      ev.payload.event !== "check"
    )
      return;
    try {
      const client = new DeviceClient(await ev.action.getSettings());
      const device = await client.device();
      const state = await client.state();
      if (streamDeck.ui.action?.id === ev.action.id)
        await streamDeck.ui.sendToPropertyInspector({
          ok: true,
          name: device.name,
          revision: state.revision,
          status: state.operation.status,
          authenticated: false,
          message:
            "API reachable. Token authorization is checked when you use a control.",
        });
    } catch (error) {
      if (streamDeck.ui.action?.id === ev.action.id)
        await streamDeck.ui.sendToPropertyInspector({
          error: error instanceof Error ? error.message : "Connection failed.",
        });
    }
  }
}
const controls = (["power", "brightness", "scene", "lock"] as const).map(
  (kind) => new Control(kind),
);
for (const control of controls) streamDeck.actions.registerAction(control);
// Visible actions only. Concurrent reads share a request per configured device/token.
let polling = false;
setInterval(() => {
  if (polling) return;
  polling = true;
  void Promise.allSettled(controls.map((control) => control.poll())).finally(
    () => {
      polling = false;
    },
  );
}, 5000).unref();
await streamDeck.connect();
