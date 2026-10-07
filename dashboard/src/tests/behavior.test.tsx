import {
  act,
  fireEvent,
  render,
  screen,
  waitFor,
} from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import { describe, expect, it, vi } from "vitest";
import { Studio } from "../App";
import { ApiError, StudioStore } from "../api";
import { DemoTransport } from "../demo";

async function setup() {
  const api = new DemoTransport();
  api.state.desired.mode = "color";
  api.state.desired.transition_ms = 2400;
  api.scenes = [
    {
      id: 1,
      name: "Interview",
      state: {
        ...api.state.desired,
        mode: "white",
        temperature_k: 3200,
        brightness: 25,
        transition_ms: 1200,
      },
    },
  ];
  const requests = vi.spyOn(api, "request");
  const store = new StudioStore(api);
  const view = render(<Studio store={store} />);
  await screen.findByLabelText("Hex colour");
  await screen.findByRole("button", { name: "Activate Interview" });
  return { ...view, api, store, requests, user: userEvent.setup() };
}

function previewWheel(container: HTMLElement) {
  const hue = container.querySelector<HTMLElement>(
    '[data-colour-surface="hue"]',
  )!;
  vi.spyOn(hue, "getBoundingClientRect").mockReturnValue({
    left: 20,
    top: 30,
    width: 200,
    height: 200,
    right: 220,
    bottom: 230,
    x: 20,
    y: 30,
    toJSON: () => ({}),
  });
  vi.spyOn(HTMLElement.prototype, "hasPointerCapture").mockReturnValue(true);
  fireEvent.pointerDown(hue, {
    clientX: 120,
    clientY: 230,
    pointerId: 1,
    button: 0,
    isPrimary: true,
  });
  return hue;
}

