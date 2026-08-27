"""End-to-end tests: pack round-trip, and (when the ARM toolchain is present)
a full compile of hello.aur to a bootable AUR1 container.

The full-build test is skipped automatically if arm-none-eabi-gcc is not on
PATH, so the front-end tests still run anywhere.
"""
import shutil
import struct
import sys
import tempfile
import unittest
from pathlib import Path

AURIC_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(AURIC_ROOT / "tools"))
import aur_pack  # noqa: E402

from compiler import aurc  # noqa: E402

HAVE_GCC = shutil.which(aurc.CC) is not None


class PackTest(unittest.TestCase):
    def test_pack_roundtrip(self):
        with tempfile.TemporaryDirectory() as d:
            payload = Path(d) / "p.bin"
            payload.write_bytes(b"\xAA" * 64)
            out = Path(d) / "out.bin"
            aur_pack.pack(payload, out, 0x22000000, 0x22000000, b"AUR1")

            data = out.read_bytes()
            self.assertEqual(data[:4], b"AUR1")
            fields = struct.unpack(aur_pack.HEADER_FMT, data[:36])
            magic, a9_off, a9_size, a9_load, a9_entry, *arm11 = fields
            self.assertEqual(a9_off, 36)
            self.assertEqual(a9_size, 64)
            self.assertEqual(a9_load, 0x22000000)
            self.assertEqual(a9_entry, 0x22000000)
            self.assertEqual(arm11, [0, 0, 0, 0])  # ARM11 fields zeroed
            self.assertEqual(data[36:], b"\xAA" * 64)

    def test_layout_matches_aos1(self):
        # AUR1 must share AOS1's 36-byte header size/format exactly.
        self.assertEqual(aur_pack.HEADER_SIZE, 36)
        self.assertEqual(struct.calcsize(aur_pack.HEADER_FMT), 36)

    def test_empty_payload_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            payload = Path(d) / "p.bin"
            payload.write_bytes(b"")
            with self.assertRaises(ValueError):
                aur_pack.pack(payload, Path(d) / "o.bin", 0x22000000,
                              0x22000000, b"AUR1")


@unittest.skipUnless(HAVE_GCC, "arm-none-eabi-gcc not on PATH")
class FullBuildTest(unittest.TestCase):
    def test_build_hello(self):
        src = AURIC_ROOT / "examples" / "hello.aur"
        with tempfile.TemporaryDirectory() as d:
            out = Path(d) / "hello.bin"
            aurc.compile_file(src, out, build_dir=Path(d) / "build")
            self.assertTrue(out.exists())

            data = out.read_bytes()
            self.assertEqual(data[:4], b"AUR1")
            magic, a9_off, a9_size, a9_load, a9_entry, *arm11 = struct.unpack(
                aur_pack.HEADER_FMT, data[:36])
            self.assertEqual(a9_load, 0x22000000)
            self.assertEqual(a9_entry, 0x22000000)  # entry == load == _start
            self.assertGreater(a9_size, 0)
            self.assertEqual(a9_off + a9_size, len(data))  # ARM9-only file
            self.assertEqual(arm11, [0, 0, 0, 0])

    def test_build_demo(self):
        src = AURIC_ROOT / "examples" / "demo.aur"
        with tempfile.TemporaryDirectory() as d:
            out = Path(d) / "demo.bin"
            aurc.compile_file(src, out, build_dir=Path(d) / "build")
            self.assertEqual(out.read_bytes()[:4], b"AUR1")

    def test_embedded_icon_at_fixed_offset(self):
        from compiler import icon as icon_mod
        src = AURIC_ROOT / "examples" / "hello.aur"
        with tempfile.TemporaryDirectory() as d:
            # a custom icon: single top-left pixel
            icon_path = Path(d) / "test.icon"
            icon_path.write_text("#\n", encoding="utf-8")
            out = Path(d) / "hello.bin"
            aurc.compile_file(src, out, build_dir=Path(d) / "build",
                              icon=icon_path)
            data = out.read_bytes()
            a9_off = struct.unpack(aur_pack.HEADER_FMT, data[:36])[1]
            # payload: [b _start (4)][AURICON1 (8)][icon (128)]
            self.assertEqual(data[a9_off + 4:a9_off + 12], b"AURICON1")
            embedded = data[a9_off + 12:a9_off + 12 + icon_mod.ICON_BYTES]
            self.assertEqual(embedded, icon_mod.parse_icon_text("#"))

    def test_build_rainbow_sample(self):
        # The sample app in ../sample builds with its icon.
        sample = AURIC_ROOT.parent / "sample"
        src = sample / "rainbow.aur"
        icon_path = sample / "rainbow.icon"
        if not src.exists():
            self.skipTest("sample/rainbow.aur not present")
        with tempfile.TemporaryDirectory() as d:
            out = Path(d) / "RAINBOW.BIN"
            aurc.compile_file(src, out, build_dir=Path(d) / "build",
                              icon=icon_path if icon_path.exists() else None)
            self.assertEqual(out.read_bytes()[:4], b"AUR1")

    def test_aos1_magic_for_bootcompat(self):
        # The --magic AOS1 build must be byte-identical to AUR1 except the magic.
        src = AURIC_ROOT / "examples" / "hello.aur"
        with tempfile.TemporaryDirectory() as d:
            aur1 = Path(d) / "a.bin"
            aos1 = Path(d) / "b.bin"
            aurc.compile_file(src, aur1, build_dir=Path(d) / "b1", magic="AUR1")
            aurc.compile_file(src, aos1, build_dir=Path(d) / "b2", magic="AOS1")
            da, db = aur1.read_bytes(), aos1.read_bytes()
            self.assertEqual(da[4:], db[4:])       # payload identical
            self.assertEqual(db[:4], b"AOS1")


if __name__ == "__main__":
    unittest.main()
