"""Tests for sdk/aurcc.py. Standard library only; the build test is skipped
without arm-none-eabi-gcc on PATH.

    python sdk/tests/test_aurcc.py
"""
import shutil
import struct
import sys
import tempfile
import unittest
from unittest import mock
import zlib
from pathlib import Path

SDK = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SDK))
import aurcc  # noqa: E402


def write_png(path: Path, w: int, h: int, pixel) -> None:
    """An 8-bit RGBA PNG; pixel(x, y) gives (r, g, b, a)."""
    raw = b"".join(b"\0" + b"".join(bytes(pixel(x, y)) for x in range(w))
                   for y in range(h))

    def chunk(tag, data):
        body = tag + data
        return (struct.pack(">I", len(data)) + body
                + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF))

    path.write_bytes(b"\x89PNG\r\n\x1a\n"
                     + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
                     + chunk(b"IDAT", zlib.compress(raw))
                     + chunk(b"IEND", b""))


def bit(icon: bytes, x: int, y: int) -> int:
    return (icon[y * 4 + x // 8] >> (7 - x % 8)) & 1


class IconTests(unittest.TestCase):
    def test_text_icon(self):
        icon = aurcc.icon_from_text("; comment\n#.\n .X\n")
        self.assertEqual(len(icon), 128)
        self.assertEqual(bit(icon, 0, 0), 1)
        self.assertEqual(bit(icon, 1, 0), 0)
        self.assertEqual(bit(icon, 2, 1), 1)
        self.assertEqual(sum(icon), 0x80 + 0x20)

    def test_long_lines_and_rows_are_cut(self):
        icon = aurcc.icon_from_text("\n".join(["#" * 40] * 40))
        self.assertEqual(icon, b"\xff" * 128)

    def test_default_icon(self):
        icon = aurcc.load_icon(None)
        self.assertEqual(len(icon), 128)
        self.assertEqual(bit(icon, 0, 1), 1)  # the frame
        self.assertEqual(bit(icon, 16, 16), 0)

    def test_png_icon(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "icon.png"
            write_png(p, 32, 32, lambda x, y: (255, 255, 255, 255) if x == y
                      else (255, 255, 255, 0) if x < 16 else (10, 10, 10, 255))
            icon = aurcc.load_icon(p)
        for i in range(32):
            self.assertEqual(bit(icon, i, i), 1)
        self.assertEqual(bit(icon, 3, 0), 0)   # clear
        self.assertEqual(bit(icon, 20, 0), 0)  # dark

    def test_png_icon_must_be_32(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "icon.png"
            write_png(p, 16, 16, lambda x, y: (255, 255, 255, 255))
            with self.assertRaises(aurcc.BuildError):
                aurcc.load_icon(p)


class HeaderTests(unittest.TestCase):
    def test_header_asm(self):
        asm = aurcc.header_asm(bytes(range(128)), "My App")
        self.assertIn('.section .aurhead, "ax"', asm)
        self.assertIn("    b _start", asm)
        self.assertIn('.ascii "AURICON1"', asm)
        self.assertIn('.asciz "My App"', asm)
        data = [int(b, 16) for line in asm.splitlines()
                if line.startswith("    .byte") for b in line[9:].split(",")]
        self.assertEqual(data, list(range(128)))

    def test_safe_name(self):
        self.assertEqual(aurcc.safe_name('a"b\\c'), "a_b_c")
        self.assertEqual(aurcc.safe_name("Star Field (2)"), "Star Field (2)")
        self.assertEqual(aurcc.safe_name(""), "app")

    def test_container_header_is_deterministic(self):
        first = aurcc.container_header("AURC", 1234)
        second = aurcc.container_header("AURC", 1234)
        self.assertEqual(first, second)
        self.assertEqual(struct.unpack("<4sIIIIIIII", first),
                         (b"AURC", 36, 1234, 0x22000000, 0x22000000,
                          0, 0, 0, 0))

    def test_container_header_rejects_unknown_magic(self):
        with self.assertRaises(aurcc.BuildError):
            aurcc.container_header("TEST", 10)


class DependencyTests(unittest.TestCase):
    def test_dependency_paths_reads_continuation_lines(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "hello.d"
            path.write_text("hello.o: hello.c include/a\\ b.h \\\n include/c.h\n")
            self.assertEqual(aurcc.dependency_paths(path),
                             [Path("hello.c"), Path("include/a b.h"),
                              Path("include/c.h")])


class DoctorTests(unittest.TestCase):
    def test_doctor_succeeds_in_repository(self):
        with mock.patch("builtins.print") as output:
            self.assertEqual(aurcc.cmd_doctor(None), 0)
        self.assertIn("All C SDK build prerequisites are available.",
                      "\n".join(str(call.args[0]) for call in output.call_args_list))

    def test_doctor_fails_when_compiler_is_missing(self):
        with mock.patch.object(aurcc.shutil, "which",
                              side_effect=lambda name:
                              None if name == aurcc.CC else "/tool"):
            with mock.patch("builtins.print"):
                self.assertEqual(aurcc.cmd_doctor(None), 1)


class RuntimeContractTests(unittest.TestCase):
    def test_invalid_seek_origin_is_rejected(self):
        source = (SDK / "runtime" / "app_fs.c").read_text()
        self.assertIn("default:\n    return -EINVAL;", source)

    def test_negative_nanosleep_seconds_are_rejected(self):
        source = (SDK / "runtime" / "app_syscalls.c").read_text()
        self.assertIn("req->tv_sec < 0", source)

    def test_http_post_rejects_truncated_content_type(self):
        source = (SDK / "runtime" / "app_net.c").read_text()
        self.assertIn('(size_t)written >= sizeof(head)', source)
        self.assertIn('g_err = "the content type is too long";', source)


@unittest.skipUnless(shutil.which(aurcc.CC), "arm-none-eabi-gcc not on PATH")
class BuildTests(unittest.TestCase):
    def test_build_hello(self):
        with tempfile.TemporaryDirectory() as d:
            out = Path(d) / "Hello.bin"
            aurcc.build([str(SDK / "examples" / "hello")], out,
                        build_dir=Path(d) / "build")
            data = out.read_bytes()
        magic, off, size, load, entry = struct.unpack("<4sIIII", data[:20])
        self.assertEqual(magic, b"AURC")
        self.assertEqual((off, load, entry), (36, 0x22000000, 0x22000000))
        self.assertEqual(off + size, len(data))
        payload = data[off:]
        # A branch (condition AL, opcode 101, no link) over the icon block.
        word = struct.unpack("<I", payload[:4])[0]
        self.assertEqual(word >> 24, 0xEA)
        self.assertGreaterEqual(8 + 4 * (word & 0xFFFFFF), 12 + 128)
        self.assertEqual(payload[4:12], b"AURICON1")
        icon = aurcc.icon_from_text(
            (SDK / "examples" / "hello" / "hello.icon").read_text())
        self.assertEqual(payload[12:140], icon)
        self.assertIn(b"Hello\0", payload)

    def test_missing_source(self):
        with self.assertRaises(aurcc.BuildError):
            aurcc.build(["no_such_file.c"], None)


if __name__ == "__main__":
    unittest.main()
