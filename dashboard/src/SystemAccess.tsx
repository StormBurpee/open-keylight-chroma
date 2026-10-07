import { useEffect, useState } from "react";
import { KeyRound, RefreshCw, Trash2, Wifi } from "lucide-react";
import { Button } from "@/components/ui/button";
import { Input } from "@/components/ui/input";
import { Label } from "@/components/ui/label";
import { Switch } from "@/components/ui/switch";
import {
  Dialog,
  DialogContent,
  DialogHeader,
  DialogTitle,
  DialogDescription,
} from "@/components/ui/dialog";
import {
  message,
  sha256,
  type StudioStore,
  type PairedClient,
  type Settings,
} from "./api";

export function ColourRendering({
  encoding,
  disabled,
  save,
}: {
  encoding: NonNullable<Settings["output_encoding"]>;
  disabled: boolean;
  save: (value: NonNullable<Settings["output_encoding"]>) => Promise<void>;
}) {
  const [selected, setSelected] = useState(encoding);
  const [saving, setSaving] = useState(false);
  const [error, setError] = useState("");
  useEffect(() => setSelected(encoding), [encoding]);
  return (
    <form
      className="settings-form colour-rendering"
      onSubmit={async (event) => {
        event.preventDefault();
        if (disabled || saving || selected === encoding) return;
        setSaving(true);
        setError("");
        try {
          await save(selected);
        } catch (cause) {
          setError(message(cause));
        } finally {
          setSaving(false);
        }
      }}
    >
      <Label htmlFor="output-encoding">Colour rendering</Label>
      <select
        id="output-encoding"
        value={selected}
        disabled={disabled || saving}
        onChange={(event) => setSelected(event.target.value as typeof encoding)}
      >
        <option value="srgb">sRGB · recommended</option>
        <option value="linear">Linear · direct channel levels</option>
      </select>
      <p className="hint">
        sRGB interprets colours from the picker and hex codes. Linear uses
        direct channel levels. White temperature is unchanged.
      </p>
      {error && (
        <p role="alert" className="inline-error">
          {error}
        </p>
      )}
      <Button
        variant="outline"
        disabled={disabled || saving || selected === encoding}
      >
        {saving ? "Applying…" : "Apply colour rendering"}
      </Button>
    </form>
  );
}

export function WifiSetup({
  store,
  disabled,
}: {
  store: StudioStore;
  disabled: boolean;
}) {
  const [ssid, setSsid] = useState(""),
    [password, setPassword] = useState(""),
    [open, setOpen] = useState(false),
    [error, setError] = useState(""),
    [saved, setSaved] = useState(false);
  const save = async () => {
    setError("");
    setSaved(false);
    const bytes = new TextEncoder();
    if (
      !ssid ||
      bytes.encode(ssid).length > 32 ||
      /[\x00-\x1f\x7f]/.test(ssid)
    ) {
      setError(
        "Use a network name of 1–32 UTF-8 bytes without control characters.",
      );
      return;
    }
    const length = bytes.encode(password).length;
    if (
      !open &&
      (length < 8 ||
        length > 64 ||
        (length === 64 && !/^[0-9a-f]{64}$/i.test(password)) ||
        /[\x00-\x1f\x7f]/.test(password))
    ) {
      setError(
        "Use an 8–63 byte Wi-Fi password, or a 64-digit hexadecimal key.",
      );
      return;
    }
    try {
      await store.write(
        "PATCH",
        "/settings",
        { ssid, password: open ? "" : password },
        "Wi-Fi settings saved; the light is restarting.",
      );
      setPassword("");
      setSaved(true);
    } catch (e) {
      setError(message(e));
    }
  };
  return (
    <details className="setup-details">
      <summary>
        <Wifi size={15} />
        Change Wi-Fi network
      </summary>
      <form
        className="settings-form"
        onSubmit={(e) => {
          e.preventDefault();
          void save();
        }}
      >
        <p className="panel-copy">
          Use a 2.4 GHz network. Saving restarts the light and disconnects this
          page. Your saved password is never returned to this browser.
        </p>
        <Label htmlFor="wifi-ssid">Network name (SSID)</Label>
        <Input
          id="wifi-ssid"
          autoComplete="off"
          spellCheck={false}
          value={ssid}
          onChange={(e) => setSsid(e.target.value)}
          disabled={disabled}
        />
        <Label htmlFor="wifi-password">New Wi-Fi password</Label>
        <Input
          id="wifi-password"
          type="password"
          autoComplete="new-password"
          value={password}
          onChange={(e) => setPassword(e.target.value)}
          disabled={disabled || open}
        />
        <div className="open-network">
          <Switch
            id="wifi-open"
            checked={open}
            onCheckedChange={setOpen}
            disabled={disabled}
          />
          <Label htmlFor="wifi-open">This is an open network</Label>
        </div>
        <Button variant="outline" disabled={disabled || !ssid}>
          Save Wi-Fi & restart light
        </Button>
        {error && (
          <p role="alert" className="inline-error">
            {error}
          </p>
        )}
        {saved && (
          <p role="status" className="setup-success">
            {store.transport.demo
              ? "Preview only: Wi-Fi save simulated. No network changed."
              : "Network saved. Rejoin that Wi-Fi network, then open the light’s local address again."}
          </p>
        )}
      </form>
    </details>
  );
}

