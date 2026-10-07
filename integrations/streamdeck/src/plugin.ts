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
  stateLabel,
  errorLabel,
  parseColor,
  hue,
  type Config,
  type Intent,
  type State,
} from "./client.js";
import { DeviceControl } from "./control.js";
type Kind = "power" | "brightness" | "color" | "scene" | "lock";
type Entry = {
  action: Action<Config>;
  settings: Config;
  kind: Kind;
  errorUntil: number;
  nextPoll: number;
  control?: DeviceControl;
  client?: DeviceClient;
};
const entries = new Map<string, Entry>();
const devices = new Map<string, DeviceControl>();
const sharedReads = new Map<string, Promise<State>>();
function current(entry: Entry) {
  return entries.get(entry.action.id) === entry;
}
function safely(work: Promise<unknown>) {
  void work.catch(() => {});
}
async function label(entry: Entry, text: string, value = 0) {
  if (!current(entry)) return;
  if (entry.action.isDial())
    await entry.action.setFeedback({
      title: entry.kind === "color" ? "Colour · hue" : "Brightness",
      value: text,
      indicator: value,
    });
  else if (entry.action.isKey()) await entry.action.setTitle(text);
}
async function display(entry: Entry, state: State) {
  await label(
    entry,
    stateLabel(entry.kind, state, entry.settings.scene ?? 1),
    entry.kind === "color"
      ? hue(state.desired.rgb) / 3.6
      : state.desired.brightness,
  );
  if (current(entry) && entry.action.isKey() && entry.kind === "lock")
    await entry.action.setState(state.desired.recording_lock ? 1 : 0);
}
async function fail(entry: Entry, error: unknown) {
  if (!current(entry)) return;
  entry.errorUntil = Date.now() + 5000;
  await label(entry, errorLabel(error));
  if (entry.action.isKey()) await entry.action.showAlert();
  if (streamDeck.ui.action?.id === entry.action.id)
    await streamDeck.ui.sendToPropertyInspector({
      error: error instanceof Error ? error.message : "Request failed.",
    });
}
function connection(entry: Entry) {
  if (entry.client && entry.control)
    return { client: entry.client, control: entry.control };
  const client = new DeviceClient(entry.settings);
  let control = devices.get(client.origin);
  if (!control) {
    const origin = client.origin;
    control = new DeviceControl({
      changed(state) {
        for (const visible of entries.values())
          if (visible.client?.origin === origin) {
            visible.nextPoll = Date.now() + 180;
            visible.errorUntil = 0;
            safely(display(visible, state));
          }
      },
      pending(owner, state) {
        const visible = owner as Entry;
        if (!current(visible)) return;
        visible.errorUntil = 0;
        safely(state ? display(visible, state) : label(visible, "Sending…"));
      },
      failed(owner, error) {
        safely(fail(owner as Entry, error));
      },
    });
    devices.set(origin, control);
  }
  entry.client = client;
  entry.control = control;
  return { client, control };
}
function read(client: DeviceClient) {
  const key = client.origin + "\n" + (client.config.token ?? "");
  let reading = sharedReads.get(key);
  if (!reading) {
    reading = client.state();
    sharedReads.set(key, reading);
    void reading.finally(() => sharedReads.delete(key)).catch(() => {});
  }
  return reading;
}
function step(config: Config) {
  return Number.isInteger(config.step) &&
    config.step &&
    Math.abs(config.step) <= 25
    ? config.step
    : 5;
}
function hueStep(config: Config) {
  return [1, 5, 10, 15].includes(config.hueStep ?? 5)
    ? (config.hueStep ?? 5)
    : 5;
}
async function poll() {
  await Promise.allSettled(
    [...entries.values()].map(async (entry) => {
      if (!entry.settings.url) {
        await label(entry, "Set address");
        return;
      }
      if (entry.errorUntil > Date.now() || entry.nextPoll > Date.now()) return;
      try {
        const { client, control } = connection(entry);
        if (control.busy) return;
        const generation = control.generation;
        entry.nextPoll = Date.now() + 2000;
        const state = await read(client);
        if (
          current(entry) &&
          generation === control.generation &&
          !control.busy
        ) {
          control.observe(state);
          await display(entry, state);
          if (state.operation.status === "pending")
            entry.nextPoll = Date.now() + 250;
        }
      } catch (error) {
        if (current(entry)) await label(entry, errorLabel(error));
      }
    }),
  );
  // Visible configurations bound the registry. In-flight controllers remain until their job ends.
  for (const [origin, control] of devices)
    if (
      !control.busy &&
      ![...entries.values()].some((e) => e.client?.origin === origin)
    )
      devices.delete(origin);
}
class Control extends SingletonAction<Config> {
  override readonly manifestId: string;
  constructor(private kind: Kind) {
    super();
    this.manifestId = "org.openkeylight.chroma." + kind;
  }
  private configure(action: Action<Config>, settings: Config) {
    const old = entries.get(action.id);
    if (old) old.control?.cancel(old);
    entries.set(action.id, {
      action,
      settings,
      kind: this.kind,
      errorUntil: 0,
      nextPoll: 0,
    });
  }
  override async onWillAppear(ev: WillAppearEvent<Config>) {
    this.configure(ev.action, ev.payload.settings);
    await poll();
  }
  override onWillDisappear(ev: WillDisappearEvent<Config>) {
    const entry = entries.get(ev.action.id);
    if (entry) entry.control?.cancel(entry);
    entries.delete(ev.action.id);
  }
  override async onDidReceiveSettings(ev: DidReceiveSettingsEvent<Config>) {
    this.configure(ev.action, ev.payload.settings);
    await poll();
  }
  private command(entry: Entry, intent: Intent) {
    const { client, control } = connection(entry);
    if (intent.kind === "brightness")
      control.motion(entry, client, "brightness", intent.delta);
    else control.command(entry, client, intent);
  }
  override async onKeyDown(ev: KeyDownEvent<Config>) {
    const entry = entries.get(ev.action.id);
    if (!entry) return;
    try {
      const intent: Intent =
        this.kind === "brightness"
          ? { kind: "brightness", delta: step(entry.settings) }
          : this.kind === "scene"
            ? { kind: "scene", id: entry.settings.scene ?? 1 }
            : this.kind === "color"
              ? {
                  kind: "color",
                  rgb: parseColor(entry.settings.color ?? "#FF8844"),
                }
              : { kind: this.kind };
      this.command(entry, intent);
    } catch (error) {
      await fail(entry, error);
    }
  }
  override async onDialRotate(ev: DialRotateEvent<Config>) {
    const entry = entries.get(ev.action.id);
    if (!entry || !["brightness", "color"].includes(this.kind)) return;
    try {
      const { client, control } = connection(entry);
      control.motion(
        entry,
        client,
        this.kind === "color" ? "hue" : "brightness",
        ev.payload.ticks *
          (this.kind === "color"
            ? hueStep(entry.settings)
            : Math.abs(step(entry.settings))),
      );
    } catch (error) {
      await fail(entry, error);
    }
  }
  override async onDialDown(ev: DialDownEvent<Config>) {
    await this.dialPower(ev.action.id);
  }
  override async onTouchTap(ev: TouchTapEvent<Config>) {
    await this.dialPower(ev.action.id);
  }
  private async dialPower(id: string) {
    const entry = entries.get(id);
    if (!entry) return;
    try {
      this.command(entry, { kind: "power" });
    } catch (error) {
      await fail(entry, error);
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
      const device = await client.device(),
        state = await client.state();
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
for (const kind of ["power", "brightness", "color", "scene", "lock"] as const)
  streamDeck.actions.registerAction(new Control(kind));
let polling = false;
setInterval(() => {
  if (polling) return;
  polling = true;
  void poll()
    .finally(() => {
      polling = false;
    })
    .catch(() => {});
}, 100).unref();
// Stream Deck 7.0 predates SDK 3 settings message identifiers.
streamDeck.settings.useLegacySettingsBehavior = true;
await streamDeck.connect();
