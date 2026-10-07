import { test } from "node:test";
import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
// jsdom is test-only; the shipped inspector has no dependency or CDN.
// @ts-expect-error jsdom has no bundled declarations and is only used by this harness.
import { JSDOM } from "jsdom";
const base = new URL(
  "../org.openkeylight.chroma.sdPlugin/ui/",
  import.meta.url,
);
async function page(kind = "brightness") {
  const dom = new JSDOM(
    await readFile(new URL("settings.html", base), "utf8"),
    { runScripts: "outside-only", url: "http://localhost/" },
  );
  const sent: Record<string, unknown>[] = [];
  let ws: any;
  class FakeSocket {
    readyState = 1;
    onopen?: () => void;
    onmessage?: Function;
    onclose?: () => void;
    constructor() {
      ws = this;
    }
    send(v: string) {
      const message = JSON.parse(v);
      if (message.event !== "registerPropertyInspector")
        assert.equal(
          message.context,
          "pi-uuid",
          "Stream Deck rejects PI commands addressed to the action instance",
        );
      sent.push(message);
    }
  }
  dom.window.WebSocket = FakeSocket;
  dom.window.eval(await readFile(new URL("settings.js", base), "utf8"));
  dom.window.connectElgatoStreamDeckSocket(
    "12345",
    "pi-uuid",
    "registerPropertyInspector",
    "{}",
    JSON.stringify({
      context: "key-context",
      action: "org.openkeylight.chroma." + kind,
      payload: {
        settings: {
          url: "http://light.local",
          token: "secret",
          step: 5,
          scene: 2,
        },
      },
    }),
  );
  ws.onopen();
  return {
    dom,
    sent,
    ws,
    $: (id: string) => dom.window.document.getElementById(id),
  };
}
test("property inspector sends its registered UUID and action type, then verifies the action's saved values", async () => {
  const { dom, sent, ws, $ } = await page();
  assert.deepEqual(sent[0], {
    event: "registerPropertyInspector",
    uuid: "pi-uuid",
  });
  $("settings").dispatchEvent(
    new dom.window.Event("submit", { cancelable: true }),
  );
  assert.deepEqual(sent[1], {
    event: "setSettings",
    context: "pi-uuid",
    action: "org.openkeylight.chroma.brightness",
    payload: {
      url: "http://light.local",
      token: "secret",
      step: 5,
      scene: 2,
      color: "#FF8844",
      hueStep: 5,
      fadeMs: 150,
      temperature: 4500,
      temperatureStep: 100,
    },
  });
  assert.deepEqual(sent[2], {
    event: "getSettings",
    context: "pi-uuid",
    action: "org.openkeylight.chroma.brightness",
  });
  assert.equal($("check").disabled, true);
  assert.equal($("status").textContent, "Saving settings…");
  ws.onmessage({
    data: JSON.stringify({
      event: "didReceiveSettings",
      context: "key-context",
      payload: { settings: sent[1].payload },
    }),
  });
  assert.match($("status").textContent, /saved and read back/);
  assert.equal($("check").disabled, false);
  dom.window.close();
});

test("a valid IPv4 origin survives save/readback; a rejected save is not labelled successful", async () => {
  const { dom, sent, ws, $ } = await page();
  $("url").value = "http://192.168.86.248";
  $("settings").dispatchEvent(
    new dom.window.Event("submit", { cancelable: true }),
  );
  assert.equal((sent[1].payload as any).url, "http://192.168.86.248");
  ws.onmessage({
    data: JSON.stringify({
      event: "didReceiveSettings",
      context: "key-context",
      payload: { settings: {} },
    }),
  });
  assert.match($("status").textContent, /returned different settings/);
  assert.equal($("status").classList.contains("error"), true);
  dom.window.close();
});
test("connection check sends only a read-check request and explicitly avoids claiming token validation", async () => {
  const { dom, sent, ws, $ } = await page();
  $("check").click();
  assert.deepEqual(sent[1], {
    event: "sendToPlugin",
    context: "pi-uuid",
    action: "org.openkeylight.chroma.brightness",
    payload: { event: "check" },
  });
  ws.onmessage({
    data: JSON.stringify({
      event: "sendToPropertyInspector",
      context: "key-context",
      payload: {
        ok: true,
        name: "Studio",
        revision: 3,
        message: "Token authorization is checked when you use a control.",
      },
    }),
  });
  assert.match($("status").textContent, /Token authorization is checked/);
  dom.window.close();
});
test("invalid addresses do not replace saved settings and action-specific controls stay scoped", async () => {
  const { dom, sent, $ } = await page("scene");
  assert.equal($("step-field").hidden, true);
  assert.equal($("scene-field").hidden, false);
  $("url").value = "https://secret@elsewhere/api";
  $("settings").dispatchEvent(
    new dom.window.Event("submit", { cancelable: true }),
  );
  assert.equal(sent.length, 1);
  assert.equal($("status").classList.contains("error"), true);
  dom.window.close();
});
test("foreign-context replies and device HTML are never treated as markup", async () => {
  const { dom, ws, $ } = await page();
  const old = $("status").textContent;
  ws.onmessage({
    data: JSON.stringify({
      event: "sendToPropertyInspector",
      context: "other",
      payload: { error: "other device" },
    }),
  });
  assert.equal($("status").textContent, old);
  ws.onmessage({
    data: JSON.stringify({
      event: "sendToPropertyInspector",
      context: "key-context",
      payload: { error: "<img src=x onerror=alert(1)>" },
    }),
  });
  assert.equal($("status").children.length, 0);
  dom.window.close();
});

