import { useEffect, useId, useRef, useState } from "react";
import type { CSSProperties, KeyboardEvent, PointerEvent } from "react";
import "./colour-wheel.css";

export type RGB = { r: number; g: number; b: number };
export type HSV = { h: number; s: number; v: number };
export type ColourWheelProps = {
  value: RGB;
  disabled: boolean;
  onChange: (rgb: RGB) => void;
  onCommit: (rgb: RGB) => void;
  onCancel?: () => void;
  compact?: boolean;
};

const clamp = (n: number, max = 1) =>
  Math.min(max, Math.max(0, Number.isFinite(n) ? n : 0));
const hue = (n: number) => (((Number.isFinite(n) ? n : 0) % 360) + 360) % 360;
const sameRGB = (a: RGB, b: RGB) => a.r === b.r && a.g === b.g && a.b === b.b;

/** HSV uses degrees and unit saturation/value; RGB uses 0..255 channels. */
export function rgbToHsv(rgb: RGB, achromaticHue = 0): HSV {
  const r = clamp(rgb.r, 255) / 255;
  const g = clamp(rgb.g, 255) / 255;
  const b = clamp(rgb.b, 255) / 255;
  const max = Math.max(r, g, b),
    min = Math.min(r, g, b),
    delta = max - min;
  let h = hue(achromaticHue);
  if (delta) {
    if (max === r) h = hue(60 * ((g - b) / delta));
    else if (max === g) h = 60 * ((b - r) / delta + 2);
    else h = 60 * ((r - g) / delta + 4);
  }
  return { h, s: max ? delta / max : 0, v: max };
}

export function hsvToRgb(hsv: HSV): RGB {
  const h = hue(hsv.h) / 60,
    s = clamp(hsv.s),
    v = clamp(hsv.v);
  const c = v * s,
    x = c * (1 - Math.abs((h % 2) - 1)),
    m = v - c;
  const channels =
    h < 1
      ? [c, x, 0]
      : h < 2
        ? [x, c, 0]
        : h < 3
          ? [0, c, x]
          : h < 4
            ? [0, x, c]
            : h < 5
              ? [x, 0, c]
              : [c, 0, x];
  return {
    r: Math.round((channels[0] + m) * 255),
    g: Math.round((channels[1] + m) * 255),
    b: Math.round((channels[2] + m) * 255),
  };
}

type Channel = "h" | "s" | "v";
type Gesture = {
  id: number;
  element: HTMLElement;
  kind: "hue" | "sv" | "range";
  start: HSV;
};

