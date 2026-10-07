"use strict";
let socket,
  context,
  inspectorId,
  actionId,
  pendingSave,
  saveTimeout,
  settings = {};
const $ = (id) => document.getElementById(id);
function status(text, error = false) {
  $("status").textContent = text;
  $("status").classList.toggle("error", error);
}
function fill() {
  const defaults = {
    url: "",
    token: "",
    step: 5,
    scene: 1,
    color: "#FF8844",
    hueStep: 5,
    fadeMs: 150,
    temperature: 4500,
    temperatureStep: 100,
  };
  for (const key of Object.keys(defaults))
    $(key).value = String(settings[key] ?? defaults[key]);
}
function send(event, payload) {
  if (socket?.readyState !== 1)
    throw Error("Stream Deck connection is closed.");
  socket.send(
    JSON.stringify({
      event,
      context: inspectorId,
      action: actionId,
      payload,
    }),
  );
}
window.connectElgatoStreamDeckSocket = (
  port,
  uuid,
  event,
  _info,
  actionInfo,
) => {
  const action = JSON.parse(actionInfo);
  inspectorId = uuid;
  context = action.context;
  actionId = action.action;
  settings = action.payload?.settings ?? {};
  $("step-field").hidden = !actionId.endsWith(".brightness");
  $("scene-field").hidden = !actionId.endsWith(".scene");
  $("color-field").hidden = !actionId.endsWith(".color");
  $("temperature-field").hidden = !actionId.endsWith(".temperature");
  $("fade-field").hidden = ![".color", ".brightness"].some((kind) =>
    actionId.endsWith(kind),
  );
  fill();
  socket = new WebSocket(`ws://127.0.0.1:${port}`);
  socket.onopen = () => {
    socket.send(JSON.stringify({ event, uuid }));
    $("save").disabled = false;
    $("check").disabled = false;
    status("Settings are local to this action.");
  };
  socket.onclose = () => {
    clearTimeout(saveTimeout);
    pendingSave = undefined;
    $("save").disabled = true;
    $("check").disabled = true;
    status("Stream Deck connection closed.", true);
  };
  socket.onmessage = (event) => {
    let data;
    try {
      data = JSON.parse(event.data);
    } catch {
      return;
    }
    if (data.context && data.context !== context) return;
    if (data.event === "didReceiveSettings") {
      settings = data.payload.settings;
      fill();
      if (pendingSave) {
        const matches = Object.keys(pendingSave).every(
          (key) =>
            JSON.stringify(settings[key]) === JSON.stringify(pendingSave[key]),
        );
        clearTimeout(saveTimeout);
        pendingSave = undefined;
        $("save").disabled = false;
        $("check").disabled = false;
        status(
          matches
            ? "Settings saved and read back from Stream Deck. Ready to check connection."
            : "Stream Deck returned different settings. Save again before checking connection.",
          !matches,
        );
      }
    }
    if (data.event === "sendToPropertyInspector") {
      $("check").disabled = false;
      const p = data.payload ?? {};
      status(
        p.error ||
          `${p.name}: API v1 reachable · revision ${p.revision}. ${p.message ?? ""}`,
        Boolean(p.error),
      );
    }
  };
};
$("settings").addEventListener("submit", (event) => {
  event.preventDefault();
  try {
    const u = new URL($("url").value.trim());
    if (
      !["http:", "https:"].includes(u.protocol) ||
      u.username ||
      u.password ||
      u.search ||
      u.hash ||
      u.pathname !== "/"
    )
      throw Error(
        "Use a full light origin, without a path, credentials or query.",
      );
    const next = {
      ...settings,
      url: u.origin,
      token: $("token").value.trim(),
      step: Number($("step").value),
      scene: Number($("scene").value),
      color: $("color").value.toUpperCase(),
      hueStep: Number($("hueStep").value),
      fadeMs: Number($("fadeMs").value),
      temperature: Number($("temperature").value),
      temperatureStep: Number($("temperatureStep").value),
    };
    if (
      !/^#[0-9A-F]{6}$/.test(next.color) ||
      ![1, 5, 10, 15].includes(next.hueStep) ||
      ![0, 100, 150, 200, 400].includes(next.fadeMs)
    )
      throw Error("Choose a valid colour, hue step and fade.");
    if (
      !Number.isInteger(next.temperature) ||
      next.temperature < 3000 ||
      next.temperature > 7000 ||
      ![50, 100, 250, 500].includes(next.temperatureStep)
    )
      throw Error(
        "Choose a white temperature from 3000 to 7000 K and a supported dial step.",
      );
    pendingSave = next;
    $("save").disabled = true;
    $("check").disabled = true;
    send("setSettings", next);
    send("getSettings");
    status("Saving settings…");
    saveTimeout = setTimeout(() => {
      pendingSave = undefined;
      $("save").disabled = socket?.readyState !== 1;
      $("check").disabled = socket?.readyState !== 1;
      status(
        "Stream Deck has not confirmed the saved settings. Reopen this action to check them.",
        true,
      );
    }, 4000);
  } catch (error) {
    pendingSave = undefined;
    $("save").disabled = socket?.readyState !== 1;
    $("check").disabled = socket?.readyState !== 1;
    status(error.message, true);
  }
});
$("check").addEventListener("click", () => {
  try {
    send("sendToPlugin", { event: "check" });
    $("check").disabled = true;
    status("Checking saved address…");
    setTimeout(() => {
      $("check").disabled = socket?.readyState !== 1;
    }, 9000);
  } catch (error) {
    status(error.message, true);
  }
});
