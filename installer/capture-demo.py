#!/usr/bin/env python3
"""Render a captured Ink demo to PNG without a browser or device connection.

Run npm run bundle and npm run capture:demo first. Pillow is a development-only
dependency; the distributed installer does not use this script or Pillow.
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import re
import unicodedata


BACKGROUND = (234, 232, 224)
TERMINAL = (20, 35, 40)
INK = (237, 233, 223)
SGR = re.compile(r"\x1b\[([0-9;]*)m")


@dataclass(frozen=True)
class Cell:
    text: str
    colour: tuple[int, int, int]
    bold: bool


def parse_ansi(text: str) -> list[list[Cell]]:
    """Accept only the colour/weight commands emitted by capture-demo.mjs."""
    if len(text) > 32768:
        raise ValueError("Demo capture exceeds 32 KiB")
    text = text.replace("\r\n", "\n")
    lines: list[list[Cell]] = [[]]
    colour, bold, dim = INK, False, False
    offset = 0
    while offset < len(text):
        match = SGR.match(text, offset)
        if match:
            args = [int(value) for value in (match[1] or "0").split(";")]
            index = 0
            while index < len(args):
                code = args[index]
                if code == 0:
                    colour, bold, dim = INK, False, False
                elif code == 1:
                    bold = True
                elif code == 2:
                    dim = True
                elif code == 22:
                    bold, dim = False, False
                elif code == 39:
                    colour = INK
                elif code == 38 and args[index + 1:index + 2] == [2]:
                    rgb = args[index + 2:index + 5]
                    if len(rgb) != 3 or any(value > 255 for value in rgb):
                        raise ValueError("Invalid true-colour escape")
                    colour = tuple(rgb)
                    index += 4
                else:
                    raise ValueError(f"Unsupported terminal style: {code}")
                index += 1
            offset = match.end()
            continue
        char = text[offset]
        offset += 1
        if char == "\n":
            lines.append([])
        else:
            # Reject cursor movement, hidden controls and double-width glyphs:
            # silently flattening these would misrepresent terminal geometry.
            if unicodedata.category(char).startswith("C") or unicodedata.combining(char) \
                    or unicodedata.east_asian_width(char) in ("W", "F"):
                raise ValueError("Unsupported control or terminal glyph")
            visible = tuple(round(back + .65 * (front - back)) for back, front in zip(TERMINAL, colour)) if dim else colour
            lines[-1].append(Cell(char, visible, bold))
        if len(lines) > 80 or len(lines[-1]) > 160:
            raise ValueError("Demo capture exceeds the canvas limits")
    if lines[-1] == []:
        lines.pop()
    if not any(lines):
        raise ValueError("Empty demo capture")
    return lines


def render(ansi: Path, output: Path, font: Path, bold_font: Path, overwrite=False) -> dict:
    from PIL import Image, ImageDraw, ImageFont

    raw = ansi.read_bytes()
    lines = parse_ansi(raw.decode("utf-8"))
    text = "\n".join("".join(cell.text for cell in line) for line in lines)
    if "NO DEVICE ACTIVITY" not in text or "Illustrative" not in text:
        raise ValueError("Use a labelled Ink demonstration capture")
    regular = ImageFont.truetype(str(font), 20)
    heavy = ImageFont.truetype(str(bold_font), 20)
    for face, weight in ((regular, b"Regular"), (heavy, b"Bold")):
        try:
            names = face.get_variation_names()
        except OSError:
            names = []
        if weight in names:
            face.set_variation_by_name(weight)
    for face in (regular, heavy):
        if abs(face.getlength("W") - face.getlength("i")) > .01:
            raise ValueError("Choose monospaced terminal fonts")
        missing = face.getmask("\U0010ffff")
        for char in set(text) - {"\n"}:
            glyph = face.getmask(char)
            if glyph.size == missing.size and bytes(glyph) == bytes(missing):
                raise ValueError(f"Font lacks captured glyph U+{ord(char):04X}")
    cell_width = regular.getlength("M")
    if abs(heavy.getlength("M") - cell_width) > .01:
        raise ValueError("Regular and bold fonts must have the same cell width")
    line_height, margin, padding = 30, 64, 28
    terminal_width = round(max(map(len, lines)) * cell_width) + padding * 2
    terminal_height = len(lines) * line_height + 58 + padding * 2
    width, height = terminal_width + margin * 2, terminal_height + 160
    canvas = Image.new("RGB", (width, height), BACKGROUND)
    draw = ImageDraw.Draw(canvas)
    small = ImageFont.truetype(str(font), 15)
    left, top = margin, 66
    draw.text((left, 25), "OPEN KEYLIGHT CHROMA / GUIDED SETUP", font=small, fill=(88, 101, 101))
    draw.rounded_rectangle((left + 3, top + 8, left + terminal_width + 3, top + terminal_height + 8), radius=16, fill=(210, 212, 204))
    draw.rounded_rectangle((left, top, left + terminal_width, top + terminal_height), radius=16, fill=TERMINAL)
    draw.line((left, top + 49, left + terminal_width, top + 49), fill=(49, 70, 74))
    for index in range(3):
        x = left + 22 + index * 20
        draw.ellipse((x, top + 20, x + 8, top + 28), fill=(82, 99, 103))
    title = "OPEN KEYLIGHT INSTALLER"
    draw.text((left + (terminal_width - small.getlength(title)) / 2, top + 16), title, font=small, fill=(153, 171, 170))
    for row, line in enumerate(lines):
        for column, cell in enumerate(line):
            draw.text((left + padding + column * cell_width, top + 58 + padding + row * line_height),
                      cell.text, font=heavy if cell.bold else regular, fill=cell.colour)
    caption = "Actual Ink output · illustrative demonstration · no device activity"
    draw.text((left, top + terminal_height + 25), caption, font=small, fill=(88, 101, 101))
    with output.open("wb" if overwrite else "xb") as stream:
        canvas.save(stream, format="PNG")
    return {"origin": "Captured Ink ANSI, rendered directly with Pillow", "ansi_sha256": hashlib.sha256(raw).hexdigest(),
            "png_sha256": hashlib.sha256(output.read_bytes()).hexdigest(), "width": width, "height": height,
            "fonts": [list(regular.getname()), list(heavy.getname())],
            "device_operations": 0, "optical_evidence": False}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ansi", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--font", type=Path, required=True)
    parser.add_argument("--bold-font", type=Path, required=True)
    parser.add_argument("--overwrite", action="store_true")
    args = parser.parse_args()
    print(json.dumps(render(args.ansi, args.output, args.font, args.bold_font, args.overwrite), indent=2))
