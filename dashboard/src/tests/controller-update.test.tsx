import { render, screen, waitFor } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import { describe, it, expect, vi } from "vitest";
import {
  ControllerUpdate,
  inspectControllerPackage,
  type ControllerJob,
} from "../ControllerUpdate";
import { HttpTransport, StudioStore, sha256 } from "../api";
import { DemoTransport } from "../demo";

function idle(): ControllerJob {
  return {
    job_id: null,
    state: "idle",
    phase: "idle",
    result: null,
    target_sha256: null,
    program_blocks_acked: 0,
    readback_blocks_verified: 0,
    total_blocks: 448,
    commit_attempted: false,
    commit_delivery: "not_sent",
    quiet_completed: false,
    controller_confirmed: false,
    error: null,
  };
}
async function packageBytes() {
  const data = new ArrayBuffer(28736),
    bytes = new Uint8Array(data),
    view = new DataView(data);
  bytes.set([79, 75, 76, 67, 78, 88, 80, 0]);
  view.setUint16(8, 1);
  view.setUint16(10, 64);
  view.setUint32(12, 28672);
  view.setUint32(16, 0xbc40);
  bytes[20] = 1;
  bytes[22] = 2;
  bytes[25] = 1;
  const hash = await sha256(data.slice(64));
  bytes.set(
    hash.match(/../g)!.map((v) => parseInt(v, 16)),
    28,
  );
  return data;
}
function setup(job = idle(), pending = false) {
  const transport = new DemoTransport();
  transport.device.capabilities.controller_ota = true;
  transport.device.trial_pending = pending;
  const request = vi
    .spyOn(transport, "request")
    .mockImplementation(async (method, path) => {
      if (path === "/controller/update") {
        if (method === "POST") return { accepted: true, job_id: 4 };
        return job;
      }
      if (path === "/device") return transport.device;
      return transport.state;
    });
  const store = new StudioStore(transport);
  render(
    <ControllerUpdate
      store={store}
      device={transport.device}
      authorized
      busy={false}
    />,
  );
  return { store, request, user: userEvent.setup() };
}
async function select(user: ReturnType<typeof userEvent.setup>) {
  const data = await packageBytes();
  const file = new File([data], "controller.oklnxp", {
    type: "application/octet-stream",
  });
  Object.defineProperty(file, "arrayBuffer", { value: async () => data });
  await waitFor(() =>
    expect(screen.getByLabelText("Controller firmware package")).toBeEnabled(),
  );
  await user.upload(screen.getByLabelText("Controller firmware package"), file);
  await screen.findByLabelText("Expected controller package SHA-256");
  return data;
}

