"""Tests for the Auric app-icon module."""
import unittest

from compiler import icon


class IconTest(unittest.TestCase):
    def test_default_icon_is_128_bytes(self):
        self.assertEqual(len(icon.default_icon()), icon.ICON_BYTES)
        self.assertEqual(icon.ICON_BYTES, 128)

    def test_parse_empty_is_all_off(self):
        self.assertEqual(icon.parse_icon_text(""), bytes(icon.ICON_BYTES))

    def test_parse_top_left_pixel(self):
        # A single '#' at row 0, col 0 -> most-significant bit of byte 0.
        data = icon.parse_icon_text("#")
        self.assertEqual(data[0], 0x80)
        self.assertTrue(all(b == 0 for b in data[1:]))

    def test_parse_full_first_row(self):
        # 32 on-pixels across row 0 -> first 4 bytes all set.
        data = icon.parse_icon_text("#" * 32)
        self.assertEqual(data[0:4], b"\xff\xff\xff\xff")
        self.assertEqual(data[4], 0)

    def test_parse_bottom_right_pixel(self):
        rows = [""] * 31 + [" " * 31 + "#"]
        data = icon.parse_icon_text("\n".join(rows))
        # row 31, col 31 -> last byte, least-significant bit.
        self.assertEqual(data[icon.ICON_BYTES - 1], 0x01)

    def test_comments_are_skipped(self):
        text = "; a comment\n#\n; another"
        self.assertEqual(icon.parse_icon_text(text)[0], 0x80)

    def test_various_on_chars(self):
        for ch in "#*XO@8":
            self.assertEqual(icon.parse_icon_text(ch)[0], 0x80, ch)

    def test_emit_header_has_magic_and_branch(self):
        asm = icon.emit_header_asm(icon.default_icon())
        self.assertIn(".aurhead", asm)
        self.assertIn("b _start", asm)
        self.assertIn('"AURICON1"', asm)
        # 128 icon bytes emitted, 12 per line -> 11 .byte lines (128/12 rounded up)
        self.assertEqual(asm.count(".byte"), (icon.ICON_BYTES + 11) // 12)

    def test_emit_header_rejects_wrong_size(self):
        from compiler.errors import AuricError
        with self.assertRaises(AuricError):
            icon.emit_header_asm(b"\x00" * 64)


if __name__ == "__main__":
    unittest.main()
