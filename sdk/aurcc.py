"""aurcc: builds AuroraOS apps written in C.

    python sdk/aurcc.py build main.c -o Hello.bin --icon hello.icon
    python sdk/aurcc.py build sdk/examples/hello      # every .c in a folder
    python sdk/aurcc.py new MyApp                      # start a project
    python sdk/aurcc.py version

Sources are compiled with AuroraOS's ARM9 flags and linked with the SDK
runtime (sdk/runtime), the OS code it reuses (src/) and devkitARM's newlib,
then packed as an AURC app (a C app) for SD:/Aurora/Apps/C. Needs devkitARM's
arm-none-eabi-gcc on PATH and Python 3.8+.
"""
from __future__ import annotations

import argparse
import concurrent.futures
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

__version__ = "1.1"

SDK_DIR = Path(__file__).resolve().parent
AURORA_ROOT = SDK_DIR.parent
SDK_INCLUDE = SDK_DIR / "include"
RUNTIME_DIR = SDK_DIR / "runtime"
OS_INCLUDE = AURORA_ROOT / "include"
OS_SRC = AURORA_ROOT / "src"
LINKER_SCRIPT = RUNTIME_DIR / "app.ld"
DEFAULT_BUILD_DIR = SDK_DIR / "build"
PNG_READER = AURORA_ROOT / "tools" / "png_read.py"

RUNTIME_SOURCES = [RUNTIME_DIR / n for n in (
    "app_start.s", "app_main.c", "app_gfx.c", "app_console.c", "app_hid.c",
    "app_sound.c", "app_fs.c", "app_sys.c", "app_syscalls.c", "app_net.c")]
# The OS's own code: drawing, GPU presents, touch, the timer, I2C (HOME, the
# clock, the battery), FatFs and the SD driver, images, WAV, the crash screen,
# and the network (AppNet.c, with the join's request and WPA2 key).
# --gc-sections keeps only what an app reaches.
OS_SOURCES = [OS_SRC / n for n in (
    "screen.c", "i2c.c", "power.c", "model.c", "assets.c", "image.c",
    "jpeg.c", "wavload.c", "container.c", "ff.c", "ffunicode.c", "diskio.c",
    "sdmmc.c",
    "os/Gpu9.c", "os/Touch9.c", "os/Timer9.c", "os/crash.c", "os/crash.s",
    "os/AppNet.c", "os/WiFiJoin.c", "os/Crypto.c")]

CC = "arm-none-eabi-gcc"
OBJCOPY = "arm-none-eabi-objcopy"
NM = "arm-none-eabi-nm"

ARCH = ["-mcpu=arm946e-s", "-march=armv5te", "-marm", "-mthumb-interwork"]
# The runtime and the OS code build as the OS does (Makefile), freestanding.
RUNTIME_CFLAGS = ARCH + [
    "-ffreestanding", "-fno-builtin", "-Wall", "-Wextra", "-g", "-O2",
    "-ffunction-sections", "-fdata-sections",
    "-I" + str(SDK_INCLUDE), "-I" + str(OS_INCLUDE), "-I" + str(RUNTIME_DIR)]
# Apps get the hosted C library.
APP_CFLAGS = ARCH + ["-Wall", "-Wextra", "-g", "-ffunction-sections",
                     "-fdata-sections", "-I" + str(SDK_INCLUDE)]
LIBS = ["-Wl,--start-group", "-lc", "-lm", "-lgcc", "-Wl,--end-group"]

LOAD_ADDR = 0x22000000
HEADER_FMT = "<4sIIIIIIII"  # include/loader.h aos_header_t, 36 bytes

ICON_SIZE = 32
ICON_BYTES = ICON_SIZE * ICON_SIZE // 8
ICON_MAGIC = "AURICON1"
ICON_ON = set("#*XO@8")

INSTALL_HINT = (
    "arm-none-eabi-gcc was not found on PATH.\n"
    "Install devkitPro's devkitARM (https://devkitpro.org/wiki/Getting_Started)\n"
    "and put its bin folder on PATH, e.g. C:\\devkitPro\\devkitARM\\bin.")


