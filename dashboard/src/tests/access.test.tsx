import { render, screen, waitFor } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import { it, expect, vi } from "vitest";
import { WifiSetup, ClientAccess } from "../SystemAccess";
import { DemoTransport } from "../demo";
import { StudioStore, HttpTransport, sha256 } from "../api";

it("saves Wi-Fi only on explicit submit, sends exact SSID and clears the write-only password", async () => {
  const api = new DemoTransport(),
    store = new StudioStore(api),
    spy = vi.spyOn(api, "request"),
    user = userEvent.setup();
  render(<WifiSetup store={store} disabled={false} />);
  await user.click(screen.getByText("Change Wi-Fi network"));
  await user.type(
    screen.getByLabelText("Network name (SSID)"),
    "Studio network",
  );
  await user.type(
    screen.getByLabelText("New Wi-Fi password"),
    "test-network-secret",
  );
  expect(spy).not.toHaveBeenCalled();
  await user.click(
    screen.getByRole("button", { name: "Save Wi-Fi & restart light" }),
  );
  await screen.findByText(
    "Preview only: Wi-Fi save simulated. No network changed.",
  );
  expect(spy).toHaveBeenCalledWith(
    "PATCH",
    "/settings",
    { ssid: "Studio network", password: "test-network-secret" },
    undefined,
  );
  expect(screen.getByLabelText("New Wi-Fi password")).toHaveValue("");
  expect(await api.request("GET", "/settings")).not.toHaveProperty("password");
});
it("requires an explicit open-network choice", async () => {
  const api = new DemoTransport(),
    spy = vi.spyOn(api, "request"),
    user = userEvent.setup();
  render(<WifiSetup store={new StudioStore(api)} disabled={false} />);
  await user.click(screen.getByText("Change Wi-Fi network"));
  await user.type(screen.getByLabelText("Network name (SSID)"), "Visitor");
  await user.click(
    screen.getByRole("button", { name: "Save Wi-Fi & restart light" }),
  );
  expect(await screen.findByRole("alert")).toHaveTextContent("8–63");
  expect(spy).not.toHaveBeenCalled();
  await user.click(
    screen.getByRole("switch", { name: "This is an open network" }),
  );
  await user.click(
    screen.getByRole("button", { name: "Save Wi-Fi & restart light" }),
  );
  await waitFor(() =>
    expect(spy).toHaveBeenCalledWith(
      "PATCH",
      "/settings",
      { ssid: "Visitor", password: "" },
      undefined,
    ),
  );
});
it("does not fetch paired clients without access and never auto-revokes", async () => {
  const api = new HttpTransport(vi.fn()),
    spy = vi.spyOn(api, "request");
  render(
    <ClientAccess
      store={new StudioStore(api)}
      token=""
      disabled={false}
      onSelfRevoked={() => {}}
      onPair={() => {}}
    />,
  );
  expect(
    screen.getByText(
      "Connect a device token to view and manage paired clients.",
    ),
  ).toBeVisible();
  expect(spy).not.toHaveBeenCalled();
  expect(
    screen.queryByRole("button", { name: "Open pairing for another client" }),
  ).not.toBeInTheDocument();
});
it("requires deliberate revocation and clears this tab only after device confirmation", async () => {
  const api = new DemoTransport();
  api.clients = [
    {
      id: (await sha256(new TextEncoder().encode(api.token).buffer)).slice(
        0,
        16,
      ),
      label: "Studio browser",
    },
  ];
  const spy = vi.spyOn(api, "request"),
    self = vi.fn(),
    user = userEvent.setup();
  render(
    <ClientAccess
      store={new StudioStore(api)}
      token={api.token}
      disabled={false}
      onSelfRevoked={self}
      onPair={() => {}}
    />,
  );
  await screen.findByText("This tab");
  await user.click(
    screen.getByRole("button", { name: "Revoke Studio browser" }),
  );
  expect(self).not.toHaveBeenCalled();
  expect(spy.mock.calls.filter((c) => c[0] === "DELETE")).toHaveLength(0);
  await user.click(screen.getByRole("button", { name: "Keep access" }));
  expect(api.clients).toHaveLength(1);
  await user.click(
    screen.getByRole("button", { name: "Revoke Studio browser" }),
  );
  await user.click(screen.getByRole("button", { name: "Revoke access" }));
  await waitFor(() => expect(self).toHaveBeenCalledOnce());
  expect(api.clients).toHaveLength(0);
});
it("keeps access intact after an uncertain revocation and does not retry", async () => {
  const api = new DemoTransport(),
    original = api.request.bind(api),
    spy = vi
      .spyOn(api, "request")
      .mockImplementation(async (method, path, body) => {
        if (method === "DELETE")
          throw Error("Request timed out; inspect current access.");
        return original(method, path, body);
      }),
    self = vi.fn(),
    user = userEvent.setup();
  render(
    <ClientAccess
      store={new StudioStore(api)}
      token={api.token}
      disabled={false}
      onSelfRevoked={self}
      onPair={() => {}}
    />,
  );
  await screen.findByText("Preview browser");
  await user.click(
    screen.getByRole("button", { name: "Revoke Preview browser" }),
  );
  await user.click(screen.getByRole("button", { name: "Revoke access" }));
  await waitFor(() =>
    expect(screen.getAllByRole("alert")[0]).toHaveTextContent(
      "Request timed out",
    ),
  );
  expect(self).not.toHaveBeenCalled();
  expect(spy.mock.calls.filter((c) => c[0] === "DELETE")).toHaveLength(1);
});
it("lists and manages clients beyond the former four-client limit", async () => {
  const api = new DemoTransport(),
    user = userEvent.setup();
  api.clients = Array.from({ length: 40 }, (_, i) => ({
    id: i.toString(16).padStart(16, "0"),
    label: `Studio client ${i + 1}`,
  }));
  const spy = vi.spyOn(api, "request");
  render(
    <ClientAccess
      store={new StudioStore(api)}
      token={api.token}
      disabled={false}
      onSelfRevoked={() => {}}
      onPair={() => {}}
    />,
  );
  expect(await screen.findByText("40 paired clients")).toBeVisible();
  expect(
    screen.getAllByRole("button", { name: /^Revoke Studio client/ }),
  ).toHaveLength(40);
  await user.click(
    screen.getByRole("button", { name: "Revoke Studio client 40" }),
  );
  await user.click(screen.getByRole("button", { name: "Revoke access" }));
  expect(await screen.findByText("39 paired clients")).toBeVisible();
  expect(spy.mock.calls.filter((c) => c[0] === "DELETE")).toEqual([
    ["DELETE", "/clients/0000000000000027", undefined, undefined],
  ]);
});
it("allows only a bounded client ID path for authenticated revocation", async () => {
  const fetcher = vi.fn().mockResolvedValue(new Response('{"revoked":true}')),
    api = new HttpTransport(fetcher);
  api.token = "secret";
  await api.request("DELETE", "/clients/0123456789abcdef");
  expect(fetcher.mock.calls[0][0]).toBe("/api/v1/clients/0123456789abcdef");
  await expect(api.request("DELETE", "/clients/../../evil")).rejects.toThrow(
    "Invalid API path",
  );
  expect(fetcher).toHaveBeenCalledOnce();
});

