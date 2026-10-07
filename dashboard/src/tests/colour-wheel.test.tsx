import { useState } from "react";
import { fireEvent, render, screen } from "@testing-library/react";
import { describe, expect, it, vi } from "vitest";
import { ColourWheel, hsvToRgb, rgbToHsv } from "../ColourWheel";
import type { RGB } from "../ColourWheel";

const red = { r: 255, g: 0, b: 0 };
function fixture(value: RGB = red, disabled = false, controlled = false) {
  const change = vi.fn(),
    commit = vi.fn();
  function Controlled() {
    const [rgb, setRGB] = useState(value);
    return (
      <ColourWheel
        value={rgb}
        disabled={disabled}
        onChange={(next) => {
          change(next);
          setRGB(next);
        }}
        onCommit={commit}
      />
    );
  }
  const result = render(
    controlled ? (
      <Controlled />
    ) : (
      <ColourWheel
        value={value}
        disabled={disabled}
        onChange={change}
        onCommit={commit}
      />
    ),
  );
  const hue = result.container.querySelector<HTMLElement>(
    '[data-colour-surface="hue"]',
  )!;
  const sv = result.container.querySelector<HTMLElement>(
    '[data-colour-surface="sv"]',
  )!;
  const box = {
    left: 20,
    top: 30,
    width: 200,
    height: 200,
    right: 220,
    bottom: 230,
    x: 20,
    y: 30,
    toJSON: () => ({}),
  };
  vi.spyOn(hue, "getBoundingClientRect").mockReturnValue(box);
  vi.spyOn(sv, "getBoundingClientRect").mockReturnValue(box);
  const capture = vi.spyOn(HTMLElement.prototype, "setPointerCapture");
  vi.spyOn(HTMLElement.prototype, "hasPointerCapture").mockReturnValue(true);
  const release = vi.spyOn(HTMLElement.prototype, "releasePointerCapture");
  return { ...result, hue, sv, change, commit, capture, release };
}
const pointer = (x: number, y: number, pointerId = 1) => ({
  clientX: x,
  clientY: y,
  pointerId,
  button: 0,
  isPrimary: true,
});

describe("RGB / HSV conversions", () => {
  it("maps primary and secondary colors and round-trips an RGB grid", () => {
    for (const [h, rgb] of [
      [0, red],
      [60, { r: 255, g: 255, b: 0 }],
      [120, { r: 0, g: 255, b: 0 }],
      [180, { r: 0, g: 255, b: 255 }],
      [240, { r: 0, g: 0, b: 255 }],
      [300, { r: 255, g: 0, b: 255 }],
    ] as const) {
      expect(hsvToRgb({ h, s: 1, v: 1 })).toEqual(rgb);
      expect(rgbToHsv(rgb)).toEqual({ h, s: 1, v: 1 });
    }
    for (let r = 0; r <= 255; r += 17)
      for (let g = 0; g <= 255; g += 17)
        for (let b = 0; b <= 255; b += 17) {
          expect(hsvToRgb(rgbToHsv({ r, g, b }))).toEqual({ r, g, b });
        }
  });
  it("preserves a supplied achromatic hue and clamps invalid channels", () => {
    expect(rgbToHsv({ r: 128, g: 128, b: 128 }, 270)).toEqual({
      h: 270,
      s: 0,
      v: 128 / 255,
    });
    expect(rgbToHsv({ r: 0, g: 0, b: 0 }, -30)).toEqual({ h: 330, s: 0, v: 0 });
    expect(hsvToRgb({ h: 720, s: 2, v: 3 })).toEqual(red);
    expect(hsvToRgb({ h: NaN, s: NaN, v: Infinity })).toEqual({
      r: 0,
      g: 0,
      b: 0,
    });
    expect(rgbToHsv({ r: 400, g: -10, b: NaN })).toEqual({ h: 0, s: 1, v: 1 });
  });
});