export function ClientAccess({
  store,
  token,
  disabled,
  onSelfRevoked,
  onPair,
}: {
  store: StudioStore;
  token: string;
  disabled: boolean;
  onSelfRevoked: () => void;
  onPair: () => void;
}) {
  const [clients, setClients] = useState<PairedClient[]>([]),
    [error, setError] = useState(""),
    [pairingNotice, setPairingNotice] = useState(""),
    [loading, setLoading] = useState(false),
    [currentId, setCurrentId] = useState(""),
    [selected, setSelected] = useState<PairedClient | null>(null);
  const load = async () => {
    if (!token && !store.transport.demo) {
      setClients([]);
      return;
    }
    setLoading(true);
    setError("");
    try {
      const result = await store.transport.request<{ clients: PairedClient[] }>(
        "GET",
        "/clients",
      );
      if (
        !Array.isArray(result.clients) ||
        result.clients.some(
          (c) =>
            !c || !/^[0-9a-f]{16}$/.test(c.id) || typeof c.label !== "string",
        )
      )
        throw Error("Invalid paired-client response.");
      setClients(result.clients);
    } catch (e) {
      setError(message(e));
    } finally {
      setLoading(false);
    }
  };
  useEffect(() => {
    let active = true;
    setCurrentId("");
    setPairingNotice("");
    void sha256(new TextEncoder().encode(token).buffer).then((hash) => {
      if (active) setCurrentId(hash.slice(0, 16));
    });
    void load();
    return () => {
      active = false;
    };
  }, [token, store]);
  const openPairing = async () => {
    setError("");
    setPairingNotice("");
    try {
      const result = await store.write<{
        pairing_open: boolean;
        duration_ms: number;
      }>("POST", "/pairing");
      if (
        result.pairing_open !== true ||
        !Number.isInteger(result.duration_ms) ||
        result.duration_ms <= 0 ||
        result.duration_ms > 180000
      )
        throw Error("The device did not confirm a bounded pairing window.");
      setPairingNotice(
        store.transport.demo
          ? "Preview only: pairing window simulated. No device access changed."
          : `Pairing window opened for up to ${Math.ceil(result.duration_ms / 1000)} seconds. It closes after one client pairs. Open the light’s address on the other client to request access.`,
      );
    } catch (e) {
      setError(message(e));
    }
  };
  const revoke = async () => {
    if (!selected) return;
    setError("");
    try {
      const result = await store.write<{ revoked: boolean }>(
        "DELETE",
        `/clients/${selected.id}`,
        undefined,
        "Client access revoked.",
      );
      if (result.revoked !== true)
        throw Error("The device did not confirm revocation.");
      setClients((list) => list.filter((c) => c.id !== selected.id));
      setSelected(null);
      if (selected.id === currentId) {
        setClients([]);
        onSelfRevoked();
      } else await load();
    } catch (e) {
      setError(message(e));
    }
  };
  return (
    <section className="system-panel access-panel">
      <div className="panel-heading">
        <KeyRound size={19} />
        <h3>Who can control your light</h3>
      </div>
      <p className="panel-copy">
        Up to four paired clients. Revoking access invalidates that client’s
        token. A trusted client or the physical button can open a new pairing
        window. This list never exposes tokens.
      </p>
      {token || store.transport.demo ? (
        <>
          <div className="client-toolbar">
            <span>
              {loading
                ? "Reading paired clients…"
                : `${clients.length} of 4 slots used`}
            </span>
            <Button
              variant="ghost"
              aria-label="Refresh paired clients"
              disabled={disabled || loading}
              onClick={() => void load()}
            >
              <RefreshCw size={14} />
            </Button>
          </div>
          <ul className="client-list">
            {clients.map((client) => (
              <li key={client.id}>
                <div>
                  <strong>{client.label}</strong>
                  <small>
                    {client.id === currentId
                      ? "This tab"
                      : `Client ${client.id.slice(0, 8)}`}
                  </small>
                </div>
                <Button
                  variant="ghost"
                  aria-label={`Revoke ${client.label}`}
                  disabled={disabled || loading}
                  onClick={() => setSelected(client)}
                >
                  <Trash2 size={16} />
                </Button>
              </li>
            ))}
          </ul>
          {!loading && !error && !clients.length && (
            <p className="panel-copy">No paired clients were reported.</p>
          )}
        </>
      ) : (
        <p className="panel-copy">
          Connect a device token to view and manage paired clients.
        </p>
      )}
      {error && (
        <p role="alert" className="inline-error">
          {error}
        </p>
      )}
      <div className="client-actions">
        {(token || store.transport.demo) && (
          <Button
            variant="outline"
            disabled={disabled}
            onClick={() => void openPairing()}
          >
            Open pairing for another client
          </Button>
        )}
        <Button variant="outline" onClick={onPair} disabled={disabled}>
          Pair or connect this tab
        </Button>
      </div>
      {pairingNotice && (
        <p role="status" className="setup-success">
          {pairingNotice}
        </p>
      )}
      <Dialog
        open={!!selected}
        onOpenChange={(value) => {
          if (!value) setSelected(null);
        }}
      >
        <DialogContent className="studio-dialog">
          <DialogHeader>
            <DialogTitle>Revoke {selected?.label}?</DialogTitle>
            <DialogDescription>
              {selected?.id === currentId
                ? "This tab will lose control immediately. Save any work before revoking your own access."
                : "That client will lose access immediately. Its existing token will no longer work."}{" "}
              Pair again through a trusted client or the light’s physical
              button.
            </DialogDescription>
          </DialogHeader>
          {error && (
            <p role="alert" className="inline-error">
              {error}
            </p>
          )}
          <div className="revoke-buttons">
            <Button
              variant="outline"
              disabled={disabled}
              onClick={() => setSelected(null)}
            >
              Keep access
            </Button>
            <Button
              className="revoke-button"
              disabled={disabled}
              onClick={() => void revoke()}
            >
              Revoke access
            </Button>
          </div>
        </DialogContent>
      </Dialog>
    </section>
  );
}