export function ColourWheel({
  value,
  disabled,
  onChange,
  onCommit,
  onCancel,
  compact = false,
}: ColourWheelProps) {
  const [selected, setSelected] = useState<HSV>(() => rgbToHsv(value));
  const current = useRef(selected);
  const lastEmitted = useRef(value);
  const gesture = useRef<Gesture | null>(null);
  const callbacks = useRef({ onChange, onCommit, onCancel });
  const hueInput = useRef<HTMLInputElement>(null);
  const saturationInput = useRef<HTMLInputElement>(null);
  const id = useId();
  callbacks.current = { onChange, onCommit, onCancel };

  const release = (active: Gesture) => {
    try {
      if (active.element.hasPointerCapture(active.id))
        active.element.releasePointerCapture(active.id);
    } catch {
      /* The browser may already have released a cancelled pointer. */
    }
  };
  const publish = (next: HSV, commit = false) => {
    current.current = next;
    setSelected(next);
    const rgb = hsvToRgb(next);
    if (!sameRGB(rgb, lastEmitted.current)) {
      lastEmitted.current = rgb;
      callbacks.current.onChange(rgb);
    }
    if (commit) callbacks.current.onCommit(rgb);
  };
  const cancel = (notify: boolean) => {
    const active = gesture.current;
    if (!active) return;
    gesture.current = null;
    release(active);
    if (notify) {
      publish(active.start);
      callbacks.current.onCancel?.();
    }
  };

  useEffect(() => {
    if (disabled) cancel(true);
    // Ignore our parent's RGB echo: black/gray does not encode the chosen hue,
    // and black also loses saturation. Keep that editing intent locally.
    if (!sameRGB(value, lastEmitted.current)) {
      cancel(false);
      const next = rgbToHsv(value, current.current.h);
      if (next.v === 0) next.s = current.current.s;
      current.current = next;
      lastEmitted.current = value;
      setSelected(next);
    }
  }, [value.r, value.g, value.b, disabled]);

  useEffect(
    () => () => {
      const active = gesture.current;
      gesture.current = null;
      if (active) release(active); // Unmount never commits or calls onChange.
    },
    [],
  );

  const sample = (event: PointerEvent<HTMLElement>, kind: "hue" | "sv") => {
    const rect = event.currentTarget.getBoundingClientRect();
    if (!rect.width || !rect.height) return current.current;
    if (kind === "hue") {
      const dx = event.clientX - rect.left - rect.width / 2;
      const dy = event.clientY - rect.top - rect.height / 2;
      if (!dx && !dy) return current.current;
      return {
        ...current.current,
        h: hue((Math.atan2(dy, dx) * 180) / Math.PI + 90),
      };
    }
    return {
      ...current.current,
      s: clamp((event.clientX - rect.left) / rect.width),
      v: 1 - clamp((event.clientY - rect.top) / rect.height),
    };
  };
  const start = (event: PointerEvent<HTMLElement>, kind: Gesture["kind"]) => {
    if (
      disabled ||
      event.button !== 0 ||
      event.isPrimary === false ||
      gesture.current
    )
      return;
    if (kind !== "range") event.preventDefault();
    try {
      event.currentTarget.setPointerCapture(event.pointerId);
    } catch {
      return;
    }
    gesture.current = {
      id: event.pointerId,
      element: event.currentTarget,
      kind,
      start: { ...current.current },
    };
    if (kind !== "range") {
      (kind === "hue" ? hueInput : saturationInput).current?.focus({
        preventScroll: true,
      });
      publish(sample(event, kind));
    }
  };
  const move = (event: PointerEvent<HTMLElement>) => {
    const active = gesture.current;
    if (
      disabled ||
      !active ||
      active.id !== event.pointerId ||
      active.kind === "range"
    )
      return;
    publish(sample(event, active.kind));
  };
  const finish = (event: PointerEvent<HTMLElement>) => {
    const active = gesture.current;
    if (disabled || !active || active.id !== event.pointerId) return;
    const next =
      active.kind === "range" ? current.current : sample(event, active.kind);
    gesture.current = null;
    release(active);
    publish(next, true);
  };
  const cancelled = (event: PointerEvent<HTMLElement>) => {
    if (gesture.current?.id === event.pointerId) cancel(true);
  };
  const channelChange = (channel: Channel, n: number, commit: boolean) => {
    if (disabled) return;
    publish(
      {
        ...current.current,
        [channel]: channel === "h" ? hue(n) : clamp(n / 100),
      },
      commit,
    );
  };
  const key = (event: KeyboardEvent<HTMLInputElement>, channel: Channel) => {
    if (disabled) return;
    if (event.key === "Escape" && gesture.current) {
      event.preventDefault();
      cancel(true);
      return;
    }
    const max = channel === "h" ? 359 : 100;
    const value =
      channel === "h" ? current.current.h : current.current[channel] * 100;
    const step = event.shiftKey ? 10 : 1;
    const changes: Record<string, number> = {
      ArrowRight: value + step,
      ArrowUp: value + step,
      ArrowLeft: value - step,
      ArrowDown: value - step,
      PageUp: value + 10,
      PageDown: value - 10,
      Home: 0,
      End: max,
    };
    if (!(event.key in changes)) return;
    event.preventDefault();
    cancel(false);
    channelChange(channel, clamp(changes[event.key], max), true);
  };
  const angle = (selected.h * Math.PI) / 180;
  const style = { "--wheel-hue": selected.h } as CSSProperties;
  const pointerHandlers = {
    onPointerMove: move,
    onPointerUp: finish,
    onPointerCancel: cancelled,
    onLostPointerCapture: cancelled,
  };

  return (
    <fieldset className="colour-wheel" disabled={disabled} style={style}>
      <legend className="colour-wheel-sr-only">Colour selection</legend>
      <div className="colour-wheel-disc" aria-hidden="true">
        <div
          className="colour-wheel-ring"
          data-colour-surface="hue"
          onPointerDown={(event) => start(event, "hue")}
          {...pointerHandlers}
        />
        <div className="colour-wheel-well" />
        <span
          className="colour-wheel-handle colour-wheel-hue-handle"
          style={{
            left: `${50 + Math.sin(angle) * 46}%`,
            top: `${50 - Math.cos(angle) * 46}%`,
          }}
        />
        <div
          className="colour-wheel-sv"
          data-colour-surface="sv"
          onPointerDown={(event) => start(event, "sv")}
          {...pointerHandlers}
        >
          <span
            className="colour-wheel-handle"
            style={{
              left: `${selected.s * 100}%`,
              top: `${(1 - selected.v) * 100}%`,
            }}
          />
        </div>
      </div>
      <details
        className="colour-wheel-details"
        open={compact ? undefined : true}
      >
        <summary hidden={!compact}>Fine adjustment</summary>
        <div className="colour-wheel-controls">
          {(
            [
              ["h", "Hue", "°"],
              ["s", "Saturation", "%"],
              ["v", "Value", "%"],
            ] as const
          ).map(([channel, label, unit]) => {
            const amount =
              channel === "h"
                ? Math.round(selected.h) % 360
                : Math.round(selected[channel] * 100);
            return (
              <div className="colour-wheel-control" key={channel}>
                <label htmlFor={`${id}-${channel}`}>{label}</label>
                <input
                  id={`${id}-${channel}`}
                  ref={
                    channel === "h"
                      ? hueInput
                      : channel === "s"
                        ? saturationInput
                        : undefined
                  }
                  type="range"
                  min={0}
                  max={channel === "h" ? 359 : 100}
                  step={1}
                  value={channel === "h" ? amount % 360 : amount}
                  aria-valuetext={`${amount}${unit}`}
                  onKeyDown={(event) => key(event, channel)}
                  onChange={(event) =>
                    channelChange(
                      channel,
                      Number(event.currentTarget.value),
                      !gesture.current,
                    )
                  }
                  onPointerDown={(event) => start(event, "range")}
                  {...pointerHandlers}
                />
                <output htmlFor={`${id}-${channel}`}>
                  {amount}
                  {unit}
                </output>
              </div>
            );
          })}
        </div>
      </details>
    </fieldset>
  );
}
