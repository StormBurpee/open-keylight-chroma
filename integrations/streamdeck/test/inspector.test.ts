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
      sent.push(JSON.parse(v));
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
test("property inspector addresses settings with both action type and instance, then verifies saved values", async () => {
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
    context: "key-context",
    action: "org.openkeylight.chroma.brightness",
    payload: { url: "http://light.local", token: "secret", step: 5, scene: 2 },
  });
  assert.deepEqual(sent[2], {
    event: "getSettings",
    context: "key-context",
    action: "org.openkeylight.chroma.brightness",
  });
  assert.equal($("check").disabled, true);
  assert.equal($("status").textContent, "Saving settings…");
  ws.onmessage({ data: JSON.stringify({
    event: "didReceiveSettings", context: "key-context",
    payload: { settings: sent[1].payload },
  }) });
  assert.match($("status").textContent, /saved and read back/);
  assert.equal($("check").disabled, false);
  dom.window.close();
});

test("a valid IPv4 origin survives save/readback; a rejected save is not labelled successful", async () => {
  const { dom, sent, ws, $ } = await page();
  $("url").value = "http://192.168.86.248";
  $("settings").dispatchEvent(new dom.window.Event("submit", { cancelable: true }));
  assert.equal((sent[1].payload as any).url, "http://192.168.86.248");
  ws.onmessage({ data: JSON.stringify({
    event: "didReceiveSettings", context: "key-context", payload: { settings: {} },
  }) });
  assert.match($("status").textContent, /returned different settings/);
  assert.equal($("status").classList.contains("error"), true);
  dom.window.close();
});
test("connection check sends only a read-check request and explicitly avoids claiming token validation", async () => {
  const { dom, sent, ws, $ } = await page();
  $("check").click();
  assert.deepEqual(sent[1], {
    event: "sendToPlugin",
    context: "key-context",
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
