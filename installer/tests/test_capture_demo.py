"""Offline terminal capture geometry/style checks; no Pillow required."""
import importlib.util
from pathlib import Path
import sys
import unittest

path = Path(__file__).resolve().parents[1] / "capture-demo.py"
spec = importlib.util.spec_from_file_location("capture_demo", path)
capture = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = capture
spec.loader.exec_module(capture)


class Tests(unittest.TestCase):
    def test_preserves_geometry_and_unicode_frames(self):
        lines = capture.parse_ansi(" ╭──╮\r\n │✓ │\r\n ╰──╯\r\n")
        self.assertEqual(["".join(cell.text for cell in line) for line in lines], [" ╭──╮", " │✓ │", " ╰──╯"])

    def test_colour_bold_dim_and_reset(self):
        cells = capture.parse_ansi("\x1b[38;2;164;230;213m\x1b[1mA\x1b[2mB\x1b[22mC\x1b[39mD\x1b[0mE")[0]
        self.assertEqual(cells[0], capture.Cell("A", (164, 230, 213), True))
        self.assertNotEqual(cells[1].colour, cells[0].colour)
        self.assertTrue(cells[1].bold)
        self.assertEqual(cells[2], capture.Cell("C", (164, 230, 213), False))
        self.assertEqual(cells[3].colour, capture.INK)
        self.assertEqual(cells[4], capture.Cell("E", capture.INK, False))

    def test_refuses_hidden_cursor_moves_bad_styles_and_wide_glyphs(self):
        for value in ("\x1b[2J", "\x1b[8msecret", "\x1b[38;2;999;0;0mx", "\x1b[38;2;1;2mx", "\t", "\r", "x\u202ey", "界", "e\u0301"):
            with self.subTest(value=repr(value)), self.assertRaises(ValueError):
                capture.parse_ansi(value)

    def test_refuses_empty_or_unbounded_capture(self):
        for value in ("", "x" * 161, "\n" * 81, "x" * 32769):
            with self.subTest(length=len(value)), self.assertRaises(ValueError):
                capture.parse_ansi(value)


if __name__ == "__main__":
    unittest.main()