describe("studio navigation and scene behavior", () => {
  it("restores the latest controller state on pointer cancellation without committing the preview", async () => {
    const { api, store, requests, container } = await setup();
    const hue = previewWheel(container);
    await act(async () => {
      api.state = {
        ...api.state,
        revision: api.state.revision + 1,
        desired: {
          ...api.state.desired,
          brightness: 21,
          rgb: { r: 12, g: 80, b: 144 },
        },
      };
      await store.refresh();
    });
    expect(screen.getByLabelText("Hex colour")).not.toHaveValue("#0c5090");
    fireEvent.pointerCancel(hue, { pointerId: 1 });
    expect(screen.getByLabelText("Hex colour")).toHaveValue("#0c5090");
    expect(screen.getByRole("slider", { name: "Brightness" })).toHaveAttribute(
      "aria-valuenow",
      "21",
    );
    fireEvent.pointerUp(hue, { pointerId: 1, clientX: 20, clientY: 130 });
    await act(async () => {
      api.state = {
        ...api.state,
        desired: { ...api.state.desired, brightness: 33 },
      };
      await store.refresh();
    });
    expect(screen.getByRole("slider", { name: "Brightness" })).toHaveAttribute(
      "aria-valuenow",
      "33",
    );
    expect(requests.mock.calls.filter(([method]) => method !== "GET")).toEqual(
      [],
    );
  });

  it("cancels an uncommitted wheel gesture across main tabs without a late write", async () => {
    const { api, store, requests, container, user } = await setup();
    const original = structuredClone(api.state.desired.rgb);
    const hue = previewWheel(container);
    expect(screen.getByLabelText("Hex colour")).not.toHaveValue("#ebac65");
    await user.click(screen.getByRole("tab", { name: "Scenes" }));
    fireEvent.pointerUp(hue, { pointerId: 1, clientX: 20, clientY: 130 });
    await user.click(screen.getByRole("tab", { name: "Light" }));
    expect(await screen.findByLabelText("Hex colour")).toHaveValue("#ebac65");
    await act(async () => {
      await store.refresh();
    });
    expect(api.state.desired.rgb).toEqual(original);
    expect(requests.mock.calls.filter(([method]) => method !== "GET")).toEqual(
      [],
    );
  });

  it("cancels a wheel preview when opening Effects and continues accepting fresh state", async () => {
    const { api, store, requests, container, user } = await setup();
    previewWheel(container);
    await user.click(screen.getByRole("button", { name: /^Effects$/ }));
    await act(async () => {
      api.state = {
        ...api.state,
        revision: api.state.revision + 1,
        desired: { ...api.state.desired, brightness: 31 },
      };
      await store.refresh();
    });
    expect(screen.getByRole("slider", { name: "Brightness" })).toHaveAttribute(
      "aria-valuenow",
      "31",
    );
    expect(api.state.desired.rgb).toEqual({ r: 235, g: 172, b: 101 });
    expect(requests.mock.calls.filter(([method]) => method !== "GET")).toEqual(
      [],
    );
  });

  it("activates a saved scene from Light even while an unapplied hex draft exists", async () => {
    const { api, store, requests, user } = await setup();
    await user.clear(screen.getByLabelText("Hex colour"));
    await user.type(screen.getByLabelText("Hex colour"), "#ff0020");
    await user.click(
      screen.getByRole("button", { name: "Activate Interview" }),
    );
    await waitFor(() => expect(api.state.desired.mode).toBe("white"));
    await waitFor(() => expect(store.getSnapshot().busy).toBe(false));
    expect(
      await screen.findByRole("slider", { name: "White temperature" }),
    ).toHaveAttribute("aria-valuenow", "3200");
    expect(screen.getByRole("slider", { name: "Brightness" })).toHaveAttribute(
      "aria-valuenow",
      "25",
    );
    expect(
      requests.mock.calls.filter(
        ([method, path]) => method === "POST" && path === "/scenes/1/activate",
      ),
    ).toHaveLength(1);
    expect(api.state.desired.rgb).toEqual(api.scenes[0].state.rgb);
  });

  it("saves the accepted look, then actually recalls it from the Scenes page", async () => {
    const { api, store, user } = await setup();
    await user.click(screen.getByRole("tab", { name: "Scenes" }));
    await user.click(screen.getByRole("button", { name: "Save current look" }));
    await user.type(screen.getByLabelText("Scene name"), "Blue interview");
    await user.click(screen.getByRole("button", { name: "Save scene" }));
    const activate = await screen.findByRole("button", {
      name: "Activate Blue interview",
    });
    await act(async () => {
      api.state = {
        ...api.state,
        desired: { ...api.state.desired, brightness: 13, mode: "white" },
      };
      await store.refresh();
    });
    await user.click(activate);
    await waitFor(() => expect(api.state.desired.brightness).toBe(64));
    expect(api.state.desired.mode).toBe("color");
    expect(
      api.scenes.find((scene) => scene.name === "Blue interview")?.state
        .recording_lock,
    ).toBe(false);
  });

  it("retains the last positive fade across Off, navigation, and re-enabling", async () => {
    const { api, store, user } = await setup();
    await user.click(screen.getByRole("switch", { name: "Smooth transition" }));
    await waitFor(() => expect(api.state.desired.transition_ms).toBe(0));
    await waitFor(() => expect(store.getSnapshot().busy).toBe(false));
    await user.click(screen.getByRole("tab", { name: "Scenes" }));
    await user.click(screen.getByRole("tab", { name: "Light" }));
    await user.click(screen.getByRole("switch", { name: "Smooth transition" }));
    await waitFor(() => expect(api.state.desired.transition_ms).toBe(2400));
    await waitFor(() => expect(store.getSnapshot().busy).toBe(false));
    await act(async () => {
      api.state = {
        ...api.state,
        desired: { ...api.state.desired, transition_ms: 3600 },
      };
      await store.refresh();
    });
    await user.click(screen.getByRole("switch", { name: "Smooth transition" }));
    await waitFor(() => expect(api.state.desired.transition_ms).toBe(0));
    await waitFor(() => expect(store.getSnapshot().busy).toBe(false));
    await user.click(screen.getByRole("switch", { name: "Smooth transition" }));
    await waitFor(() => expect(api.state.desired.transition_ms).toBe(3600));
  });

  it("reports a scene activation failure without retrying it or faking scene state", async () => {
    const { api, store, requests, user } = await setup();
    const original = DemoTransport.prototype.request.bind(api);
    requests.mockImplementation(async (method, path, body) => {
      if (method === "POST" && path === "/scenes/1/activate")
        throw new ApiError("Controller did not accept the scene.", 503);
      return original(method, path, body);
    });
    await user.click(
      screen.getByRole("button", { name: "Activate Interview" }),
    );
    expect(
      await screen.findByText("Controller did not accept the scene."),
    ).toBeVisible();
    await act(async () => {
      await store.refresh();
    });
    expect(api.state.desired.mode).toBe("color");
    expect(api.state.desired.brightness).toBe(64);
    expect(
      requests.mock.calls.filter(
        ([method, path]) => method === "POST" && path === "/scenes/1/activate",
      ),
    ).toHaveLength(1);
  });
});
