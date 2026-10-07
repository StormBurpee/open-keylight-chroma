import { useEffect, useRef, useState } from "react";
import { CircuitBoard, Upload, Check, RefreshCw } from "lucide-react";
import { Button } from "./components/ui/button";
import { Input } from "./components/ui/input";
import { Label } from "./components/ui/label";
import { message, sha256, type Device, type StudioStore } from "./api";

export type ControllerJob = {
  job_id: number | null;
  state:
    | "idle"
    | "receiving"
    | "queued"
    | "running"
    | "completed"
    | "failed"
    | "recovery_required";
  phase: string;
  result: string | null;
  target_sha256: string | null;
  program_blocks_acked: number;
  readback_blocks_verified: number;
  total_blocks: number;
  commit_attempted: boolean;
  commit_delivery: "not_sent" | "complete" | "maybe_sent";
  quiet_completed: boolean;
  controller_confirmed: boolean;
  error: string | null;
};

function validJob(job: ControllerJob): boolean {
  return (
    !!job &&
    [
      "idle",
      "receiving",
      "queued",
      "running",
      "completed",
      "failed",
      "recovery_required",
    ].includes(job.state) &&
    (job.job_id === null || (Number.isInteger(job.job_id) && job.job_id > 0)) &&
    job.total_blocks === 448 &&
    [job.program_blocks_acked, job.readback_blocks_verified].every(
      (n) => Number.isInteger(n) && n >= 0 && n <= 448,
    ) &&
    typeof job.phase === "string" &&
    typeof job.controller_confirmed === "boolean" &&
    typeof job.commit_attempted === "boolean" &&
    typeof job.quiet_completed === "boolean" &&
    ["not_sent", "complete", "maybe_sent"].includes(job.commit_delivery)
  );
}

export async function inspectControllerPackage(data: ArrayBuffer) {
  if (data.byteLength !== 28736)
    throw Error("Choose a complete .oklnxp package (28,736 bytes).");
  const bytes = new Uint8Array(data),
    view = new DataView(data);
  if (
    [79, 75, 76, 67, 78, 88, 80, 0].some((v, i) => bytes[i] !== v) ||
    view.getUint16(8) !== 1 ||
    view.getUint16(10) !== 64 ||
    view.getUint32(12) !== 28672 ||
    view.getUint32(16) !== 0xbc40 ||
    bytes[20] !== 1 ||
    bytes[21] !== 0 ||
    bytes[22] !== 2 ||
    bytes[23] !== 0 ||
    !view.getUint32(24) ||
    view.getUint32(60) !== 0
  )
    throw Error("This is not a supported Open Keylight controller package.");
  const bankHash = [...bytes.slice(28, 60)]
    .map((v) => v.toString(16).padStart(2, "0"))
    .join("");
  if ((await sha256(data.slice(64))) !== bankHash)
    throw Error(
      "The controller package is damaged: its image digest does not match.",
    );
  return {
    data,
    digest: await sha256(data),
    version: [...bytes.slice(24, 28)].join("."),
  };
}

