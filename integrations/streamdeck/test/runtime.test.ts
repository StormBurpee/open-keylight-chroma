import { test } from "node:test";
import assert from "node:assert/strict";
import { createServer } from "node:http";
import { spawn } from "node:child_process";
import { once } from "node:events";
import { fileURLToPath } from "node:url";
import { WebSocketServer, type WebSocket } from "ws";
import { fixture } from "./fixture.js";

test(
  "compiled SDK plugin handles real key/dial/inspector events against loopback only",
  { timeout: 20000 },
  async (t) => {
    const state = fixture();
    state.revision = 1;
    let releaseResponse: (() => void) | undefined,
      holdResponse = false,
      activeWrites = 0,
      maxWrites = 0;
    const mutations: Array<{
      path: string;
      body: Record<string, unknown>;
      auth: string | undefined;
    }> = [];
    const server = createServer(async (req, res) => {
      res.setHeader("Content-Type", "application/json");
      if (req.url === "/api/v1/device") {
        res.end(
          JSON.stringify({
            api_version: 1,
            name: "Loopback light",
            capabilities: {
              scenes: true,
              color: true,
              transitions: true,
              white_transitions: false,
            },
          }),
        );
        return;
      }
      if (req.method === "GET") {
        res.end(
          JSON.stringify(
            req.url === "/api/v1/scenes"
              ? {
                  scenes: [
                    { id: 2, state: { ...state.desired, brightness: 33 } },
                  ],
                }
              : state,
          ),
        );
        return;
      }
      let body = "";
      for await (const chunk of req) body += chunk;
      const value = JSON.parse(body);
      activeWrites++;
      maxWrites = Math.max(maxWrites, activeWrites);
      mutations.push({
        path: req.url ?? "",
        body: value,
        auth: req.headers.authorization,
      });
      if (req.url?.endsWith("/activate")) state.desired.brightness = 33;
      else {
        const { expected_revision, ...patch } = value;
        assert.equal(expected_revision, state.revision);
        Object.assign(state.desired, patch);
      }
      state.revision++;
      Object.assign(state.reported, state.desired);
      if (holdResponse)
        await new Promise<void>((resolve) => {
          releaseResponse = resolve;
        });
      res.statusCode = 202;
      res.end(JSON.stringify(state));
      activeWrites--;
    });
    server.listen(0, "127.0.0.1");
    await once(server, "listening");
    const address = server.address();
    assert(address && typeof address === "object");
    const wss = new WebSocketServer({ host: "127.0.0.1", port: 0 });
    await once(wss, "listening");
    const wsAddress = wss.address();
    assert(wsAddress && typeof wsAddress !== "string");
    const messages: any[] = [];
    let socket: WebSocket | undefined;
    let stderr = "";
    wss.on("connection", (ws) => {
      socket = ws;
      ws.on("message", (data) => {
        const message = JSON.parse(data.toString());
        messages.push(message);
        if (message.event === "getSettings")
          ws.send(
            JSON.stringify({
              event: "didReceiveSettings",
              action: "org.openkeylight.chroma." + message.context,
              context: message.context,
              device: "deck",
              payload: {
                controller: "Keypad",
                coordinates: { column: 0, row: 0 },
                isInMultiAction: false,
                settings,
              },
            }),
          );
      });
    });
    const info = {
      application: {
        font: "Arial",
        language: "en",
        platform: "windows",
        platformVersion: "10",
        version: "7.0.3",
      },
      colors: {},
      devicePixelRatio: 1,
      devices: [
        {
          id: "deck",
          name: "Loopback Deck",
          type: 0,
          size: { columns: 5, rows: 3 },
        },
      ],
      plugin: { uuid: "org.openkeylight.chroma", version: "0.1.3.0" },
    };
    const plugin = fileURLToPath(
      new URL("../org.openkeylight.chroma.sdPlugin/", import.meta.url),
    );
    const child = spawn(
      process.env.OPEN_KEYLIGHT_PLUGIN_NODE || process.execPath,
      [
        "bin/plugin.js",
        "-port",
        String(wsAddress.port),
        "-pluginUUID",
        "org.openkeylight.chroma",
        "-registerEvent",
        "registerPlugin",
        "-info",
        JSON.stringify(info),
      ],
      { cwd: plugin, windowsHide: true, stdio: ["ignore", "pipe", "pipe"] },
    );
    child.stderr.on("data", (b) => {
      stderr += b;
    });
    t.after(async () => {
      child.kill();
      for (const client of wss.clients) client.terminate();
      await Promise.all([
        new Promise<void>((r) => wss.close(() => r())),
        new Promise<void>((r) => server.close(() => r())),
      ]);
    });
    const until = async (check: () => boolean, label: string) => {
      const deadline = Date.now() + 4000;
      while (!check()) {
        if (Date.now() > deadline)
          throw Error(
            label +
              "; stderr=" +
              stderr +
              "; received=" +
              JSON.stringify(messages),
          );
        await new Promise((r) => setTimeout(r, 15));
      }
    };
    await until(
      () => messages.some((m) => m.event === "registerPlugin"),
      "plugin registration",
    );
    const settings = {
      url: `http://127.0.0.1:${address.port}`,
      token: "loopback-test-token",
      step: 5,
      scene: 2,
      color: "#0080FF",
    };
    const send = (
      event: string,
      kind: string,
      controller = "Keypad",
      extra = {},
    ) =>
      socket!.send(
        JSON.stringify({
          event,
          action: "org.openkeylight.chroma." + kind,
          context: kind,
          device: "deck",
          payload: {
            controller,
            coordinates: { column: 0, row: 0 },
            isInMultiAction: false,
            settings,
            ...extra,
          },
        }),
      );
    send("willAppear", "power");
    await until(
      () =>
        messages.some(
          (m) => m.context === "power" && m.payload?.title === "ON",
        ),
      "initial controller readback",
    );
    send("keyDown", "power");
    await until(
      () =>
        messages.some(
          (m) => m.context === "power" && m.payload?.title === "OFF",
        ),
      "power write readback",
    );
    assert.deepEqual(mutations[0], {
      path: "/api/v1/state",
      body: { power: false, expected_revision: 1 },
      auth: "Bearer loopback-test-token",
    });
    await new Promise((r) => setTimeout(r, 350));
    send("willAppear", "brightness", "Encoder");
    await until(
      () =>
        messages.some(
          (m) => m.context === "brightness" && m.event === "setFeedback",
        ),
      "dial appeared",
    );
    send("dialRotate", "brightness", "Encoder", { ticks: 2 });
    await until(
      () => mutations.some((m) => m.body.brightness === 50),
      "dial brightness write",
    );
    await new Promise((r) => setTimeout(r, 350));
    send("willAppear", "lock");
    await until(
      () =>
        messages.some(
          (m) => m.context === "lock" && m.payload?.title === "Unlocked",
        ),
      "lock appeared",
    );
    send("keyDown", "lock");
    await until(
      () => mutations.some((m) => m.body.recording_lock === true),
      "lock enabled",
    );
    await new Promise((r) => setTimeout(r, 350));
    const before = mutations.length;
    send("dialRotate", "brightness", "Encoder", { ticks: 1 });
    await until(
      () =>
        messages.some(
          (m) => m.context === "brightness" && m.payload?.value === "Locked",
        ),
      "lock rejects dial",
    );
    assert.equal(mutations.length, before);
    send("keyDown", "lock");
    await until(
      () => mutations.some((m) => m.body.recording_lock === false),
      "explicit unlock",
    );
    await new Promise((r) => setTimeout(r, 350));
    send("willAppear", "scene");
    await until(
      () =>
        messages.some(
          (m) => m.context === "scene" && m.payload?.title === "Scene 2\nReady",
        ),
      "scene appeared",
    );
    send("keyDown", "scene");
    await until(
      () => mutations.some((m) => m.body.brightness === 33),
      "scene recalled",
    );
    await new Promise((r) => setTimeout(r, 350));
    send("willAppear", "color", "Encoder");
    await until(
      () =>
        messages.some(
          (m) => m.context === "color" && m.event === "setFeedback",
        ),
      "colour dial appeared",
    );
    send("dialRotate", "color", "Encoder", { ticks: 24 });
    await until(
      () => mutations.some((m) => (m.body.rgb as any)?.g === 255),
      "hue rotates red to green",
    );
    await new Promise((r) => setTimeout(r, 200));
    send("willDisappear", "color", "Encoder");
    send("willAppear", "color");
    await new Promise((r) => setTimeout(r, 100));
    send("keyDown", "color");
    await until(
      () =>
        mutations.some(
          (m) =>
            (m.body.rgb as any)?.b === 255 && (m.body.rgb as any)?.g === 128,
        ),
      "saved key colour",
    );
    await new Promise((r) => setTimeout(r, 200));
    holdResponse = true;
    const beforeBurst = mutations.length;
    send("dialRotate", "brightness", "Encoder", { ticks: 1 });
    await until(
      () => mutations.length === beforeBurst + 1,
      "slow first brightness write",
    );
    send("dialRotate", "brightness", "Encoder", { ticks: 20 });
    send("dialRotate", "brightness", "Encoder", { ticks: -1 });
    await until(
      () =>
        messages.some(
          (m) => m.context === "brightness" && m.payload?.value === "95% *",
        ),
      "pending accumulated value visible while HTTP is blocked",
    );
    holdResponse = false;
    releaseResponse!();
    await until(
      () => mutations.length === beforeBurst + 2,
      "one accumulated continuation",
    );
    assert.equal(mutations.at(-1)?.body.brightness, 95);
    await new Promise((r) => setTimeout(r, 200));
    holdResponse = true;
    const beforePower = mutations.length;
    send("dialRotate", "brightness", "Encoder", { ticks: -1 });
    await until(
      () => mutations.length === beforePower + 1,
      "active dial before power press",
    );
    send("dialRotate", "brightness", "Encoder", { ticks: -10 });
    send("dialDown", "brightness", "Encoder");
    holdResponse = false;
    releaseResponse!();
    await until(
      () => mutations.length === beforePower + 2,
      "power follows active write and cancels unsent dial",
    );
    assert.equal(mutations.at(-1)?.body.power, true);
    assert.equal(mutations.at(-1)?.body.brightness, undefined);
    assert.equal(maxWrites, 1);
    await new Promise((r) => setTimeout(r, 200));
    const afterScene = mutations.length;
    send("propertyInspectorDidAppear", "power");
    send("sendToPlugin", "power", "Keypad", { event: "check" });
    await until(
      () =>
        messages.some(
          (m) =>
            m.event === "sendToPropertyInspector" &&
            m.payload?.name === "Loopback light",
        ),
      "property inspector connection read",
    );
    assert.equal(mutations.length, afterScene);
    assert.equal(messages.filter((m) => m.event === "getSettings").length, 1);
    assert.equal(stderr, "");
  },
);
