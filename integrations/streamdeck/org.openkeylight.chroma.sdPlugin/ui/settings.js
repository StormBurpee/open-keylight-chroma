"use strict";
let socket,
  context,
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
          (key) => settings[key] === pendingSave[key],
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
      url: u.origin,
      token: $("token").value.trim(),
      step: Number($("step").value),
      scene: Number($("scene").value),
    };
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
      status("Stream Deck has not confirmed the saved settings. Reopen this action to check them.", true);
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
