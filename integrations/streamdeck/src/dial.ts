import { rotateColor, type Intent, type State } from "./client.js";
export type Axis = "brightness" | "hue";
/** Constant-space composition preserves reversals after saturation, unlike summing ticks. */
export class Motion {
  private offset = 0;
  private low = 0;
  private high = 100;
  constructor(readonly axis: Axis) {}
  add(delta: number) {
    if (!Number.isSafeInteger(delta)) return;
    if (this.axis === "hue") {
      this.offset = (this.offset + (delta % 360)) % 360;
      return;
    }
    delta = Math.max(-100, Math.min(100, delta));
    this.low = Math.max(0, Math.min(100, this.low + delta));
    this.high = Math.max(0, Math.min(100, this.high + delta));
    this.offset = this.low === this.high ? 0 : this.offset + delta;
  }
  intent(state: State): Intent {
    return this.axis === "hue"
      ? { kind: "hue", degrees: this.offset }
      : {
          kind: "brightness",
          delta:
            Math.max(
              this.low,
              Math.min(this.high, state.desired.brightness + this.offset),
            ) - state.desired.brightness,
        };
  }
  preview(state: State): State {
    const intent = this.intent(state),
      desired = { ...state.desired };
    if (intent.kind === "brightness") desired.brightness += intent.delta;
    if (intent.kind === "hue") {
      desired.rgb = rotateColor(desired.rgb, intent.degrees);
      desired.mode = "color";
      desired.effect = "none";
    }
    return { ...state, desired, operation: { status: "pending" } };
  }
}
