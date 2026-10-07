import type { State } from "../src/client.js";
export const fixture = (): State => {
  const desired = {
    power: true,
    brightness: 40,
    recording_lock: false,
    mode: "color" as const,
    rgb: { r: 255, g: 0, b: 0 },
    effect: "none",
    transition_ms: 0,
    temperature_k: 4500,
  };
  return {
    revision: 7,
    desired,
    reported: {
      ...desired,
      valid: true,
      confirmed_fields: Object.keys(desired),
    },
    operation: { status: "idle" },
  };
};
export const device = {
  api_version: 1,
  name: "Test light",
  controller: { ready: true },
  capabilities: {
    scenes: true,
    color: true,
    white: true,
    transitions: true,
    white_transitions: false,
  },
};
export const response = (value: unknown, status = 200) =>
  new Response(JSON.stringify(value), { status });
export const wait = (n = 10) =>
  new Promise<void>((resolve) => setTimeout(resolve, n));
export async function until(predicate: () => boolean) {
  for (let i = 0; i < 200; i++) {
    if (predicate()) return;
    await wait(5);
  }
  throw Error("Condition did not become true");
}
export function deferred<T>() {
  let resolve!: (v: T) => void, reject!: (e: unknown) => void;
  const promise = new Promise<T>((r, j) => {
    resolve = r;
    reject = j;
  });
  return { promise, resolve, reject };
}