describe("ColourWheel pointer interaction", () => {
  it("captures one pointer, previews hue, and commits only the final pointer-up coordinates", () => {
    const f = fixture();
    fireEvent.pointerDown(f.hue, pointer(220, 130)); // 90 degrees, clockwise from top.
    expect(f.capture).toHaveBeenCalledWith(1);
    expect(f.change).toHaveBeenLastCalledWith({ r: 128, g: 255, b: 0 });
    expect(f.commit).not.toHaveBeenCalled();
    fireEvent.pointerMove(f.hue, pointer(120, 230));
    expect(f.change).toHaveBeenLastCalledWith({ r: 0, g: 255, b: 255 });
    fireEvent.pointerUp(f.hue, pointer(20, 130));
    expect(f.commit).toHaveBeenCalledExactlyOnceWith({ r: 128, g: 0, b: 255 });
    expect(f.release).toHaveBeenCalledWith(1);
    fireEvent.pointerUp(f.hue, pointer(120, 30));
    expect(f.commit).toHaveBeenCalledTimes(1);
  });
  it("uses exact SV corners and clamps captured drags outside the field", () => {
    const f = fixture();
    fireEvent.pointerDown(f.sv, pointer(120, 80));
    expect(f.change).toHaveBeenLastCalledWith({ r: 191, g: 96, b: 96 });
    fireEvent.pointerMove(f.sv, pointer(-100, -100));
    expect(f.change).toHaveBeenLastCalledWith({ r: 255, g: 255, b: 255 });
    fireEvent.pointerUp(f.sv, pointer(500, -100));
    expect(f.commit).toHaveBeenCalledExactlyOnceWith(red);
  });
  it("ignores secondary pointers and right clicks during an active gesture", () => {
    const f = fixture();
    fireEvent.pointerDown(f.hue, { ...pointer(120, 230), button: 2 });
    fireEvent.pointerDown(f.hue, { ...pointer(120, 230), isPrimary: false });
    expect(f.change).not.toHaveBeenCalled();
    fireEvent.pointerDown(f.hue, pointer(220, 130));
    fireEvent.pointerDown(f.sv, pointer(20, 230, 2));
    fireEvent.pointerMove(f.hue, pointer(20, 130, 2));
    fireEvent.pointerUp(f.hue, pointer(20, 130, 2));
    expect(f.commit).not.toHaveBeenCalled();
    fireEvent.pointerUp(f.hue, pointer(220, 130));
    expect(f.commit).toHaveBeenCalledExactlyOnceWith({ r: 128, g: 255, b: 0 });
  });
  it("rolls a cancelled preview back without committing", () => {
    const f = fixture();
    fireEvent.pointerDown(f.hue, pointer(120, 230));
    fireEvent.pointerCancel(f.hue, pointer(120, 230));
    expect(f.change).toHaveBeenLastCalledWith(red);
    expect(f.commit).not.toHaveBeenCalled();
    expect(screen.getByRole("slider", { name: "Hue" })).toHaveValue("0");
  });
  it("cancels on capture loss, and release after a completed gesture does not undo it", () => {
    const f = fixture();
    fireEvent.pointerDown(f.hue, pointer(120, 230));
    fireEvent.lostPointerCapture(f.hue, pointer(120, 230));
    expect(f.change).toHaveBeenLastCalledWith(red);
    expect(f.commit).not.toHaveBeenCalled();
    fireEvent.pointerDown(f.hue, pointer(120, 230));
    fireEvent.pointerUp(f.hue, pointer(120, 230));
    fireEvent.lostPointerCapture(f.hue, pointer(120, 230));
    expect(f.change).toHaveBeenLastCalledWith({ r: 0, g: 255, b: 255 });
    expect(f.commit).toHaveBeenCalledTimes(1);
  });
  it("releases capture on unmount without a late callback", () => {
    const f = fixture();
    fireEvent.pointerDown(f.sv, pointer(120, 130));
    const count = f.change.mock.calls.length;
    f.unmount();
    expect(f.change).toHaveBeenCalledTimes(count);
    expect(f.commit).not.toHaveBeenCalled();
    expect(f.release).toHaveBeenCalledWith(1);
  });
  it("ignores unavailable capture and zero-size geometry without invalid RGB", () => {
    const f = fixture();
    f.capture.mockImplementationOnce(() => {
      throw new Error("Pointer no longer active");
    });
    fireEvent.pointerDown(f.hue, pointer(120, 230));
    expect(f.change).not.toHaveBeenCalled();
    vi.mocked(f.sv.getBoundingClientRect).mockReturnValue({
      width: 0,
      height: 0,
    } as DOMRect);
    fireEvent.pointerDown(f.sv, pointer(120, 130));
    fireEvent.pointerUp(f.sv, pointer(120, 130));
    expect(f.commit).toHaveBeenCalledExactlyOnceWith(red);
  });
});

