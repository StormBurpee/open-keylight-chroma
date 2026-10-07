import { rotateColor, type Intent, type State } from "./client.js";
export type Axis = "brightness" | "hue" | "temperature";
/** Constant-space composition preserves reversals after saturation, unlike summing ticks. */
export class Motion {
  private offset = 0;
  private low: number;
  private high: number;
  private readonly minimum: number;
  private readonly maximum: number;
  constructor(readonly axis: Axis) {
    this.low = this.minimum = axis === "temperature" ? 3000 : 0;
    this.high = this.maximum = axis === "temperature" ? 7000 : 100;
  }
  add(delta: number) {
    if (!Number.isSafeInteger(delta)) return;
    if (this.axis === "hue") {
      this.offset = (this.offset + (delta % 360)) % 360;
      return;
    }
    const span = this.maximum - this.minimum;
    delta = Math.max(-span, Math.min(span, delta));
    this.low = Math.max(this.minimum, Math.min(this.maximum, this.low + delta));
    this.high = Math.max(
      this.minimum,
      Math.min(this.maximum, this.high + delta),
    );
    this.offset = this.low === this.high ? 0 : this.offset + delta;
  }
  intent(state: State): Intent {
    if (this.axis === "temperature")
      return {
        kind: "temperature",
        kelvin: Math.max(
          this.low,
          Math.min(this.high, state.desired.temperature_k + this.offset),
        ),
      };
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
    if (intent.kind === "temperature") {
      desired.temperature_k = intent.kelvin;
      desired.mode = "white";
      desired.effect = "none";
      desired.transition_ms = 0;
    }
    return { ...state, desired, operation: { status: "pending" } };
  }
}
