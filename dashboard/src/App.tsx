import { useEffect, useRef, useState, useSyncExternalStore } from "react";
import {
  Sun,
  Moon,
  Power,
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
        transport = new DemoTransport();
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
  const editing = useRef(false),
    fileRef = useRef<HTMLInputElement>(null);
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
      setHex(rgbHex(state.desired.rgb));
    }
  }, [state]);
  const readResources = async () => {
    setResourceError("");
    try {
      if (tab === "scenes" && device?.capabilities.scenes) {
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
          <span className="brand-mark">
            <Sun size={25} />
          </span>
          <span>
            open<span className="brand-light">keylight</span>
            <small>YOUR LIGHT. YOUR CONTROL.</small>
          </span>
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
            <p className="eyebrow">
              THE LOCAL LIGHT STUDIO <span> / </span>{" "}
              {device?.id || "AWAITING DEVICE"}
            </p>
            <h1>
              {device?.name || "Make room for light"}
              <span>.</span>
            </h1>
            <p className="intro-copy">Set the mood. Keep the moment.</p>
          </div>
          <div className="device-tag">
            <span className="tag-line" />
            <div>
              <strong>{device?.model || "Open Keylight"}</strong>
              <span>
                {store.transport.demo
                  ? "Interactive preview"
                  : device
                    ? `Firmware ${device.firmware}`
                    : "Connect to your device to begin"}
              </span>
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
              Made to stay local <ArrowUpRight size={13} />
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
          <TabsContent value="light" className="light-layout">
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
              <div className="stage-top">
                <span className="eyebrow">LIGHT, REIMAGINED</span>
                <span className="quiet-tag">
                  {draft.mode === "white" ? "WHITE LIGHT" : "FULL COLOR"}
                </span>
              </div>
              <div className="lamp-art" aria-hidden="true">
                <div className="light-halo" />
                <div className="lamp-body">
                  <div className="lamp-panel" />
                  <span className="lamp-insignia">OPEN / 01</span>
                </div>
                <div className="lamp-stem" />
                <div className="lamp-base" />
              </div>
              <div className="stage-bottom">
                <div>
                  <p className="stage-caption">
                    {draft.power ? "YOUR SELECTED LOOK" : "A QUIET MOMENT"}
                  </p>
                  <div className="stage-value">
                    {draft.mode === "white" ? (
                      <>
                        {draft.temperature_k.toLocaleString()}
                        <span>K</span>
                      </>
                    ) : (
                      <span className="stage-hex">
                        {rgbHex(draft.rgb).toUpperCase()}
                      </span>
                    )}
                  </div>
                </div>
                <div className="stage-dots">
                  <span />
                  <span />
                  <span />
                </div>
              </div>
              <p className="preview-note">
                A visual preview. Actual colour depends on your light and room.
              </p>
            </section>
            <div className="control-rail">
              <section className="power-row">
                <div>
                  <span className="eyebrow">OUTPUT</span>
                  <h2>
                    {draft.power
                      ? "Let there be light."
                      : "Ready for a little light?"}
                  </h2>
                </div>
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
                  <Power size={21} />
                </Button>
              </section>
              <section className="brightness-section">
                <div className="control-heading">
                  <Label htmlFor="brightness-slider">Brightness</Label>
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
                <div className="range-captions">
                  <Moon size={12} />
                  <span>A little atmosphere</span>
                  <Sun size={15} />
                </div>
              </section>
              <section className="color-section">
                <div
                  className="mode-buttons"
                  role="group"
                  aria-label="Light mode"
                >
                  <Button
                    variant="ghost"
                    aria-pressed={draft.mode === "white"}
                    disabled={outputDisabled || !supported.white}
                    onClick={() => commit({ mode: "white", effect: "none" })}
                  >
                    <Sun size={15} />
                    White
                  </Button>
                  <Button
                    variant="ghost"
                    aria-pressed={draft.mode === "color"}
                    disabled={outputDisabled || !supported.color}
                    onClick={() => commit({ mode: "color", effect: "none" })}
                  >
                    <span className="spectrum-dot" />
                    Color
                  </Button>
                </div>
                {draft.mode === "white" ? (
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
                    <div className="color-entry">
                      <input
                        type="color"
                        aria-label="Choose RGB color"
                        value={rgbHex(draft.rgb)}
                        disabled={outputDisabled}
                        onChange={(e) => {
                          const rgb = parseHex(e.target.value)!;
                          setField("rgb", rgb);
                          setHex(e.target.value);
                        }}
                        onBlur={() => {
                          if (editing.current) commit({ rgb: draft.rgb });
                        }}
                      />
                      <div>
                        <Label htmlFor="hex-color">Hex colour</Label>
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
                                setHexError("Enter six hexadecimal digits.");
                            }
                          }}
                        />
                      </div>
                      <Button
                        variant="outline"
                        disabled={outputDisabled}
                        onClick={() => {
                          const rgb = parseHex(hex);
                          if (rgb) {
                            setHexError("");
                            commit({ rgb });
                          } else setHexError("Enter six hexadecimal digits.");
                        }}
                      >
                        Apply
                      </Button>
                    </div>
                    {hexError && (
                      <p className="inline-error" role="alert">
                        {hexError}
                      </p>
                    )}
                    <div className="color-swatches">
                      {[
                        "#edac65",
                        "#ed725d",
                        "#9ab6a4",
                        "#7eafd6",
                        "#dfdde5",
                        "#ff0020",
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
                    {(["r", "g", "b"] as const).map((channel) => (
                      <div className="channel" key={channel}>
                        <Label>{channel.toUpperCase()}</Label>
                        <Slider
                          aria-label={`${channel.toUpperCase()} channel`}
                          disabled={outputDisabled}
                          min={0}
                          max={255}
                          value={[draft.rgb[channel]]}
                          onValueChange={([v]) => {
                            setField("rgb", { ...draft.rgb, [channel]: v });
                            setHex(rgbHex({ ...draft.rgb, [channel]: v }));
                          }}
                          onValueCommit={([v]) =>
                            commit({ rgb: { ...draft.rgb, [channel]: v } })
                          }
                        />
                        <output>{draft.rgb[channel]}</output>
                      </div>
                    ))}
                  </div>
                )}
              </section>
              <div className="motion-row">
                <div>
                  <Label htmlFor="effect">A little movement</Label>
                  <select
                    id="effect"
                    disabled={outputDisabled || !supported.effects}
                    value={draft.effect}
                    onChange={(e) => commit({ effect: e.target.value })}
                  >
                    {(supported.effect_names || ["none"]).map((effect) => (
                      <option key={effect} value={effect}>
                        {effectLabel(effect)}
                      </option>
                    ))}
                  </select>
                </div>
                <div>
                  <Label htmlFor="transition">Transition</Label>
                  <select
                    id="transition"
                    value={draft.transition_ms}
                    disabled={
                      outputDisabled ||
                      !supported.transitions ||
                      (draft.mode === "white" &&
                        supported.white_transitions === false)
                    }
                    onChange={(e) =>
                      commit({ transition_ms: Number(e.target.value) })
                    }
                  >
                    {[
                      ...new Set([
                        0,
                        200,
                        800,
                        1500,
                        3000,
                        5000,
                        10000,
                        draft.transition_ms,
                      ]),
                    ]
                      .sort((a, b) => a - b)
                      .map((ms) => (
                        <option key={ms} value={ms}>
                          {ms ? `${ms / 1000} seconds` : "Instant"}
                        </option>
                      ))}
                  </select>
                  {draft.mode === "white" &&
                    supported.white_transitions === false && (
                      <p className="transition-note">
                        Smooth transitions are available in Color mode.
                      </p>
                    )}
                </div>
              </div>
              <div
                className={
                  "recording-row " + (draft.recording_lock ? "locked" : "")
                }
              >
                <LockKeyhole size={18} />
                <div>
                  <Label htmlFor="recording-lock">Recording lock</Label>
                  <p>
                    {draft.recording_lock
                      ? "Your look is held. Unlock to make changes; Off remains available."
                      : "Hold this look against accidental changes."}
                  </p>
                </div>
                <Switch
                  id="recording-lock"
                  aria-label="Recording lock"
                  checked={draft.recording_lock}
                  disabled={!authorized || !state || stale || busy}
                  onCheckedChange={(recording_lock) =>
                    commit({ recording_lock })
                  }
                />
              </div>
            </div>
          </TabsContent>
          <TabsContent value="scenes">
            <div className="section-intro">
              <div>
                <p className="eyebrow">GOOD LOOKS, KEPT CLOSE</p>
                <h2>Your moments of light.</h2>
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
                          "--scene":
                            scene.state.mode === "color"
                              ? rgbHex(scene.state.rgb)
                              : "#e5c494",
                        } as React.CSSProperties
                      }
                    >
                      <span>{String(scene.id).padStart(2, "0")}</span>
                      <Sun />
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
                <h3>The best light is worth keeping.</h3>
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
                <p className="eyebrow">BEHIND THE LIGHT</p>
                <h2>A little peace of mind.</h2>
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
            <Sun size={13} /> Independent. Open source. Entirely yours.
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