describe("ColourWheel accessible and controlled editing", () => {
  it("provides labeled native sliders with keyboard increments, endpoints and one commit per action", () => {
    const f = fixture();
    const h = screen.getByRole("slider", { name: "Hue" });
    expect(h).toHaveAttribute("aria-valuetext", "0°");
    fireEvent.keyDown(h, { key: "ArrowUp", shiftKey: true });
    expect(h).toHaveValue("10");
    fireEvent.keyDown(h, { key: "PageUp" });
    expect(h).toHaveValue("20");
    fireEvent.keyDown(h, { key: "End" });
    expect(h).toHaveValue("359");
    fireEvent.keyDown(h, { key: "Home" });
    expect(f.commit).toHaveBeenLastCalledWith(red);
    expect(f.commit).toHaveBeenCalledTimes(4);
    fireEvent.keyDown(h, { key: "Tab" });
    expect(f.commit).toHaveBeenCalledTimes(4);
    fireEvent.keyDown(screen.getByRole("slider", { name: "Saturation" }), {
      key: "Home",
    });
    expect(f.commit).toHaveBeenLastCalledWith({ r: 255, g: 255, b: 255 });
    fireEvent.keyDown(screen.getByRole("slider", { name: "Value" }), {
      key: "Home",
    });
    expect(f.commit).toHaveBeenLastCalledWith({ r: 0, g: 0, b: 0 });
  });
  it("commits range gestures at pointer-up and assistive input changes immediately", () => {
    const f = fixture();
    const input = screen.getByRole("slider", { name: "Saturation" });
    fireEvent.pointerDown(input, pointer(100, 20));
    fireEvent.change(input, { target: { value: "50" } });
    fireEvent.change(input, { target: { value: "25" } });
    expect(f.commit).not.toHaveBeenCalled();
    fireEvent.pointerUp(input, pointer(100, 20));
    expect(f.commit).toHaveBeenCalledExactlyOnceWith({
      r: 255,
      g: 191,
      b: 191,
    });
    fireEvent.change(input, { target: { value: "0" } });
    expect(f.commit).toHaveBeenLastCalledWith({ r: 255, g: 255, b: 255 });
    expect(f.commit).toHaveBeenCalledTimes(2);
  });
  it("preserves hue through gray and both hue and saturation through black parent echoes", () => {
    const f = fixture({ r: 0, g: 0, b: 255 }, false, true);
    const h = screen.getByRole("slider", { name: "Hue" });
    const s = screen.getByRole("slider", { name: "Saturation" });
    const v = screen.getByRole("slider", { name: "Value" });
    fireEvent.change(s, { target: { value: "0" } });
    expect(h).toHaveValue("240");
    fireEvent.change(h, { target: { value: "120" } });
    expect(h).toHaveValue("120");
    fireEvent.change(s, { target: { value: "80" } });
    fireEvent.change(v, { target: { value: "0" } });
    expect(h).toHaveValue("120");
    expect(s).toHaveValue("80");
    fireEvent.change(v, { target: { value: "100" } });
    expect(f.commit).toHaveBeenLastCalledWith({ r: 51, g: 255, b: 51 });
  });
  it("synchronizes external colors and cancels the stale in-progress gesture", () => {
    const f = fixture();
    fireEvent.pointerDown(f.hue, pointer(120, 230));
    f.rerender(
      <ColourWheel
        value={{ r: 0, g: 255, b: 0 }}
        disabled={false}
        onChange={f.change}
        onCommit={f.commit}
      />,
    );
    expect(screen.getByRole("slider", { name: "Hue" })).toHaveValue("120");
    fireEvent.pointerUp(f.hue, pointer(20, 130));
    expect(f.commit).not.toHaveBeenCalled();
  });
  it("disables every interaction and cancels a gesture if disabled mid-drag", () => {
    const f = fixture(red, true);
    for (const slider of screen.getAllByRole("slider"))
      expect(slider).toBeDisabled();
    fireEvent.pointerDown(f.sv, pointer(120, 130));
    fireEvent.keyDown(screen.getByRole("slider", { name: "Hue" }), {
      key: "End",
    });
    fireEvent.change(screen.getByRole("slider", { name: "Value" }), {
      target: { value: "0" },
    });
    expect(f.change).not.toHaveBeenCalled();
    expect(f.commit).not.toHaveBeenCalled();
    f.rerender(
      <ColourWheel
        value={red}
        disabled={false}
        onChange={f.change}
        onCommit={f.commit}
      />,
    );
    fireEvent.pointerDown(f.hue, pointer(120, 230));
    f.rerender(
      <ColourWheel
        value={red}
        disabled
        onChange={f.change}
        onCommit={f.commit}
      />,
    );
    fireEvent.pointerUp(f.hue, pointer(120, 230));
    expect(f.commit).not.toHaveBeenCalled();
    expect(f.change).toHaveBeenLastCalledWith(red);
  });
  it("lets Escape cancel a pointer preview from its focused keyboard control", () => {
    const f = fixture();
    fireEvent.pointerDown(f.hue, pointer(120, 230));
    const input = screen.getByRole("slider", { name: "Hue" });
    expect(input).toHaveFocus();
    fireEvent.keyDown(input, { key: "Escape" });
    fireEvent.pointerUp(f.hue, pointer(120, 230));
    expect(f.change).toHaveBeenLastCalledWith(red);
    expect(f.commit).not.toHaveBeenCalled();
  });
});
