import {
  DeviceClient,
  DeviceError,
  type Intent,
  type State,
} from "./client.js";
import { Motion, type Axis } from "./dial.js";
type Task = {
  owner: object;
  client: DeviceClient;
  motion?: Motion;
  intent?: Intent;
  cancelled: boolean;
  sealed: boolean;
  started?: boolean;
  expected?: number;
  target?: State;
};
export type ControlEvents = {
  changed(state: State): void;
  failed(owner: object, error: unknown): void;
  pending(owner: object, state?: State): void;
};
/** One device, one mutation in flight, one bounded pending intent. Never retries a write. */
export class DeviceControl {
  private active?: Task;
  private next?: Task;
  private timer?: ReturnType<typeof setTimeout>;
  private lastWrite = 0;
  private state?: State;
  generation = 0;
  constructor(
    private events: ControlEvents,
    private delay = 60,
    private interval = 90,
  ) {}
  get busy() {
    return !!(this.active || this.next);
  }
  observe(state: State) {
    if (!this.busy) this.state = state;
  }
  motion(owner: object, client: DeviceClient, axis: Axis, delta: number) {
    if (!Number.isSafeInteger(delta) || delta === 0) return;
    if (this.next?.intent)
      throw new DeviceError("A control is already waiting.", 429);
    let task =
      this.active &&
      !this.active.sealed &&
      !this.active.cancelled &&
      this.active.owner === owner &&
      this.active.motion?.axis === axis
        ? this.active
        : this.next;
    if (task && (task.owner !== owner || task.motion?.axis !== axis))
      throw new DeviceError("Another dial is controlling this light.", 429);
    if (!task)
      this.next = task = {
        owner,
        client,
        motion: new Motion(axis),
        cancelled: false,
        sealed: false,
      };
    task.motion!.add(delta);
    this.generation++;
    this.preview();
    this.schedule(this.delay);
  }
  command(owner: object, client: DeviceClient, intent: Intent) {
    if (this.next?.intent || this.active?.intent)
      throw new DeviceError("A control is already running.", 429);
    if (this.active && !this.active.started) this.active.cancelled = true;
    this.next = { owner, client, intent, cancelled: false, sealed: false };
    this.generation++;
    this.events.pending(owner);
    this.schedule(0);
  }
  cancel(owner: object) {
    if (this.active?.owner === owner) this.active.cancelled = true;
    if (this.next?.owner === owner) this.next = undefined;
    this.generation++;
    if (!this.next && this.timer) {
      clearTimeout(this.timer);
      this.timer = undefined;
    }
  }
  private preview() {
    let state = this.active?.target ?? this.state;
    if (!state) {
      if (this.next) this.events.pending(this.next.owner);
      return;
    }
    if (this.active?.motion && !this.active.target && !this.active.cancelled)
      state = this.active.motion.preview(state);
    if (this.next?.motion) state = this.next.motion.preview(state);
    this.events.pending((this.next ?? this.active)!.owner, state);
  }
  private schedule(delay: number) {
    if (this.active || this.timer || !this.next) return;
    this.timer = setTimeout(
      () => {
        this.timer = undefined;
        void this.run();
      },
      Math.max(delay, this.lastWrite + this.interval - Date.now()),
    );
  }
  private queued(): Task | undefined {
    return this.next;
  }
  private async run() {
    if (this.active || !this.next) return;
    const task = (this.active = this.next);
    this.next = undefined;
    try {
      const state = await task.client.state();
      if (task.cancelled) return;
      if (task.expected !== undefined && state.revision !== task.expected)
        throw new DeviceError(
          "State changed during this dial gesture. Turn again after checking the light.",
          409,
        );
      this.state = state;
      task.sealed = true;
      const intent = task.motion?.intent(state) ?? task.intent!;
      const patch = await task.client.prepare(state, intent);
      if (task.cancelled) return;
      const { expected_revision: _revision, ...values } = patch;
      task.target = {
        ...state,
        desired: { ...state.desired, ...values },
        operation: { status: "pending" },
      };
      this.preview();
      this.lastWrite = Date.now();
      task.started = true;
      const accepted = await task.client.mutate(state, patch);
      this.state = accepted;
      const queued = this.queued();
      if (
        queued?.motion &&
        queued.owner === task.owner &&
        queued.motion.axis === task.motion?.axis
      )
        queued.expected = accepted.revision;
      this.events.changed(accepted);
      if (queued) this.preview();
    } catch (error) {
      const waiting = this.queued();
      this.next = undefined;
      if (!task.cancelled) this.events.failed(task.owner, error);
      if (waiting && waiting.owner !== task.owner)
        this.events.failed(waiting.owner, error);
    } finally {
      this.active = undefined;
      this.generation++;
      this.schedule(0);
    }
  }
}
