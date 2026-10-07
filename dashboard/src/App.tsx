import {
  useCallback,
  useEffect,
  useRef,
  useState,
  useSyncExternalStore,
} from "react";
import {
  Sun,
  ArrowUpRight,
  Check,
  Wifi,
  SlidersHorizontal,
  Layers,
  Settings2,
  LockKeyhole,
  Unlock,
  RefreshCw,
  Plus,
  ArrowRight,
  Upload,
  KeyRound,
  Activity,
  AlertCircle,
  ChevronRight,
  Radio,
  Copy,
  CheckCheck,
} from "lucide-react";
import { Button } from "@/components/ui/button";
import { Slider } from "@/components/ui/slider";
import { Switch } from "@/components/ui/switch";
import { Tabs, TabsList, TabsTrigger, TabsContent } from "@/components/ui/tabs";
import {
  Dialog,
  DialogContent,
  DialogTitle,
  DialogDescription,
  DialogHeader,
} from "@/components/ui/dialog";
import { Input } from "@/components/ui/input";
import { Label } from "@/components/ui/label";
import { WifiSetup, ClientAccess } from "./SystemAccess";
import { ControllerUpdate } from "./ControllerUpdate";
import { ColourWheel } from "./ColourWheel";
import sceneAtlas from "./assets/scene-atlas.webp";
import {
  HttpTransport,
  StudioStore,
  message,
  rgbHex,
  parseHex,
  sha256,
  type Output,
  type Scene,
  type Settings,
  type HistoryEntry,
  type Transport,
} from "./api";

const initial: Output = {
  power: false,
  mode: "white",
  brightness: 0,
  temperature_k: 4200,
  rgb: { r: 255, g: 170, b: 90 },
  transition_ms: 0,
  effect: "none",
  recording_lock: false,
};
const effectLabel = (name: string) =>
  ({ none: "Still light", aurora: "Aurora", breathe: "Slow breathe" })[name] ||
  name;
const scenePosition = (state: Output) => {
  const { r, g, b } = state.rgb;
  const index =
    state.mode === "white" ? 0 : b > r * 1.7 ? 1 : r > b * 1.3 && r > g ? 2 : 3;
  return `${(index * 100) / 3}% center`;
};
const controllerLabel = (status?: string) =>
  ({
    starting: "Starting",
    ready: "Ready",
    diagnostic: "Diagnostic firmware",
    unsupported: "Unsupported firmware",
    fault: "Controller unavailable",
  })[status || ""] || "Not reported";
const duration = (ms: number) => {
  const h = Math.floor(ms / 3600000),
    m = Math.floor(ms / 60000) % 60;
  return h ? `${h}h ${m}m` : `${m}m`;
};

export default function App() {
  const [store, setStore] = useState<StudioStore | null>(null),
    [blocked, setBlocked] = useState("");
  useEffect(() => {
    let active = true;
    void (async () => {
      let transport: Transport;
      if (new URLSearchParams(location.search).get("demo") === "1") {
        if (!import.meta.env.DEV) {
          setBlocked(
            "Demo preview is disabled in device builds. Remove ?demo=1 to connect to this device.",
          );
          return;
        }
        const { DemoTransport } = await import("./demo");
        transport = new DemoTransport(true);
      } else {
        transport = new HttpTransport();
        try {
          transport.token = sessionStorage.getItem("keylight-token") || "";
        } catch {
          /* private browsing may deny storage */
        }
      }
      if (active) setStore(new StudioStore(transport));
    })();
    return () => {
      active = false;
    };
  }, []);
  if (blocked)
    return (
      <main className="boot">
        <Sun />
        <h1>Preview is separate.</h1>
        <p>{blocked}</p>
        <a href="/">
          Open the device dashboard <ArrowRight />
        </a>
      </main>
    );
  return store ? (
    <Studio store={store} />
  ) : (
    <main className="boot">
      <Sun className="slow-spin" />
      <p>Opening your studio…</p>
    </main>
  );
}

