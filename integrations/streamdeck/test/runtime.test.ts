import { test } from "node:test";
import assert from "node:assert/strict";
import { createServer } from "node:http";
import { spawn } from "node:child_process";
import { once } from "node:events";
import { fileURLToPath } from "node:url";
import { WebSocketServer, type WebSocket } from "ws";

test(
  "compiled SDK plugin handles real key/dial/inspector events against loopback only",
  { timeout: 20000 },
  async (t) => {
    const state = {
      revision: 1,
      desired: { power: true, brightness: 40, recording_lock: false },
      reported: {
        valid: true,
        confirmed_fields: ["power", "brightness"],
        power: true,
        brightness: 40,
      },
      operation: { status: "idle" },
    };
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
            capabilities: { scenes: true },
          }),
        );
        return;
      }
      if (req.method === "GET") {
        res.end(JSON.stringify(state));
        return;
      }
      let body = "";
      for await (const chunk of req) body += chunk;
      const value = JSON.parse(body);
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
      state.reported.power = state.desired.power;
      state.reported.brightness = state.desired.brightness;
      res.statusCode = 202;
      res.end(JSON.stringify(state));
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
          ws.send(JSON.stringify({
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
          }));
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
      () => mutations.some((m) => m.path === "/api/v1/scenes/2/activate"),
      "scene recalled",
    );
    await new Promise((r) => setTimeout(r, 350));
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
