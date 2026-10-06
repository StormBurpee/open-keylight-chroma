"use strict";
let socket,
  context,
  actionId,
  settings = {};
const $ = (id) => document.getElementById(id);
function status(text, error = false) {
  $("status").textContent = text;
  $("status").classList.toggle("error", error);
}
function fill() {
  for (const key of ["url", "token", "step", "scene"])
    $(key).value = String(
      settings[key] ?? (key === "step" ? 5 : key === "scene" ? 1 : ""),
    );
}
function send(event, payload) {
  if (socket?.readyState !== 1)
    throw Error("Stream Deck connection is closed.");
  socket.send(
    JSON.stringify({
      event,
      context,
      ...(event === "sendToPlugin" ? { action: actionId } : {}),
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
  context = action.context;
  actionId = action.action;
  settings = action.payload?.settings ?? {};
  $("step-field").hidden = !actionId.endsWith(".brightness");
  $("scene-field").hidden = !actionId.endsWith(".scene");
  fill();
  socket = new WebSocket(`ws://127.0.0.1:${port}`);
  socket.onopen = () => {
    socket.send(JSON.stringify({ event, uuid }));
    $("save").disabled = false;
    $("check").disabled = false;
    status("Settings are local to this action.");
  };
  socket.onclose = () => {
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
    settings = {
      url: u.origin,
      token: $("token").value.trim(),
      step: Number($("step").value),
      scene: Number($("scene").value),
    };
    send("setSettings", settings);
    status(
      "Settings saved. Check connection reads the API without changing output.",
    );
  } catch (error) {
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