export function Studio({ store }: { store: StudioStore }) {
  const view = useSyncExternalStore(store.subscribe, store.getSnapshot),
    { device, state, busy, stale } = view;
  const [tab, setTab] = useState("light"),
    [lightPanel, setLightPanel] = useState("white"),
    [draft, setDraft] = useState<Output>(initial),
    [hex, setHex] = useState("#ffaa5a"),
    [hexError, setHexError] = useState("");
  const [authOpen, setAuthOpen] = useState(false),
    [token, setToken] = useState(store.transport.token),
    [tokenInput, setTokenInput] = useState(""),
    [pairLabel, setPairLabel] = useState("Studio browser"),
    [pairBusy, setPairBusy] = useState(false),
    [authError, setAuthError] = useState(""),
    [pairToken, setPairToken] = useState("");
  const [scenes, setScenes] = useState<Scene[]>([]),
    [history, setHistory] = useState<HistoryEntry[]>([]),
    [settings, setSettings] = useState<Settings | null>(null),
    [resourceError, setResourceError] = useState(""),
    [sceneOpen, setSceneOpen] = useState(false),
    [sceneName, setSceneName] = useState(""),
    [sceneError, setSceneError] = useState("");
  const [firmware, setFirmware] = useState<{
      name: string;
      data: ArrayBuffer;
      digest: string;
    } | null>(null),
    [expectedHash, setExpectedHash] = useState(""),
    [uploadError, setUploadError] = useState(""),
    [updating, setUpdating] = useState(false),
    [updateAccepted, setUpdateAccepted] = useState(false),
    [copied, setCopied] = useState(false);
  const lastFade = useRef(800);
  const editing = useRef(false),
    fileRef = useRef<HTMLInputElement>(null);
  const cancelDraft = useCallback(() => {
    editing.current = false;
    setHexError("");
    const current = store.getSnapshot().state;
    if (current) {
      setDraft(current.desired);
      setHex(rgbHex(current.desired.rgb));
    }
  }, [store]);
  useEffect(cancelDraft, [tab, cancelDraft]);
  useEffect(() => {
    void store.connect();
    const timer = setInterval(() => {
      if (document.visibilityState === "visible") void store.refresh();
    }, 1000);
    return () => clearInterval(timer);
  }, [store]);
  useEffect(() => {
    if (state && !editing.current) {
      setDraft(state.desired);
      if (state.desired.transition_ms > 0)
        lastFade.current = state.desired.transition_ms;
      setHex(rgbHex(state.desired.rgb));
    }
  }, [state]);
  useEffect(() => {
    if (state)
      setLightPanel(
        state.desired.effect === "none" ? state.desired.mode : "effects",
      );
  }, [state?.desired.mode, state?.desired.effect]);
  const readResources = async () => {
    setResourceError("");
    try {
      if (
        (tab === "scenes" || tab === "light") &&
        device?.capabilities.scenes
      ) {
        const result = await store.transport.request<{ scenes: Scene[] }>(
          "GET",
          "/scenes",
        );
        setScenes(result.scenes);
      }
      if (tab === "system") {
        const log = await store.transport.request<{ entries: HistoryEntry[] }>(
          "GET",
          "/history",
        );
        setHistory(log.entries);
        if (device?.capabilities.settings)
          setSettings(
            await store.transport.request<Settings>("GET", "/settings"),
          );
      }
    } catch (error) {
      setResourceError(message(error));
    }
  };
  useEffect(() => {
    void readResources();
  }, [
    tab,
    device?.id,
    device?.capabilities.scenes,
    device?.capabilities.settings,
  ]);
  const supported = device?.capabilities || {},
    authorized = !!token || store.transport.demo;
  const controllerBlocked = device?.controller.ready === false;
  const outputDisabled =
    !state ||
    !authorized ||
    stale ||
    controllerBlocked ||
    draft.recording_lock ||
    updating;
  const commit = (patch: Partial<Output>) => {
    editing.current = false;
    store.patch(patch);
  };
  const setField = <K extends keyof Output>(key: K, value: Output[K]) => {
    editing.current = true;
    setDraft((v) => ({ ...v, [key]: value }));
  };
  const saveToken = (value: string) => {
    const clean = value.trim();
    if (!clean || clean.length > 256) {
      setAuthError("Enter a device token of 1–256 characters.");
      return;
    }
    store.transport.token = clean;
    setToken(clean);
    if (!store.transport.demo)
      try {
        sessionStorage.setItem("keylight-token", clean);
      } catch {}
    setAuthError("");
    setAuthOpen(false);
    store.clearError();
    void store.connect();
  };
  const pair = async () => {
    setPairBusy(true);
    setAuthError("");
    try {
      const result = await store.write<{ token: string }>(
        "POST",
        "/pair",
        { label: pairLabel },
        "Pairing token issued.",
      );
      if (!result.token)
        throw Error("Pairing response did not contain a token.");
      setPairToken(result.token);
    } catch (error) {
      setAuthError(message(error));
    } finally {
      setPairBusy(false);
    }
  };
  const logout = () => {
    store.transport.token = "";
    setToken("");
    if (!store.transport.demo)
      try {
        sessionStorage.removeItem("keylight-token");
      } catch {}
    setPairToken("");
    setAuthOpen(false);
  };
  const saveScene = async () => {
    if (!state) return;
    if (
      !sceneName.trim() ||
      new TextEncoder().encode(sceneName.trim()).length > 32
    ) {
      setSceneError("Use a name of 1–32 UTF-8 bytes.");
      return;
    }
    const id = [1, 2, 3, 4, 5, 6, 7, 8].find(
      (i) => !scenes.some((s) => s.id === i),
    );
    if (!id) {
      setSceneError("All eight scene slots are used.");
      return;
    }
    try {
      await store.write(
        "PUT",
        `/scenes/${id}`,
        {
          name: sceneName.trim(),
          state: { ...state.desired, recording_lock: false },
        },
        "Scene saved on the device.",
      );
      setSceneOpen(false);
      setSceneName("");
      await readResources();
    } catch (error) {
      setSceneError(message(error));
    }
  };
  const applyScene = async (scene: Scene) => {
    cancelDraft();
    try {
      await store.write(
        "POST",
        `/scenes/${scene.id}/activate`,
        {},
        "Scene accepted; waiting for controller report.",
      );
      await store.refresh();
    } catch {
      await store.refresh();
    }
  };
  const selectFirmware = async (file?: File) => {
    setFirmware(null);
    setExpectedHash("");
    setUploadError("");
    if (!file) return;
    if (file.size < 24 || file.size > 1572864) {
      setUploadError(
        "Choose an ESP32 application image no larger than 1,572,864 bytes.",
      );
      return;
    }
    try {
      const data = await file.arrayBuffer();
      if (new Uint8Array(data)[0] !== 0xe9)
        throw Error(
          "This file does not have an ESP32 application-image header.",
        );
      setFirmware({ name: file.name, data, digest: await sha256(data) });
    } catch (error) {
      setUploadError(message(error));
    }
  };
  const upload = async () => {
    if (!firmware || expectedHash.trim().toLowerCase() !== firmware.digest)
      return;
    setUpdating(true);
    setUploadError("");
    try {
      const result = await store.write<{
        accepted: boolean;
        rebooting?: boolean;
      }>(
        "POST",
        "/update",
        firmware.data,
        "Update accepted. Waiting for the device to restart.",
        { "X-SHA256": firmware.digest },
      );
      if (!result.accepted)
        throw Error("The device did not confirm update acceptance.");
      setUpdateAccepted(true);
    } catch (error) {
      setUploadError(message(error));
    } finally {
      setUpdating(false);
    }
  };
  const tint =
    draft.mode === "color"
      ? rgbHex(draft.rgb)
      : `rgb(${Math.round(255 - ((draft.temperature_k - 3000) / 4000) * 40)},${Math.round(190 + ((draft.temperature_k - 3000) / 4000) * 45)},${Math.round(110 + ((draft.temperature_k - 3000) / 4000) * 145)})`;
  const report = state?.reported,
    proof = report?.valid ? "Controller report" : "No valid controller report";
  const operation = stale
    ? "Last known state"
    : controllerBlocked
      ? controllerLabel(device?.controller.status)
      : state?.operation.status === "error"
        ? "Controller error"
        : state?.operation.status === "pending"
          ? "Applying your change"
          : busy
            ? "Sending request"
            : report?.valid
              ? "Controller connected"
              : "Waiting for controller";
  return (
    <div className="app-shell">
      {store.transport.demo && (
        <div className="demo-banner">
          <span>
            <Radio size={13} /> ISOLATED DEMO
          </span>
          Preview only · no light connected · changes stay in this page
        </div>
      )}
      <header className="topbar">
        <a
          className="brand"
          href={store.transport.demo ? "/?demo=1" : "/"}
          aria-label="Open Keylight home"
        >
          <span className="brand-mark" />
          <span>OPEN KEYLIGHT</span>
        </a>
        <div className="header-right">
          <span className={"connection " + (stale ? "offline" : "")}>
            <i />
            {stale ? "Device unavailable" : "Local connection"}
          </span>
          <Button
            variant="outline"
            size="sm"
            className="access-button"
            onClick={() => setAuthOpen(true)}
          >
            <KeyRound size={14} />
            {authorized ? "Access ready" : "Connect access"}
          </Button>
        </div>
      </header>
      <main>
        <div className="intro">
          <div>
            <p className="eyebrow">YOUR STUDIO</p>
            <div className="title-line">
              <h1>{device?.name || "Open Keylight"}</h1>
              <span className={"connection " + (stale ? "offline" : "")}>
                <i />
                {store.transport.demo
                  ? "Preview"
                  : stale
                    ? "Offline"
                    : "Connected"}
              </span>
            </div>
          </div>
          <div className="header-controls">
            <Button
              className={"power-button " + (draft.power ? "on" : "")}
              aria-label={draft.power ? "Turn light off" : "Turn light on"}
              aria-pressed={draft.power}
              disabled={
                !state ||
                !authorized ||
                busy ||
                stale ||
                controllerBlocked ||
                (!draft.power && draft.recording_lock)
              }
              onClick={() => commit({ power: !draft.power })}
            >
              <span className="power-track">
                <span />
              </span>
              {draft.power ? "Light on" : "Light off"}
            </Button>
            <div className="header-lock">
              <LockKeyhole size={18} />
              <Label htmlFor="recording-lock">Recording lock</Label>
              <Switch
                id="recording-lock"
                aria-label="Recording lock"
                checked={draft.recording_lock}
                disabled={!authorized || !state || stale || busy}
                onCheckedChange={(recording_lock) => commit({ recording_lock })}
              />
            </div>
          </div>
        </div>
        <Tabs value={tab} onValueChange={setTab} className="main-tabs">
          <div className="tab-bar">
            <TabsList aria-label="Studio sections">
              <TabsTrigger value="light">
                <SlidersHorizontal size={15} />
                Light
              </TabsTrigger>
              <TabsTrigger value="scenes">
                <Layers size={15} />
                Scenes
              </TabsTrigger>
              <TabsTrigger value="system">
                <Settings2 size={15} />
                System
              </TabsTrigger>
            </TabsList>
            <span className="tab-note">
              <LockKeyhole size={13} /> LOCAL · PRIVATE
            </span>
          </div>
          {device?.trial_pending && (
            <div className="notice" role="status">
              <AlertCircle size={17} />
              <div>
                <strong>Trial firmware · check your controls</strong>
                <p>
                  Confirm explicitly after checking the light. Unconfirmed trial
                  firmware returns to the previous application after its trial
                  window.
                </p>
              </div>
              <Button
                variant="outline"
                size="sm"
                disabled={!authorized || busy}
                onClick={() =>
                  void store
                    .write(
                      "POST",
                      "/confirm",
                      {},
                      "Confirmation accepted. Checking trial status.",
                    )
                    .then(() => store.connect())
                    .catch(() => {})
                }
              >
                Confirm this firmware
                <Check size={13} />
              </Button>
            </div>
          )}
          {view.error && (
            <div className="notice error" role="alert">
              <AlertCircle size={17} />
              <div>
                <strong>{view.notice || "Connection needs attention"}</strong>
                <p>{view.error}</p>
              </div>
              <Button
                size="sm"
                variant="ghost"
                onClick={() => {
                  store.clearError();
                  void store.connect();
                }}
              >
                Refresh
                <RefreshCw size={13} />
              </Button>
            </div>
          )}
          {!authorized && (
            <div className="notice">
              <KeyRound size={17} />
              <div>
                <strong>Look around. Connect when you’re ready.</strong>
                <p>
                  Enter a device token or pair with the physical button to
                  change settings.
                </p>
              </div>
              <Button
                size="sm"
                variant="outline"
                onClick={() => setAuthOpen(true)}
              >
                Connect
                <ArrowRight size={13} />
              </Button>
            </div>
          )}
          <TabsContent value="light" className="light-content">
            <div className="light-layout">
              <section
                className={"light-stage " + (!draft.power ? "is-off" : "")}
                style={
                  {
                    "--light": tint,
                    "--level": String(draft.brightness / 100),
                  } as React.CSSProperties
                }
                aria-label="Approximate light preview"
              >
                <div className="light-orb" aria-hidden="true">
                  <div />
                </div>
                <div className="stage-bottom">
                  <p className="eyebrow">
                    {draft.mode === "white" ? "WHITE OUTPUT" : "COLOUR OUTPUT"}
                  </p>
                  <div className="stage-value">
                    {draft.mode === "white" ? (
                      <>
                        {draft.temperature_k.toLocaleString()}
                        <span>K</span>
                      </>
                    ) : (
                      rgbHex(draft.rgb).toUpperCase()
                    )}
                  </div>
                  <p className="stage-channels">
                    {draft.mode === "color"
                      ? `${draft.rgb.r} / ${draft.rgb.g} / ${draft.rgb.b}`
                      : `${draft.brightness}% intensity`}
                  </p>
                </div>
                <span className="preview-note">Approximate colour preview</span>
              </section>
              <div className="control-rail">
                <div
                  className="mode-buttons"
                  role="group"
                  aria-label="Light mode"
                >
                  <Button
                    variant="ghost"
                    aria-pressed={lightPanel === "white"}
                    disabled={outputDisabled || !supported.white}
                    onClick={() => {
                      setLightPanel("white");
                      commit({ mode: "white", effect: "none" });
                    }}
                  >
                    White
                  </Button>
                  <Button
                    variant="ghost"
                    aria-pressed={lightPanel === "color"}
                    disabled={outputDisabled || !supported.color}
                    onClick={() => {
                      setLightPanel("color");
                      commit({ mode: "color", effect: "none" });
                    }}
                  >
                    Colour
                  </Button>
                  <Button
                    variant="ghost"
                    aria-pressed={lightPanel === "effects"}
                    disabled={outputDisabled || !supported.effects}
                    onClick={() => {
                      cancelDraft();
                      setLightPanel("effects");
                    }}
                  >
                    Effects
                  </Button>
                </div>
                <section className="brightness-section">
                  <div className="control-heading">
                    <Label htmlFor="brightness-slider">Intensity</Label>
                    <div className="big-value">
                      {draft.brightness}
                      <span>%</span>
                    </div>
                  </div>
                  <Slider
                    id="brightness-slider"
                    aria-label="Brightness"
                    disabled={outputDisabled}
                    value={[draft.brightness]}
                    min={0}
                    max={100}
                    onValueChange={([v]) => setField("brightness", v)}
                    onValueCommit={([v]) => commit({ brightness: v })}
                  />
                </section>
                <section className="colour-workspace">
                  {lightPanel === "effects" ? (
                    <div className="effect-choices">
                      <p className="eyebrow">MOVEMENT</p>
                      {(supported.effect_names || ["none"]).map((effect) => (
                        <button
                          key={effect}
                          className={
                            "effect-choice " +
                            (draft.effect === effect ? "selected" : "")
                          }
                          disabled={outputDisabled}
                          onClick={() => commit({ mode: "color", effect })}
                        >
                          <span className={"effect-art effect-" + effect} />
                          <span>
                            <strong>{effectLabel(effect)}</strong>
                            <small>
                              {effect === "aurora"
                                ? "A slow drift through colour"
                                : effect === "breathe"
                                  ? "A gentle rise and fall"
                                  : "Hold a constant colour"}
                            </small>
                          </span>
                          {draft.effect === effect ? (
                            <Check size={17} />
                          ) : (
                            <ArrowRight size={17} />
                          )}
                        </button>
                      ))}
                    </div>
                  ) : draft.mode === "white" ? (
                    <div className="temperature-controls">
                      <div className="control-heading">
                        <Label>Temperature</Label>
                        <output>
                          {draft.temperature_k.toLocaleString()} <span>K</span>
                        </output>
                      </div>
                      <Slider
                        aria-label="White temperature"
                        className="temperature-slider"
                        disabled={outputDisabled}
                        min={3000}
                        max={7000}
                        step={50}
                        value={[draft.temperature_k]}
                        onValueChange={([v]) => setField("temperature_k", v)}
                        onValueCommit={([v]) => commit({ temperature_k: v })}
                      />
                      <div className="range-captions">
                        <span>Warm · 3,000 K</span>
                        <span>Cool · 7,000 K</span>
                      </div>
                      <div className="presets">
                        {[
                          [3200, "Evening"],
                          [4200, "Balanced"],
                          [5600, "Daylight"],
                        ].map(([v, label]) => (
                          <button
                            key={v}
                            disabled={outputDisabled}
                            className={
                              draft.temperature_k === v ? "selected" : ""
                            }
                            onClick={() => commit({ temperature_k: Number(v) })}
                          >
                            <i
                              style={{
                                background:
                                  v === 3200
                                    ? "#e7b777"
                                    : v === 4200
                                      ? "#e6d4ac"
                                      : "#d8e1e4",
                              }}
                            />
                            {label}
                          </button>
                        ))}
                      </div>
                    </div>
                  ) : (
                    <div className="rgb-controls">
                      <ColourWheel
                        compact
                        value={draft.rgb}
                        disabled={outputDisabled}
                        onChange={(rgb) => {
                          setField("rgb", rgb);
                          setHex(rgbHex(rgb));
                        }}
                        onCommit={(rgb) => {
                          setHexError("");
                          commit({ rgb });
                        }}
                        onCancel={cancelDraft}
                      />
                      <div className="colour-numbers">
                        <div className="color-entry">
                          <Label htmlFor="hex-color">Hex colour</Label>
                          <div className="hex-field">
                            <Input
                              id="hex-color"
                              value={hex}
                              disabled={outputDisabled}
                              maxLength={7}
                              aria-invalid={!!hexError}
                              onChange={(e) => {
                                editing.current = true;
                                setHex(e.target.value);
                              }}
                              onKeyDown={(e) => {
                                if (e.key === "Enter") {
                                  const rgb = parseHex(hex);
                                  if (rgb) {
                                    setHexError("");
                                    commit({ rgb });
                                  } else
                                    setHexError(
                                      "Enter six hexadecimal digits.",
                                    );
                                }
                              }}
                            />
                            <Button
                              variant="ghost"
                              aria-label="Apply"
                              disabled={outputDisabled}
                              onClick={() => {
                                const rgb = parseHex(hex);
                                if (rgb) {
                                  setHexError("");
                                  commit({ rgb });
                                } else
                                  setHexError("Enter six hexadecimal digits.");
                              }}
                            >
                              <ArrowRight size={18} />
                            </Button>
                          </div>
                        </div>
                        <div className="rgb-numbers">
                          {(["r", "g", "b"] as const).map((channel) => (
                            <label key={channel}>
                              <span>{channel.toUpperCase()}</span>
                              <input
                                type="number"
                                min={0}
                                max={255}
                                step={1}
                                aria-label={`${channel.toUpperCase()} channel`}
                                disabled={outputDisabled}
                                value={draft.rgb[channel]}
                                onChange={(e) => {
                                  const value = e.target.valueAsNumber;
                                  if (
                                    Number.isInteger(value) &&
                                    value >= 0 &&
                                    value <= 255
                                  ) {
                                    const rgb = {
                                      ...draft.rgb,
                                      [channel]: value,
                                    };
                                    setField("rgb", rgb);
                                    setHex(rgbHex(rgb));
                                  }
                                }}
                                onBlur={() => {
                                  if (editing.current)
                                    commit({ rgb: draft.rgb });
                                }}
                                onKeyDown={(e) => {
                                  if (e.key === "Enter")
                                    commit({ rgb: draft.rgb });
                                }}
                              />
                            </label>
                          ))}
                        </div>
                        {hexError && (
                          <p className="inline-error" role="alert">
                            {hexError}
                          </p>
                        )}
                        <div className="color-swatches">
                          {[
                            "#fff2d6",
                            "#ff8c45",
                            "#ff0020",
                            "#245cff",
                            "#80ebd5",
                            "#b28aff",
                          ].map((color) => (
                            <button
                              key={color}
                              aria-label={`Set color ${color}`}
                              style={{ background: color }}
                              disabled={outputDisabled}
                              onClick={() => commit({ rgb: parseHex(color)! })}
                            />
                          ))}
                        </div>
                      </div>
                    </div>
                  )}
                </section>
                <div className="transition-controls">
                  <div className="smooth-control">
                    <Label htmlFor="smooth-transition">Smooth transition</Label>
                    <Switch
                      id="smooth-transition"
                      aria-label="Smooth transition"
                      checked={draft.transition_ms > 0}
                      disabled={
                        outputDisabled ||
                        !supported.transitions ||
                        (draft.mode === "white" &&
                          supported.white_transitions === false)
                      }
                      onCheckedChange={(on) =>
                        commit({ transition_ms: on ? lastFade.current : 0 })
                      }
                    />
                  </div>
                  <div className="fade-control">
                    <div>
                      <Label>Fade duration</Label>
                      <output>
                        {(draft.transition_ms / 1000).toFixed(1)}
                        <span> s</span>
                      </output>
                    </div>
                    <Slider
                      aria-label="Fade duration"
                      min={100}
                      max={10000}
                      step={100}
                      value={[draft.transition_ms || lastFade.current]}
                      disabled={
                        outputDisabled ||
                        !supported.transitions ||
                        !draft.transition_ms ||
                        (draft.mode === "white" &&
                          supported.white_transitions === false)
                      }
                      onValueChange={([v]) => setField("transition_ms", v)}
                      onValueCommit={([v]) => commit({ transition_ms: v })}
                    />
                  </div>
                </div>
                {draft.mode === "white" &&
                  supported.white_transitions === false && (
                    <p className="transition-note">
                      Smooth transitions are available in Colour mode.
                    </p>
                  )}
                {draft.recording_lock && (
                  <p className="transition-note">
                    Recording lock is on. Unlock to make changes; Off remains
                    available.
                  </p>
                )}
                <div className="control-actions">
                  <Button
                    className="primary-button"
                    disabled={
                      !authorized ||
                      !state ||
                      !supported.scenes ||
                      scenes.length >= 8 ||
                      busy
                    }
                    onClick={() => {
                      setSceneOpen(true);
                      setSceneError("");
                    }}
                  >
                    Save as scene <ArrowRight size={16} />
                  </Button>
                </div>
              </div>
            </div>
            <section className="scene-strip" aria-label="Saved scenes">
              <div className="strip-heading">
                <h2>Scenes</h2>
                <Button variant="ghost" onClick={() => setTab("scenes")}>
                  View all <ArrowRight size={15} />
                </Button>
              </div>
              {resourceError && (
                <p className="inline-error" role="alert">
                  {resourceError}
                </p>
              )}
              {scenes.length ? (
                <div className="scene-strip-grid">
                  {scenes.slice(0, 4).map((scene) => (
                    <button
                      className="scene-tile"
                      key={scene.id}
                      disabled={outputDisabled || busy}
                      aria-label={`Activate ${scene.name}`}
                      onClick={() => void applyScene(scene)}
                    >
                      <span
                        className="scene-photo"
                        style={{
                          backgroundImage: `url(${sceneAtlas})`,
                          backgroundPosition: scenePosition(scene.state),
                        }}
                      />
                      <span>
                        {scene.name}
                        <ArrowUpRight size={16} />
                      </span>
                    </button>
                  ))}
                </div>
              ) : (
                <div className="scene-empty-inline">
                  <p>Your saved looks will live here.</p>
                  <span>Set your light, then save your first scene.</span>
                </div>
              )}
            </section>
          </TabsContent>
          <TabsContent value="scenes">
            <div className="section-intro">
              <div>
                <p className="eyebrow">SAVED ON YOUR LIGHT</p>
                <h2>Scenes</h2>
                <p>
                  Save the settings you come back to. Stored on your device.
                </p>
              </div>
              <Button
                className="primary-button"
                disabled={
                  !authorized ||
                  !state ||
                  !supported.scenes ||
                  scenes.length >= 8 ||
                  busy
                }
                onClick={() => {
                  setSceneOpen(true);
                  setSceneError("");
                }}
              >
                <Plus size={16} />
                Save current look
              </Button>
            </div>
            {resourceError && (
              <p className="inline-error" role="alert">
                {resourceError}
              </p>
            )}
            {!supported.scenes ? (
              <Unavailable text="This firmware does not advertise scene support." />
            ) : scenes.length ? (
              <div className="scene-grid">
                {scenes.map((scene) => (
                  <article className="scene-card" key={scene.id}>
                    <div
                      className="scene-art"
                      style={
                        {
                          backgroundImage: `url(${sceneAtlas})`,
                          backgroundPosition: scenePosition(scene.state),
                          "--scene":
                            scene.state.mode === "color"
                              ? rgbHex(scene.state.rgb)
                              : "#e5c494",
                        } as React.CSSProperties
                      }
                    >
                      <span>{String(scene.id).padStart(2, "0")}</span>
                    </div>
                    <div className="scene-info">
                      <h3>{scene.name}</h3>
                      <p>
                        {scene.state.mode === "white"
                          ? `${scene.state.temperature_k.toLocaleString()} K`
                          : rgbHex(scene.state.rgb).toUpperCase()}
                        <span>·</span>
                        {scene.state.brightness}%
                      </p>
                      <Button
                        variant="ghost"
                        aria-label={`Activate ${scene.name}`}
                        disabled={outputDisabled || busy}
                        onClick={() => void applyScene(scene)}
                      >
                        <ArrowUpRight size={20} />
                      </Button>
                    </div>
                  </article>
                ))}
              </div>
            ) : (
              <div className="empty-scenes">
                <Layers size={35} />
                <h3>Your scenes start here.</h3>
                <p>
                  Once you’ve found your look, save it here.
                  <br />
                  Up to eight scenes, entirely on your device.
                </p>
              </div>
            )}
          </TabsContent>
          <TabsContent value="system">
            <div className="section-intro">
              <div>
                <p className="eyebrow">DEVICE & CONNECTIONS</p>
                <h2>System</h2>
                <p>
                  Local connections, honest status, and updates you control.
                </p>
              </div>
              <Button variant="outline" onClick={() => void readResources()}>
                <RefreshCw size={14} />
                Refresh details
              </Button>
            </div>
            {resourceError && (
              <p role="alert" className="inline-error">
                {resourceError}
              </p>
            )}
            <div className="system-grid">
              <section className="system-panel">
                <div className="panel-heading">
                  <Wifi size={19} />
                  <h3>At home on your network</h3>
                </div>
                <dl className="facts">
                  <div>
                    <dt>Address</dt>
                    <dd>{device?.network.ip || "Not reported"}</dd>
                  </div>
                  <div>
                    <dt>Signal</dt>
                    <dd>
                      {device?.network.connected
                        ? `${device.network.rssi} dBm`
                        : "Disconnected"}
                    </dd>
                  </div>
                  <div>
                    <dt>Controller</dt>
                    <dd>
                      {device?.controller.status
                        ? controllerLabel(device.controller.status)
                        : device?.controller.connected
                          ? "Connected"
                          : "Unavailable"}
                    </dd>
                  </div>
                  <div>
                    <dt>Controller firmware</dt>
                    <dd>
                      {device?.controller.backend === "original"
                        ? "Open Keylight"
                        : device?.controller.backend === "legacy"
                          ? "Razer compatibility"
                          : "Not identified"}
                    </dd>
                  </div>
                  <div>
                    <dt>Controller version</dt>
                    <dd>{device?.controller.version || "Not reported"}</dd>
                  </div>
                  <div>
                    <dt>Running for</dt>
                    <dd>{device ? duration(device.uptime_ms) : "—"}</dd>
                  </div>
                </dl>
                {settings && (
                  <form
                    className="settings-form"
                    onSubmit={(e) => {
                      e.preventDefault();
                      void store
                        .write(
                          "PATCH",
                          "/settings",
                          { name: settings.name, role: settings.role },
                          "Device settings saved.",
                        )
                        .then(() => store.connect())
                        .catch(() => {});
                    }}
                  >
                    <Label htmlFor="device-name">Device name</Label>
                    <Input
                      id="device-name"
                      value={settings.name}
                      maxLength={32}
                      onChange={(e) =>
                        setSettings({ ...settings, name: e.target.value })
                      }
                    />
                    <Label htmlFor="device-role">Studio role</Label>
                    <select
                      id="device-role"
                      value={settings.role}
                      onChange={(e) =>
                        setSettings({
                          ...settings,
                          role: e.target.value as Settings["role"],
                        })
                      }
                    >
                      {["key", "fill", "background", "other"].map((role) => (
                        <option key={role}>{role}</option>
                      ))}
                    </select>
                    <Button variant="outline" disabled={!authorized || busy}>
                      Save device settings
                    </Button>
                  </form>
                )}
                {supported.settings && (
                  <WifiSetup store={store} disabled={!authorized || busy} />
                )}
              </section>
              <section className="system-panel">
                <div className="panel-heading">
                  <Layers size={19} />
                  <h3>Part of your setup</h3>
                </div>
                <div className="integration">
                  <span className="integration-icon">
                    <Radio />
                  </span>
                  <div>
                    <h4>Home Assistant</h4>
                    <p>
                      {settings?.mqtt.connected
                        ? "MQTT connected"
                        : settings?.mqtt.enabled
                          ? "MQTT enabled · not connected"
                          : "MQTT discovery · configure your broker"}
                    </p>
                  </div>
                </div>
                {settings && (
                  <MqttForm
                    settings={settings}
                    disabled={!authorized || busy}
                    save={async (mqtt) => {
                      await store.write(
                        "PATCH",
                        "/settings",
                        { mqtt },
                        "MQTT settings saved.",
                      );
                      await readResources();
                    }}
                  />
                )}
                <div className="integration divided">
                  <span className="integration-icon">
                    <SlidersHorizontal />
                  </span>
                  <div>
                    <h4>Stream Deck & automations</h4>
                    <p>
                      Use the authenticated local API. A device token is
                      required.
                    </p>
                  </div>
                </div>
                <code className="api-code">PATCH /api/v1/state</code>
                <div className="button-guide">
                  <h4>Your physical button</h4>
                  {settings ? (
                    Object.entries(settings.button).map(([gesture, action]) => (
                      <div key={gesture}>
                        <span>
                          {gesture === "single"
                            ? "Single press"
                            : gesture === "double"
                              ? "Double press"
                              : "Hold"}
                        </span>
                        <span>{action.replaceAll("_", " ")}</span>
                      </div>
                    ))
                  ) : (
                    <p>Button mapping has not been reported.</p>
                  )}
                </div>
              </section>
              <ClientAccess
                store={store}
                token={token}
                disabled={busy}
                onSelfRevoked={logout}
                onPair={() => setAuthOpen(true)}
              />
              <section className="system-panel firmware-panel">
                <div className="panel-heading">
                  <Upload size={19} />
                  <h3>Made to keep getting better</h3>
                  <span>{device?.firmware || "Version unknown"}</span>
                </div>
                <p className="panel-copy">
                  Upload an application image from your trusted build. Check its
                  release SHA-256 before sending. The device verifies the image
                  and digest before selecting it for boot.
                </p>
                {!supported.ota ? (
                  <Unavailable text="Application updates are not advertised by this firmware." />
                ) : (
                  <>
                    <input
                      ref={fileRef}
                      type="file"
                      accept=".bin,application/octet-stream"
                      aria-label="Firmware application image"
                      className="file-input"
                      disabled={!authorized || updating}
                      onChange={(e) => void selectFirmware(e.target.files?.[0])}
                    />
                    <button
                      className="upload-zone"
                      disabled={!authorized || updating}
                      onClick={() => fileRef.current?.click()}
                    >
                      <Upload size={22} />
                      <strong>
                        {firmware?.name || "Choose an application image"}
                      </strong>
                      <span>
                        {firmware
                          ? `${firmware.data.byteLength.toLocaleString()} bytes · SHA-256 computed`
                          : "ESP32 .bin · up to 1.5 MiB"}
                      </span>
                    </button>
                    {firmware && (
                      <div className="digest-review">
                        <Label>Computed SHA-256</Label>
                        <code>{firmware.digest}</code>
                        <Label htmlFor="expected-hash">
                          Expected SHA-256 from your build or release
                        </Label>
                        <Input
                          id="expected-hash"
                          spellCheck={false}
                          value={expectedHash}
                          onChange={(e) => setExpectedHash(e.target.value)}
                          placeholder="Paste the 64-character digest"
                        />
                        <Button
                          className="primary-button"
                          disabled={
                            !authorized ||
                            busy ||
                            updating ||
                            expectedHash.trim().toLowerCase() !==
                              firmware.digest
                          }
                          onClick={() => void upload()}
                        >
                          <Upload size={14} />
                          {updating ? "Uploading…" : "Verify & update"}
                        </Button>
                        <p>
                          This restarts the light. An accepted upload is not
                          proof of a successful reboot. The application trial
                          needs your explicit confirmation after checking the
                          controls. This does not imply bootloader-level
                          automatic rollback.
                        </p>
                      </div>
                    )}
                  </>
                )}
                {uploadError && (
                  <p className="inline-error" role="alert">
                    {uploadError}
                  </p>
                )}
                {updateAccepted && (
                  <div className="notice">
                    <Check />
                    <p>
                      Image accepted. Wait for restart, then refresh and verify
                      the firmware version.
                    </p>
                  </div>
                )}
              </section>
              {device && (
                <ControllerUpdate
                  store={store}
                  device={device}
                  authorized={authorized}
                  busy={busy}
                />
              )}
              <section className="system-panel history-panel">
                <div className="panel-heading">
                  <Activity size={19} />
                  <h3>The recent story</h3>
                  <span>DEVICE HISTORY</span>
                </div>
                {history.length ? (
                  <ol className="history-list">
                    {history.map((item) => (
                      <li key={item.sequence}>
                        <span className="history-marker" />
                        <div>
                          <strong>{item.event}</strong>
                          <p>{item.detail}</p>
                        </div>
                        <span>
                          {item.actor}
                          <small>at {duration(item.uptime_ms)} uptime</small>
                        </span>
                      </li>
                    ))}
                  </ol>
                ) : (
                  <p className="panel-copy">
                    No history has been reported. Events appear here as the
                    device returns them.
                  </p>
                )}
              </section>
            </div>
          </TabsContent>
        </Tabs>
        <section className="readback" aria-label="Last controller report">
          <div className="readback-status">
            <span className={"status-mark " + (stale ? "stale" : "")}>
              <CheckCheck size={17} />
            </span>
            <div>
              <h3>{operation}</h3>
              <p>
                {stale
                  ? "Displayed values may be out of date."
                  : proof + " · not an optical measurement"}
              </p>
            </div>
          </div>
          <div>
            <span>POWER</span>
            <strong>
              {report?.valid && report.confirmed_fields?.includes("power")
                ? report.power
                  ? "On"
                  : "Off"
                : "Unconfirmed"}
            </strong>
          </div>
          <div>
            <span>BRIGHTNESS</span>
            <strong>
              {report?.valid && report.confirmed_fields?.includes("brightness")
                ? `${report.brightness}%`
                : "Unconfirmed"}
            </strong>
          </div>
          <div>
            <span>{draft.mode === "white" ? "TEMPERATURE" : "RGB"}</span>
            <strong>
              {report?.valid &&
              draft.mode === "white" &&
              report.confirmed_fields?.includes("temperature_k")
                ? `${report.temperature_k?.toLocaleString()} K`
                : report?.valid &&
                    report.confirmed_fields?.includes("rgb") &&
                    report.rgb
                  ? rgbHex(report.rgb).toUpperCase()
                  : "Unconfirmed"}
            </strong>
          </div>
          <div>
            <span>REVISION</span>
            <strong>
              {state ? String(state.revision).padStart(3, "0") : "—"}
            </strong>
          </div>
        </section>
        <footer>
          <span>
            <Sun size={13} /> Open Keylight Chroma
          </span>
          <span>
            {store.transport.demo ? "PREVIEW SANDBOX" : "LOCAL FIRST"}
            <i />
            OPEN KEYLIGHT
          </span>
        </footer>
      </main>
      <Dialog open={authOpen} onOpenChange={setAuthOpen}>
        <DialogContent className="studio-dialog">
          <DialogHeader>
            <div className="dialog-icon">
              <KeyRound />
            </div>
            <DialogTitle>Make it your light.</DialogTitle>
            <DialogDescription>
              Device access stays in this tab. Pairing requires an open pairing
              window.
            </DialogDescription>
          </DialogHeader>
          {authError && (
            <p role="alert" className="inline-error">
              {authError}
            </p>
          )}
          {pairToken ? (
            <>
              <Label>Your new device token</Label>
              <code className="token-value">{pairToken}</code>
              <p className="panel-copy">
                Save it securely. The device returns this token only once.
              </p>
              <Button
                variant="outline"
                onClick={() =>
                  void navigator.clipboard
                    .writeText(pairToken)
                    .then(() => setCopied(true))
                    .catch(() =>
                      setAuthError(
                        "Clipboard unavailable. Select and copy the token manually.",
                      ),
                    )
                }
              >
                {copied ? <Check size={14} /> : <Copy size={14} />}Copy token
              </Button>
              <Button
                className="primary-button"
                onClick={() => saveToken(pairToken)}
              >
                Use this token
                <ArrowRight size={15} />
              </Button>
            </>
          ) : (
            <>
              <Label htmlFor="device-token">Already have a token?</Label>
              <Input
                id="device-token"
                type="password"
                autoComplete="off"
                placeholder="Paste your device token"
                value={tokenInput}
                onChange={(e) => setTokenInput(e.target.value)}
              />
              <Button
                className="primary-button"
                disabled={!tokenInput.trim()}
                onClick={() => saveToken(tokenInput)}
              >
                Connect access
                <ArrowRight size={14} />
              </Button>
              <div className="dialog-divider">OR PAIR AT THE LIGHT</div>
              <p className="panel-copy">
                Open pairing with the physical button or an already trusted
                client, then request access below. The device will explain if
                pairing is unavailable.
              </p>
              <Label htmlFor="pair-label">Name this connection</Label>
              <Input
                id="pair-label"
                value={pairLabel}
                maxLength={32}
                onChange={(e) => setPairLabel(e.target.value)}
              />
              <Button
                variant="outline"
                disabled={pairBusy || busy || !pairLabel.trim()}
                onClick={() => void pair()}
              >
                {pairBusy ? "Requesting…" : "Pair this browser"}
                <ChevronRight size={14} />
              </Button>
              {authorized && (
                <Button variant="ghost" onClick={logout}>
                  <Unlock size={14} />
                  Forget this tab’s token
                </Button>
              )}
            </>
          )}
        </DialogContent>
      </Dialog>
      <Dialog open={sceneOpen} onOpenChange={setSceneOpen}>
        <DialogContent className="studio-dialog">
          <DialogHeader>
            <div className="dialog-icon">
              <Layers />
            </div>
            <DialogTitle>A look worth keeping.</DialogTitle>
            <DialogDescription>
              Save the current desired settings to an empty device slot.
              Recording lock is not saved.
            </DialogDescription>
          </DialogHeader>
          <Label htmlFor="scene-name">Scene name</Label>
          <Input
            id="scene-name"
            autoFocus
            value={sceneName}
            onChange={(e) => setSceneName(e.target.value)}
            placeholder="e.g. Late afternoon"
          />
          {sceneError && (
            <p className="inline-error" role="alert">
              {sceneError}
            </p>
          )}
          <Button
            className="primary-button"
            disabled={busy || !sceneName.trim()}
            onClick={() => void saveScene()}
          >
            Save scene
            <Check size={15} />
          </Button>
        </DialogContent>
      </Dialog>
    </div>
  );
}
function Unavailable({ text }: { text: string }) {
  return (
    <div className="unavailable">
      <AlertCircle size={17} />
      <p>{text}</p>
    </div>
  );
}
function MqttForm({
  settings,
  disabled,
  save,
}: {
  settings: Settings;
  disabled: boolean;
  save: (mqtt: unknown) => Promise<void>;
}) {
  const [enabled, setEnabled] = useState(settings.mqtt.enabled),
    [uri, setUri] = useState(settings.mqtt.uri),
    [username, setUsername] = useState(settings.mqtt.username),
    [password, setPassword] = useState(""),
    [error, setError] = useState("");
  return (
    <form
      className="settings-form"
      onSubmit={(e) => {
        e.preventDefault();
        setError("");
        void save({ enabled, uri, username, ...(password ? { password } : {}) })
          .then(() => setPassword(""))
          .catch((error) => setError(message(error)));
      }}
    >
      <div className="switch-line">
        <Label htmlFor="mqtt-enabled">Enable MQTT</Label>
        <Switch
          id="mqtt-enabled"
          checked={enabled}
          onCheckedChange={setEnabled}
        />
      </div>
      <Label htmlFor="mqtt-uri">Broker URI</Label>
      <Input
        id="mqtt-uri"
        value={uri}
        onChange={(e) => setUri(e.target.value)}
        placeholder="mqtt://homeassistant.local"
      />
      <Label htmlFor="mqtt-user">Username</Label>
      <Input
        id="mqtt-user"
        value={username}
        onChange={(e) => setUsername(e.target.value)}
      />
      <Label htmlFor="mqtt-pass">Password · leave blank to keep</Label>
      <Input
        id="mqtt-pass"
        type="password"
        autoComplete="new-password"
        value={password}
        onChange={(e) => setPassword(e.target.value)}
      />
      {error && (
        <p className="inline-error" role="alert">
          {error}
        </p>
      )}
      <Button variant="outline" disabled={disabled}>
        Save MQTT settings
      </Button>
    </form>
  );
}