export function ControllerUpdate({
  store,
  device,
  authorized,
  busy,
}: {
  store: StudioStore;
  device: Device;
  authorized: boolean;
  busy: boolean;
}) {
  const [job, setJob] = useState<ControllerJob | null>(null);
  const [readError, setReadError] = useState("");
  const [error, setError] = useState("");
  const [file, setFile] = useState<
    | (Awaited<ReturnType<typeof inspectControllerPackage>> & { name: string })
    | null
  >(null);
  const [expected, setExpected] = useState("");
  const [uploading, setUploading] = useState(false);
  const input = useRef<HTMLInputElement>(null),
    reading = useRef(false),
    generation = useRef(0),
    selection = useRef(0);
  const enabled = !!device.capabilities.controller_ota;
  const active =
    !!job && ["receiving", "queued", "running"].includes(job.state);
  const unavailable =
    !authorized ||
    busy ||
    uploading ||
    active ||
    !job ||
    !!readError ||
    !!device.trial_pending ||
    device.controller.ready !== true ||
    job.state === "recovery_required";

  useEffect(() => {
    if (!enabled) return;
    let mounted = true;
    const poll = async () => {
      if (
        reading.current ||
        store.getSnapshot().busy ||
        document.visibilityState !== "visible"
      )
        return;
      reading.current = true;
      const current = generation.current;
      try {
        const value = await store.transport.request<ControllerJob>(
          "GET",
          "/controller/update",
        );
        if (!validJob(value))
          throw Error(
            "The device returned an incomplete controller update status.",
          );
        if (mounted && current === generation.current) {
          setJob(value);
          setReadError("");
        }
      } catch (failure) {
        if (mounted && current === generation.current)
          setReadError(message(failure));
      } finally {
        reading.current = false;
      }
    };
    void poll();
    const timer = setInterval(() => void poll(), 1000);
    return () => {
      mounted = false;
      clearInterval(timer);
      ++selection.current;
    };
  }, [enabled, store]);

  const select = async (candidate?: File) => {
    const current = ++selection.current;
    setFile(null);
    setExpected("");
    setError("");
    if (!candidate) return;
    try {
      if (candidate.size !== 28736)
        throw Error("Choose a complete .oklnxp package (28,736 bytes).");
      const prepared = await inspectControllerPackage(
        await candidate.arrayBuffer(),
      );
      if (current === selection.current)
        setFile({ ...prepared, name: candidate.name });
    } catch (failure) {
      if (current === selection.current) setError(message(failure));
    }
  };
  const upload = async () => {
    if (unavailable || !file || expected.trim().toLowerCase() !== file.digest)
      return;
    ++generation.current;
    setUploading(true);
    setError("");
    try {
      const result = await store.write<{ accepted: boolean; job_id: number }>(
        "POST",
        "/controller/update",
        file.data,
        "Controller package accepted. Follow its progress in System.",
        { "X-SHA256": file.digest },
      );
      if (
        result.accepted !== true ||
        !Number.isInteger(result.job_id) ||
        result.job_id < 1
      )
        throw Error(
          "Update acceptance is unclear. Inspect the job status before trying again.",
        );
      setJob({
        ...job!,
        job_id: result.job_id,
        state: "queued",
        phase: "validated",
        controller_confirmed: false,
        program_blocks_acked: 0,
        readback_blocks_verified: 0,
        error: null,
      });
      setFile(null);
      setExpected("");
      if (input.current) input.current.value = "";
    } catch (failure) {
      setError(message(failure));
    } finally {
      setUploading(false);
      void store.refresh();
    }
  };
  const progress = job
    ? ((job.program_blocks_acked + job.readback_blocks_verified) / 896) * 100
    : 0;
  return (
    <section className="system-panel firmware-panel controller-update-panel">
      <div className="panel-heading">
        <CircuitBoard size={19} />
        <h3>The light engine</h3>
        <span>NXP CONTROLLER</span>
      </div>
      <p className="panel-copy">
        Update the firmware that drives the LEDs. The light goes dark during
        installation; your dashboard stays here.
      </p>
      {!enabled ? (
        <p className="panel-copy">
          Controller updates are not advertised by this firmware.
        </p>
      ) : (
        <>
          {job && (
            <div className="controller-job" aria-live="polite">
              <div>
                <strong>
                  {job.state === "completed" && job.controller_confirmed
                    ? "Installed and verified"
                    : job.state === "recovery_required"
                      ? "Recovery required"
                      : active
                        ? "Installing controller firmware"
                        : "Ready for a controller package"}
                </strong>
                <span>
                  {active
                    ? job.phase.replaceAll("_", " ")
                    : device.controller.version}
                </span>
              </div>
              {active && (
                <>
                  <progress
                    aria-label="Controller image transfer and verification"
                    value={progress}
                    max={100}
                  />
                  <p>
                    {job.program_blocks_acked} / 448 blocks written ·{" "}
                    {job.readback_blocks_verified} / 448 verified
                  </p>
                  <p>
                    {job.phase === "quiet"
                      ? "Allowing installation to finish. Keep power connected."
                      : "A verified transfer is followed by controller startup checks."}
                  </p>
                </>
              )}
              {job.state === "completed" && job.controller_confirmed && (
                <p>
                  <Check size={14} /> Controller startup confirmed. Choose a
                  scene when you’re ready.
                </p>
              )}
              {job.error && <p className="inline-error">{job.error}</p>}
            </div>
          )}
          {device.trial_pending && (
            <p className="panel-copy">
              Confirm the current ESP firmware trial before updating the light
              engine.
            </p>
          )}
          <input
            ref={input}
            type="file"
            accept=".oklnxp,application/octet-stream"
            className="file-input"
            aria-label="Controller firmware package"
            disabled={unavailable}
            onChange={(event) => void select(event.target.files?.[0])}
          />
          <button
            className="upload-zone"
            disabled={unavailable}
            onClick={() => input.current?.click()}
          >
            <Upload size={22} />
            <strong>{file?.name || "Choose a controller package"}</strong>
            <span>
              {file
                ? `Version ${file.version} · package digest verified`
                : "Open Keylight .oklnxp · 28 KiB image"}
            </span>
          </button>
          {file && (
            <div className="digest-review">
              <Label>Computed package SHA-256</Label>
              <code>{file.digest}</code>
              <Label htmlFor="controller-expected-hash">
                Expected controller package SHA-256
              </Label>
              <Input
                id="controller-expected-hash"
                value={expected}
                spellCheck={false}
                onChange={(event) => setExpected(event.target.value)}
              />
              <Button
                className="primary-button"
                disabled={
                  unavailable || expected.trim().toLowerCase() !== file.digest
                }
                onClick={() => void upload()}
              >
                {uploading ? <RefreshCw size={14} /> : <Upload size={14} />}{" "}
                {uploading ? "Uploading controller…" : "Update light engine"}
              </Button>
              <p>
                Use the package and digest from your trusted build. Keep power
                connected. Installation does not restore the previous scene
                automatically.
              </p>
            </div>
          )}
          {readError && (
            <p className="inline-error" role="alert">
              Update status unavailable: {readError}
            </p>
          )}
          {error && (
            <p className="inline-error" role="alert">
              {error}
            </p>
          )}
        </>
      )}
    </section>
  );
}