test("colour picker, hue step and fade persist through the registered PI with exact readback", async () => {
  const { dom, sent, ws, $ } = await page("color");
  assert.equal($("color-field").hidden, false);
  assert.equal($("fade-field").hidden, false);
  assert.equal($("step-field").hidden, true);
  $("color").value = "#22aaff";
  $("hueStep").value = "15";
  $("fadeMs").value = "200";
  $("settings").dispatchEvent(
    new dom.window.Event("submit", { cancelable: true }),
  );
  const settings = sent[1].payload as any;
  assert.equal(settings.color, "#22AAFF");
  assert.equal(settings.hueStep, 15);
  assert.equal(settings.fadeMs, 200);
  assert.equal(settings.token, "secret");
  assert.equal(sent[1].context, "pi-uuid");
  ws.onmessage({
    data: JSON.stringify({
      event: "didReceiveSettings",
      context: "key-context",
      payload: { settings },
    }),
  });
  assert.match($("status").textContent, /saved and read back/);
  assert.equal($("hueStep").value, "15");
  dom.window.close();
});

test("future settings survive edits and changed fade is not falsely acknowledged", async () => {
  const { dom, sent, ws, $ } = await page();
  ws.onmessage({
    data: JSON.stringify({
      event: "didReceiveSettings",
      context: "key-context",
      payload: {
        settings: {
          url: "http://light.local",
          token: "secret",
          future: { enabled: true },
        },
      },
    }),
  });
  $("settings").dispatchEvent(
    new dom.window.Event("submit", { cancelable: true }),
  );
  const saved = sent[1].payload as any;
  assert.deepEqual(saved.future, { enabled: true });
  ws.onmessage({
    data: JSON.stringify({
      event: "didReceiveSettings",
      context: "key-context",
      payload: { settings: { ...saved, fadeMs: 0 } },
    }),
  });
  assert.match($("status").textContent, /different settings/);
  dom.window.close();
});

test("temperature PI exposes Kelvin controls only and preserves existing saved fields", async () => {
  const { dom, sent, ws, $ } = await page("temperature");
  assert.equal($("temperature-field").hidden, false);
  assert.equal($("color-field").hidden, true);
  assert.equal($("fade-field").hidden, true);
  assert.equal($("step-field").hidden, true);
  assert.equal($("temperature").value, "4500");
  assert.equal($("temperatureStep").value, "100");
  assert.match($("temperature-field").textContent, /direct/);
  assert.match(
    $("temperature-field").textContent,
    /Power and brightness stay unchanged/,
  );
  $("temperature").value = "5300";
  $("temperatureStep").value = "250";
  $("settings").dispatchEvent(
    new dom.window.Event("submit", { cancelable: true }),
  );
  const value = sent[1].payload as any;
  assert.equal(value.temperature, 5300);
  assert.equal(value.temperatureStep, 250);
  assert.equal(value.token, "secret");
  assert.equal(value.scene, 2);
  assert.equal(sent[1].context, "pi-uuid");
  ws.onmessage({
    data: JSON.stringify({
      event: "didReceiveSettings",
      context: "key-context",
      payload: { settings: value },
    }),
  });
  assert.match($("status").textContent, /saved and read back/);
  dom.window.close();
});
test("invalid Kelvin settings never replace saved settings; readback mismatch is not success", async () => {
  const { dom, sent, ws, $ } = await page("temperature");
  for (const value of ["2999", "7001", "4500.5"]) {
    $("temperature").value = value;
    $("settings").dispatchEvent(
      new dom.window.Event("submit", { cancelable: true }),
    );
    assert.equal(sent.length, 1);
    assert.match($("status").textContent, /3000 to 7000 K/);
  }
  $("temperature").value = "3000";
  $("settings").dispatchEvent(
    new dom.window.Event("submit", { cancelable: true }),
  );
  const saved = sent[1].payload as any;
  ws.onmessage({
    data: JSON.stringify({
      event: "didReceiveSettings",
      context: "key-context",
      payload: { settings: { ...saved, temperature: 4500 } },
    }),
  });
  assert.match($("status").textContent, /different settings/);
  dom.window.close();
});
