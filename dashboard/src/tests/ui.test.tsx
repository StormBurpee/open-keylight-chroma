import { act, render, screen, waitFor } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import { describe, it, expect, vi } from "vitest";
import { Studio } from "../App";
import { StudioStore, sha256 } from "../api";
import { DemoTransport } from "../demo";
async function setup(change?: (api: DemoTransport) => void) {
  const api = new DemoTransport();
  change?.(api);
  const store = new StudioStore(api);
  render(<Studio store={store} />);
  await screen.findByRole("heading", { name: "Studio key." });
  await waitFor(() =>
    expect(
      screen.getByRole("button", { name: "Turn light off" }),
    ).toBeEnabled(),
  );
  return { api, store, user: userEvent.setup() };
}
describe("studio controls", () => {
  it("blocks output for diagnostic firmware while keeping system controls available", async () => {
    const { api, store, user } = await setup();
    await act(async () => {
      api.device.controller = {
        ...api.device.controller,
        ready: false,
        status: "diagnostic",
      };
      await store.refresh();
    });
    expect(
      screen.getByRole("button", { name: "Turn light off" }),
    ).toBeDisabled();
    expect(screen.getByRole("slider", { name: "Brightness" })).toHaveAttribute(
      "data-disabled",
    );
    await user.click(screen.getByRole("tab", { name: "System" }));
    expect(await screen.findByText("Open Keylight")).toBeVisible();
    expect(
      screen.getByRole("button", { name: "Refresh details" }),
    ).toBeEnabled();
    await act(async () => {
      api.device.controller = {
        ...api.device.controller,
        backend: "legacy",
        ready: true,
        status: "ready",
      };
      await store.refresh();
    });
    expect(screen.getByText("Razer compatibility")).toBeVisible();
    await user.click(screen.getByRole("tab", { name: "Light" }));
    expect(
      screen.getByRole("button", { name: "Turn light off" }),
    ).toBeEnabled();
    expect(api.state.revision).toBe(12);
  });

  it("requires an explicit click to confirm trial firmware", async () => {
    const { user, api } = await setup((api) => {
      api.device.trial_pending = true;
    });
    expect(api.device.trial_pending).toBe(true);
    await user.click(
      screen.getByRole("button", { name: "Confirm this firmware" }),
    );
    await waitFor(() => expect(api.device.trial_pending).toBe(false));
    await waitFor(() =>
      expect(
        screen.queryByRole("button", { name: "Confirm this firmware" }),
      ).not.toBeInTheDocument(),
    );
  });
  it("honours separate white transition capability", async () => {
    const { user } = await setup((api) => {
      api.device.capabilities.white_transitions = false;
    });
    expect(screen.getByLabelText("Transition")).toBeDisabled();
    expect(
      screen.getByText("Smooth transitions are available in Color mode."),
    ).toBeVisible();
    await user.click(screen.getByRole("button", { name: "Color" }));
    await waitFor(() =>
      expect(screen.getByLabelText("Transition")).toBeEnabled(),
    );
  });
  it("labels isolated preview persistently and reports only confirmed fields", async () => {
    const { user } = await setup();
    expect(screen.getByText("ISOLATED DEMO")).toBeVisible();
    await user.click(screen.getByRole("button", { name: "Color" }));
    await waitFor(() => expect(screen.getByText("Unconfirmed")).toBeVisible());
    expect(screen.getByRole("slider", { name: "R channel" })).toBeVisible();
  });
  it("locks every output change except Off until explicitly unlocked", async () => {
    const { user, api } = await setup();
    await user.click(screen.getByRole("switch", { name: "Recording lock" }));
    await waitFor(() => expect(api.state.desired.recording_lock).toBe(true));
    await waitFor(() =>
      expect(
        screen.getByRole("slider", { name: "Brightness" }),
      ).toHaveAttribute("data-disabled"),
    );
    expect(
      screen.getByRole("button", { name: "Turn light off" }),
    ).toBeEnabled();
    await user.click(screen.getByRole("button", { name: "Turn light off" }));
    await waitFor(() =>
      expect(
        screen.getByRole("button", { name: "Turn light on" }),
      ).toBeDisabled(),
    );
    await user.click(screen.getByRole("switch", { name: "Recording lock" }));
    await waitFor(() => expect(api.state.desired.recording_lock).toBe(false));
  });
  it("shows no fabricated scenes, saves a deliberate name, then activates it", async () => {
    const { user, api } = await setup();
    await user.click(screen.getByRole("tab", { name: "Scenes" }));
    expect(
      await screen.findByText("The best light is worth keeping."),
    ).toBeVisible();
    await user.click(screen.getByRole("button", { name: "Save current look" }));
    await user.type(screen.getByLabelText("Scene name"), "Interview");
    await user.click(screen.getByRole("button", { name: "Save scene" }));
    await screen.findByRole("button", { name: "Activate Interview" });
    expect(api.scenes).toHaveLength(1);
    expect(api.scenes[0].state.recording_lock).toBe(false);
  });
  it("does not pretend unsupported effects or updates are available", async () => {
    const { user } = await setup((api) => {
      api.device.capabilities.effects = false;
      api.device.capabilities.transitions = false;
    });
    expect(screen.getByLabelText("A little movement")).toBeDisabled();
    expect(screen.getByLabelText("Transition")).toBeDisabled();
    await user.click(screen.getByRole("tab", { name: "System" }));
    expect(
      await screen.findByText(
        "Application updates are not advertised by this firmware.",
      ),
    ).toBeVisible();
    expect(
      screen.queryByRole("button", { name: "Verify & update" }),
    ).not.toBeInTheDocument();
  });
  it("requires valid RGB input without silently changing the light", async () => {
    const { user, api } = await setup();
    await user.click(screen.getByRole("button", { name: "Color" }));
    const input = await screen.findByLabelText("Hex colour");
    await user.clear(input);
    await user.type(input, "#zzzzzz");
    const before = { ...api.state.desired.rgb };
    await user.click(screen.getByRole("button", { name: "Apply" }));
    expect(
      await screen.findByText("Enter six hexadecimal digits."),
    ).toBeVisible();
    expect(api.state.desired.rgb).toEqual(before);
  });
  it("keeps demo pairing out of real tab token storage", async () => {
    const { user } = await setup();
    const spy = vi.spyOn(Storage.prototype, "setItem");
    await user.click(screen.getByRole("button", { name: "Access ready" }));
    await user.type(
      screen.getByLabelText("Already have a token?"),
      "demo-only",
    );
    await user.click(screen.getByRole("button", { name: "Connect access" }));
    expect(spy).not.toHaveBeenCalled();
  });
  it("requires a matching independently entered digest before upload", async () => {
    const { user } = await setup((api) => {
      api.device.capabilities.ota = true;
    });
    await user.click(screen.getByRole("tab", { name: "System" }));
    const bytes = new Uint8Array(64);
    bytes[0] = 0xe9;
    const file = new File([bytes], "application.bin", {
      type: "application/octet-stream",
    });
    Object.defineProperty(file, "arrayBuffer", {
      value: async () => bytes.buffer,
    });
    await user.upload(
      screen.getByLabelText("Firmware application image"),
      file,
    );
    const update = await screen.findByRole("button", {
      name: "Verify & update",
    });
    expect(update).toBeDisabled();
    await user.type(
      screen.getByLabelText("Expected SHA-256 from your build or release"),
      "0".repeat(64),
    );
    expect(update).toBeDisabled();
    await user.clear(
      screen.getByLabelText("Expected SHA-256 from your build or release"),
    );
    await user.type(
      screen.getByLabelText("Expected SHA-256 from your build or release"),
      await sha256(bytes.buffer),
    );
    expect(update).toBeEnabled();
  });
});