class BuildError(Exception):
    pass


def find_tool(name: str) -> str:
    path = shutil.which(name)
    if not path:
        raise BuildError(INSTALL_HINT if name == CC else
                         f"{name} was not found on PATH (part of devkitARM).")
    return path


def tool_env() -> dict:
    """gcc falls back to an unwritable folder (C:\\WINDOWS on Windows) when
    the temp variables are empty, so they are always set."""
    env = dict(os.environ)
    tmp = tempfile.gettempdir()
    for var in ("TMPDIR", "TMP", "TEMP"):
        if not env.get(var):
            env[var] = tmp
    return env


def run(cmd: list, verbose: bool) -> None:
    cmd = [str(c) for c in cmd]
    if verbose:
        print("  $ " + " ".join(cmd))
    proc = subprocess.run(cmd, capture_output=True, text=True, env=tool_env())
    out = (proc.stdout + proc.stderr).strip()
    if proc.returncode != 0:
        raise BuildError(out or f"{cmd[0]} failed ({proc.returncode})")
    if out:
        print(out)


def icon_from_text(text: str) -> bytes:
    """32 lines of 32 columns; # * X O @ 8 are on; lines starting with ; are
    comments (the format Auric uses)."""
    rows = [ln.rstrip("\r\n") for ln in text.split("\n")
            if not ln.lstrip().startswith(";")]
    out = bytearray(ICON_BYTES)
    for r in range(ICON_SIZE):
        row = rows[r] if r < len(rows) else ""
        for c in range(min(ICON_SIZE, len(row))):
            if row[c] in ICON_ON:
                out[r * 4 + c // 8] |= 0x80 >> (c % 8)
    return bytes(out)


def icon_from_png(path: Path) -> bytes:
    """A 32x32 PNG: light, solid pixels are on."""
    sys.path.insert(0, str(PNG_READER.parent))
    try:
        import png_read  # type: ignore
        w, h, px = png_read.read(str(path))
    except (AssertionError, OSError, ValueError, KeyError) as e:
        raise BuildError(f"cannot read icon {path}: {e}")
    finally:
        sys.path.pop(0)
    if (w, h) != (ICON_SIZE, ICON_SIZE):
        raise BuildError(f"icon {path} is {w}x{h}; it must be 32x32")
    out = bytearray(ICON_BYTES)
    for y in range(ICON_SIZE):
        for x in range(ICON_SIZE):
            r, g, b, a = px[(y * w + x) * 4:(y * w + x) * 4 + 4]
            if a >= 128 and (r * 299 + g * 587 + b * 114) >= 128000:
                out[y * 4 + x // 8] |= 0x80 >> (x % 8)
    return bytes(out)


DEFAULT_ICON = """\
 ##############################
################################
##                            ##
##                            ##
##          ########          ##
##        ############        ##
##       ####      ####       ##
##      ###          ###      ##
##     ###                    ##
##     ###                    ##
##    ###                     ##
##    ###                     ##
##    ###                     ##
##    ###                     ##
##    ###                     ##
##    ###                     ##
##    ###                     ##
##    ###                     ##
##    ###                     ##
##    ###                     ##
##     ###                    ##
##     ###                    ##
##      ###          ###      ##
##       ####      ####       ##
##        ############        ##
##          ########          ##
##                            ##
##                            ##
##                            ##
##                            ##
################################
 ##############################
"""


def load_icon(path: Path | None) -> bytes:
    if path is None:
        return icon_from_text(DEFAULT_ICON)
    if path.suffix.lower() == ".png":
        return icon_from_png(path)
    try:
        return icon_from_text(path.read_text(encoding="utf-8"))
    except OSError as e:
        raise BuildError(f"cannot read icon {path}: {e}")


def header_asm(icon: bytes, name: str) -> str:
    """The app header the Home Menu reads (docs/apps.md, "Per-app icons"),
    and the default name app_path() falls back on."""
    lines = [
        '.section .aurhead, "ax"',
        ".arm",
        ".global _aur_header",
        "_aur_header:",
        "    b _start",
        f'    .ascii "{ICON_MAGIC}"',
    ]
    for i in range(0, ICON_BYTES, 16):
        lines.append("    .byte " + ", ".join(f"0x{b:02x}" for b in icon[i:i + 16]))
    lines += [
        '.section .rodata._app_default_name, "a"',
        ".global _app_default_name",
        ".type _app_default_name, %object",
        "_app_default_name:",
        f'    .asciz "{name}"',
        ".size _app_default_name, .-_app_default_name",
    ]
    return "\n".join(lines) + "\n"


def obj_name(src: Path, root: Path) -> str:
    try:
        rel = src.resolve().relative_to(root.resolve())
    except ValueError:
        rel = Path(src.name)
    return re.sub(r"[^A-Za-z0-9_.-]", "_", str(rel)) + ".o"


def newest(paths) -> float:
    return max((p.stat().st_mtime for p in paths if p.exists()), default=0.0)


def compile_all(jobs: list, verbose: bool) -> None:
    """jobs: (source, object, flags); runs gcc on several at once."""
    def one(job):
        src, obj, flags = job
        run([CC, *flags, "-c", src, "-o", obj], verbose)
    workers = min(len(jobs), os.cpu_count() or 2) or 1
    with concurrent.futures.ThreadPoolExecutor(workers) as pool:
        for f in [pool.submit(one, j) for j in jobs]:
            f.result()


def runtime_objects(build_dir: Path, rebuild: bool, verbose: bool) -> list:
    """The runtime and OS objects, compiled once and reused until a source,
    a header or this script changes."""
    out = build_dir / "runtime"
    out.mkdir(parents=True, exist_ok=True)
    deps = newest([*OS_INCLUDE.glob("*.h"), *SDK_INCLUDE.glob("*.h"),
                   *RUNTIME_DIR.glob("*.h"), Path(__file__)])
    jobs, objs = [], []
    for src in RUNTIME_SOURCES + OS_SOURCES:
        root = RUNTIME_DIR if src.parent == RUNTIME_DIR else OS_SRC
        obj = out / (("" if root == RUNTIME_DIR else "os_") + obj_name(src, root))
        objs.append(obj)
        if (rebuild or not obj.exists()
                or obj.stat().st_mtime < max(deps, src.stat().st_mtime)):
            flags = ARCH if src.suffix == ".s" else RUNTIME_CFLAGS
            jobs.append((src, obj, flags))
    if jobs:
        print(f"Compiling the SDK runtime ({len(jobs)} files)...")
        compile_all(jobs, verbose)
    return objs


def collect_sources(inputs: list) -> tuple:
    """Files as given; folders give every .c (and .s) inside them. Also returns
    the first .icon or .png called icon in a given folder."""
    sources, icon = [], None
    for item in inputs:
        p = Path(item)
        if p.is_dir():
            found = sorted([*p.rglob("*.c"), *p.rglob("*.s")])
            found = [f for f in found if "build" not in f.relative_to(p).parts]
            if not found:
                raise BuildError(f"no .c files in {p}")
            sources += found
            if icon is None:
                icons = sorted(p.glob("*.icon")) or sorted(p.glob("icon.png"))
                icon = icons[0] if icons else None
        elif p.is_file():
            sources.append(p)
        else:
            raise BuildError(f"not found: {p}")
    return sources, icon


def safe_name(name: str) -> str:
    return re.sub(r'[^A-Za-z0-9 _.()+-]', "_", name)[:64] or "app"


def build(inputs: list, output: Path | None, *, icon: Path | None = None,
          build_dir: Path = DEFAULT_BUILD_DIR, cflags: list | None = None,
          opt: str = "2", magic: str = "AURC", rebuild: bool = False,
          verbose: bool = False) -> Path:
    find_tool(CC)
    find_tool(OBJCOPY)
    sources, found_icon = collect_sources(inputs)
    icon = icon or found_icon
    first = Path(inputs[0])
    stem = first.resolve().name if first.is_dir() else first.stem
    if output is None:
        output = Path(stem + ".bin")
    name = safe_name(output.stem)

    runtime = runtime_objects(build_dir, rebuild, verbose)

    work = build_dir / "apps" / re.sub(r"[^A-Za-z0-9_.-]", "_", name)
    work.mkdir(parents=True, exist_ok=True)
    head_s = work / "app_header.s"
    head_s.write_text(header_asm(load_icon(icon), name), encoding="ascii")
    head_o = work / "app_header.o"

    flags = APP_CFLAGS + [f"-O{opt}"] + (cflags or [])
    jobs = [(head_s, head_o, ARCH)]
    app_objs = []
    common = Path(os.path.commonpath([str(s.resolve().parent) for s in sources]))
    for src in sources:
        obj = work / obj_name(src, common)
        app_objs.append(obj)
        jobs.append((src, obj, ARCH if src.suffix == ".s" else flags))
    compile_all(jobs, verbose)

    elf = work / (name + ".elf")
    payload = work / (name + ".payload")
    run([CC, *ARCH, "-T", LINKER_SCRIPT, "-nostartfiles", "-nostdlib",
         "-Wl,--gc-sections", "-Wl,--build-id=none",
         "-Wl,--no-warn-rwx-segments", f"-Wl,-Map,{work / (name + '.map')}",
         head_o, *runtime, *app_objs, *LIBS, "-o", elf], verbose)
    run([OBJCOPY, "-O", "binary", elf, payload], verbose)

    data = payload.read_bytes()
    header = struct.pack(HEADER_FMT, magic.encode("ascii"), 36, len(data),
                         LOAD_ADDR, LOAD_ADDR, 0, 0, 0, 0)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(header + data)

    end = bss_end(elf)
    print(f"Built {output} ({len(header) + len(data):,} bytes"
          + (f"; uses memory to {end:#010x}" if end else "") + ")")
    return output


def bss_end(elf: Path) -> int:
    nm = shutil.which(NM)
    if not nm:
        return 0
    proc = subprocess.run([nm, str(elf)], capture_output=True, text=True,
                          env=tool_env())
    for line in proc.stdout.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] == "_app_bss_end":
            return int(parts[0], 16)
    return 0


TEMPLATE = """\
// {name}: an AuroraOS app in C. Build it with the SDK's aurcc,
//   python <AuroraOS>/sdk/aurcc.py build {name} -o {name}.bin
// and copy {name}.bin to SD:/Aurora/Apps/C.
#include <aurora_app.h>

int main(void) {{
  int x = 200, y = 120;

  while (app_loop()) {{
    uint32_t held = hid_keys_held();
    int cx, cy;

    if (held & KEY_LEFT) x -= 2;
    if (held & KEY_RIGHT) x += 2;
    if (held & KEY_UP) y -= 2;
    if (held & KEY_DOWN) y += 2;
    hid_cpad(&cx, &cy);
    x += cx / 32;
    y -= cy / 32;
    if (hid_keys_down() & KEY_START)
      break;

    gfx_clear(SCREEN_TOP, RGB(16, 18, 28));
    gfx_print(SCREEN_TOP, 12, 10, FONT_TITLE, COLOR_WHITE, "{name}");
    gfx_circle(SCREEN_TOP, x, y, 12, COLOR_AURORA);

    gfx_clear(SCREEN_BOTTOM, RGB(28, 28, 34));
    gfx_print(SCREEN_BOTTOM, 12, 10, FONT_REGULAR, COLOR_WHITE,
              "D-pad or circle pad: move\\nTouch: draw a dot\\nSTART: quit");
    if (hid_touch_held())
      gfx_circle(SCREEN_BOTTOM, hid_touch_x(), hid_touch_y(), 6, COLOR_ORANGE);
  }}
  return 0;
}}
"""


def shown(path: Path) -> str:
    """Relative to here when that stays below here, else absolute."""
    try:
        rel = os.path.relpath(path)
    except ValueError:  # another drive
        rel = ""
    if not rel or rel.startswith(".."):
        rel = str(Path(path).resolve())
    return rel.replace("\\", "/")


def cmd_build(a) -> int:
    try:
        build(a.sources, Path(a.output) if a.output else None,
              icon=Path(a.icon) if a.icon else None,
              build_dir=Path(a.build_dir) if a.build_dir else DEFAULT_BUILD_DIR,
              cflags=[f"-I{d}" for d in a.include] + [f"-D{d}" for d in a.define]
              + (a.cflags.split() if a.cflags else []),
              opt=a.opt, magic=a.magic, rebuild=a.rebuild, verbose=a.verbose)
    except BuildError as e:
        print(f"Error: {e}", file=sys.stderr)
        return 1
    return 0


def cmd_new(a) -> int:
    name = safe_name(a.name).replace(" ", "")
    folder = Path(a.dir) / name if a.dir else Path(name)
    if folder.exists() and any(folder.iterdir()):
        print(f"Error: {folder} exists and is not empty", file=sys.stderr)
        return 1
    folder.mkdir(parents=True, exist_ok=True)
    (folder / "main.c").write_text(TEMPLATE.format(name=name),
                                   encoding="ascii")
    (folder / (name + ".icon")).write_text(
        "; " + name + ": 32 x 32, # is a lit pixel\n" + DEFAULT_ICON,
        encoding="ascii")
    print(f"Created {shown(folder)}/main.c and {name}.icon")
    print(f"Build:  python {shown(Path(__file__))} build {shown(folder)} "
          f"-o {name}.bin")
    print(f"Then copy {name}.bin to SD:/Aurora/Apps/C on the card.")
    return 0


def cmd_version(_a) -> int:
    print(f"aurcc (AuroraOS native app SDK) {__version__}")
    print(f"  arm-none-eabi-gcc: {shutil.which(CC) or 'NOT FOUND on PATH'}")
    return 0


def main(argv=None) -> int:
    p = argparse.ArgumentParser(prog="aurcc",
                                description="Build AuroraOS apps written in C.")
    sub = p.add_subparsers(dest="command", required=True)

    b = sub.add_parser("build", help="compile C sources into an app (.bin)")
    b.add_argument("sources", nargs="+",
                   help=".c/.s files, or folders to take every .c from")
    b.add_argument("-o", "--output", help="the app file (default: <name>.bin)")
    b.add_argument("--icon", help="32x32 icon: a text .icon or a .png "
                                  "(default: a folder's *.icon, else a C)")
    b.add_argument("-I", dest="include", action="append", default=[],
                   help="add an include folder")
    b.add_argument("-D", dest="define", action="append", default=[],
                   help="define a macro, NAME or NAME=VALUE")
    b.add_argument("-O", dest="opt", default="2",
                   choices=("0", "1", "2", "3", "s", "g"),
                   help="optimisation level (default 2)")
    b.add_argument("--cflags", help="more compiler flags, quoted")
    b.add_argument("--magic", choices=("AURC", "AUR1", "AOS1"),
                   default="AURC",
                   help="AURC, a C app (default); AUR1 for an OS from before "
                        "AURC (Beta v0.1.4); AOS1 to boot the app directly "
                        "as AURORAOS.BIN")
    b.add_argument("--build-dir", help=f"objects (default {DEFAULT_BUILD_DIR})")
    b.add_argument("--rebuild", action="store_true",
                   help="compile the SDK runtime again too")
    b.add_argument("-v", "--verbose", action="store_true",
                   help="show the toolchain commands")
    b.set_defaults(func=cmd_build)

    n = sub.add_parser("new", help="start a project folder")
    n.add_argument("name")
    n.add_argument("--dir", help="where to create it (default: here)")
    n.set_defaults(func=cmd_new)

    v = sub.add_parser("version", help="print versions and the toolchain")
    v.set_defaults(func=cmd_version)

    a = p.parse_args(argv)
    return a.func(a)


if __name__ == "__main__":
    sys.exit(main())