describe("controller package", () => {
  it("checks the exact format, board, ABI and complete body digest", async () => {
    const good = await packageBytes();
    expect((await inspectControllerPackage(good)).version).toBe("0.1.0.0");
    await expect(inspectControllerPackage(good.slice(1))).rejects.toThrow(
      "28,736",
    );
    for (const offset of [0, 8, 10, 12, 16, 20, 21, 22, 23, 60]) {
      const bad = good.slice(0);
      new Uint8Array(bad)[offset] ^= 4;
      await expect(inspectControllerPackage(bad)).rejects.toThrow("supported");
    }
    const damaged = good.slice(0);
    new Uint8Array(damaged)[28735] = 1;
    await expect(inspectControllerPackage(damaged)).rejects.toThrow("damaged");
  });
  it("allows only the exact controller path through the HTTP transport", async () => {
    const fetcher = vi
      .fn()
      .mockResolvedValue({ ok: true, json: async () => idle() });
    const transport = new HttpTransport(fetcher);
    await transport.request("GET", "/controller/update");
    expect(fetcher.mock.calls[0][0]).toBe("/api/v1/controller/update");
    await expect(
      transport.request("POST", "/controller/update/reset"),
    ).rejects.toThrow("Invalid API path");
    await expect(
      transport.request("GET", "/controller/update?retry=1"),
    ).rejects.toThrow("Invalid API path");
  });
});
describe("controller update panel", () => {
  it("requires a separately entered digest and keeps acceptance distinct from completion", async () => {
    const { user, request } = setup();
    const data = await select(user);
    const button = screen.getByRole("button", { name: "Update light engine" });
    expect(button).toBeDisabled();
    await user.type(
      screen.getByLabelText("Expected controller package SHA-256"),
      await sha256(data),
    );
    expect(button).toBeEnabled();
    await user.click(button);
    expect(
      await screen.findByText("Installing controller firmware"),
    ).toBeVisible();
    expect(
      screen.queryByText("Installed and verified"),
    ).not.toBeInTheDocument();
    expect(screen.getByLabelText("Controller firmware package")).toBeDisabled();
    expect(
      request.mock.calls.filter(
        ([method, path]) => method === "POST" && path === "/controller/update",
      ),
    ).toHaveLength(1);
  });
  it("keeps controller installation unavailable during an ESP trial", async () => {
    setup(idle(), true);
    expect(
      await screen.findByText("Ready for a controller package"),
    ).toBeVisible();
    expect(screen.getByLabelText("Controller firmware package")).toBeDisabled();
    expect(
      screen.getByText(/Confirm the current ESP firmware trial/),
    ).toBeVisible();
  });
  it("reports recovery without offering an automatic retry", async () => {
    const { request } = setup({
      ...idle(),
      job_id: 3,
      state: "recovery_required",
      error: "Commit outcome unresolved",
    });
    expect(await screen.findByText("Recovery required")).toBeVisible();
    expect(screen.getByText("Commit outcome unresolved")).toBeVisible();
    expect(screen.getByLabelText("Controller firmware package")).toBeDisabled();
    expect(request.mock.calls.every(([method]) => method === "GET")).toBe(true);
  });
  it("keeps a diagnostic trial distinct from a completed lighting installation", async () => {
    const { request } = setup({
      ...idle(),
      job_id: 3,
      state: "diagnostic_trial",
      program_blocks_acked: 448,
      readback_blocks_verified: 448,
      quiet_completed: true,
      commit_attempted: true,
      commit_delivery: "complete",
    });
    expect(
      await screen.findByText("Controller diagnostic trial"),
    ).toBeVisible();
    expect(screen.getByText(/Lighting remains unavailable/)).toBeVisible();
    expect(
      screen.queryByText("Installed and verified"),
    ).not.toBeInTheDocument();
    expect(screen.getByLabelText("Controller firmware package")).toBeDisabled();
    expect(request.mock.calls.every(([method]) => method === "GET")).toBe(true);
  });
  it("reports diagnostic completion without enabling ordinary lighting installation", async () => {
    setup({
      ...idle(),
      job_id: 3,
      state: "diagnostic_trial",
      resident_recovery_ready: true,
    });
    expect(
      await screen.findByText("Controller diagnostic complete"),
    ).toBeVisible();
    expect(
      screen.queryByText("Installed and verified"),
    ).not.toBeInTheDocument();
    expect(screen.getByLabelText("Controller firmware package")).toBeDisabled();
  });
  it("rejects completion without complete programming, verification and commit evidence", async () => {
    setup({
      ...idle(),
      job_id: 3,
      state: "completed",
      controller_confirmed: true,
    });
    expect(await screen.findByRole("alert")).toHaveTextContent(
      "incomplete controller update status",
    );
    expect(
      screen.queryByText("Installed and verified"),
    ).not.toBeInTheDocument();
    expect(screen.getByLabelText("Controller firmware package")).toBeDisabled();
  });
  it("shows completion only after controller startup confirmation", async () => {
    setup({
      ...idle(),
      job_id: 3,
      state: "completed",
      controller_confirmed: true,
      program_blocks_acked: 448,
      readback_blocks_verified: 448,
      quiet_completed: true,
      commit_attempted: true,
      commit_delivery: "complete",
    });
    expect(await screen.findByText("Installed and verified")).toBeVisible();
    expect(screen.getByText(/Choose a scene when/)).toBeVisible();
  });
  it("rejects an incomplete cached status instead of enabling installation", async () => {
    setup({ ...idle(), total_blocks: 0 });
    expect(await screen.findByRole("alert")).toHaveTextContent(
      "incomplete controller update status",
    );
    expect(screen.getByLabelText("Controller firmware package")).toBeDisabled();
  });
});