it("opens one pairing window only after an explicit click and sends no body", async () => {
  const api = new DemoTransport(),
    spy = vi.spyOn(api, "request"),
    user = userEvent.setup();
  render(
    <ClientAccess
      store={new StudioStore(api)}
      token={api.token}
      disabled={false}
      onSelfRevoked={() => {}}
      onPair={() => {}}
    />,
  );
  await screen.findByText("Preview browser");
  expect(spy.mock.calls.filter((c) => c[0] === "POST")).toHaveLength(0);
  await user.click(
    screen.getByRole("button", { name: "Open pairing for another client" }),
  );
  await screen.findByText(
    "Preview only: pairing window simulated. No device access changed.",
  );
  expect(spy.mock.calls.filter((c) => c[0] === "POST")).toEqual([
    ["POST", "/pairing", undefined, undefined],
  ]);
});

it.each([
  { pairing_open: false, duration_ms: 180000 },
  { pairing_open: true, duration_ms: 0 },
  { pairing_open: true, duration_ms: 180001 },
])(
  "does not report a pairing window from an invalid response: %j",
  async (response) => {
    const api = new DemoTransport(),
      original = api.request.bind(api),
      user = userEvent.setup();
    const spy = vi
      .spyOn(api, "request")
      .mockImplementation(async (method, path, body) =>
        path === "/pairing" ? response : original(method, path, body),
      );
    render(
      <ClientAccess
        store={new StudioStore(api)}
        token={api.token}
        disabled={false}
        onSelfRevoked={() => {}}
        onPair={() => {}}
      />,
    );
    await user.click(
      screen.getByRole("button", { name: "Open pairing for another client" }),
    );
    expect(await screen.findByRole("alert")).toHaveTextContent(
      "did not confirm a bounded pairing window",
    );
    expect(
      screen.queryByText(/pairing window simulated/),
    ).not.toBeInTheDocument();
    expect(spy.mock.calls.filter((c) => c[0] === "POST")).toHaveLength(1);
  },
);

it("authenticates pairing-window requests without sending an empty JSON document", async () => {
  const fetcher = vi
      .fn()
      .mockResolvedValue(
        new Response('{"pairing_open":true,"duration_ms":180000}'),
      ),
    api = new HttpTransport(fetcher);
  api.token = "local-access-token";
  await api.request("POST", "/pairing");
  const [url, init] = fetcher.mock.calls[0];
  expect(url).toBe("/api/v1/pairing");
  expect(new Headers(init.headers).get("Authorization")).toBe(
    "Bearer local-access-token",
  );
  expect(init.body).toBeUndefined();
});

it("checks Wi-Fi network names by UTF-8 bytes before sending credentials", async () => {
  const api = new DemoTransport(),
    spy = vi.spyOn(api, "request"),
    user = userEvent.setup();
  render(<WifiSetup store={new StudioStore(api)} disabled={false} />);
  await user.click(screen.getByText("Change Wi-Fi network"));
  await user.type(screen.getByLabelText("Network name (SSID)"), "é".repeat(17));
  await user.type(screen.getByLabelText("New Wi-Fi password"), "test-password");
  await user.click(
    screen.getByRole("button", { name: "Save Wi-Fi & restart light" }),
  );
  expect(await screen.findByRole("alert")).toHaveTextContent(
    "1–32 UTF-8 bytes",
  );
  expect(spy).not.toHaveBeenCalled();
});
