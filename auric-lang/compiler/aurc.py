"""aurc -- the Auric compiler driver.

Ties the whole pipeline together:

    source.aur
      -> front end (lexer -> parser -> type check -> C codegen)   [pure Python]
      -> arm-none-eabi-gcc  (AuroraOS's exact ARM9 flags)
      -> objcopy -O binary                                         [raw payload]
      -> aur_pack.py                                               [AUR1 .bin]

`arm-none-eabi-gcc` (devkitARM) is a hard dependency: it is detected on PATH and
a clear, actionable error is printed if it is missing.

Usage:
    python -m compiler.aurc build examples/hello.aur -o hello.bin
    python -m compiler.aurc emit-c examples/hello.aur -o hello.c
    python -m compiler.aurc version
"""
from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

# Allow running both as a module (python -m compiler.aurc) and as a script.
if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
    from compiler import AuricError, __version__, compile_to_c  # type: ignore
    from compiler import icon as icon_mod  # type: ignore
else:
    from . import AuricError, __version__, compile_to_c
    from . import icon as icon_mod

if getattr(sys, "frozen", False):
    BUNDLE = Path(getattr(sys, "_MEIPASS"))
    RUNTIME_DIR = BUNDLE / "runtime"
    TOOLS_DIR = BUNDLE / "tools"
    AURORA_INCLUDE = BUNDLE / "aurora_include"
    AURORA_SRC = BUNDLE / "aurora_src"
    DEFAULT_BUILD_DIR = Path.cwd() / "build"   # bundle dir is read-only/temp
else:
    COMPILER_DIR = Path(__file__).resolve().parent
    AURIC_ROOT = COMPILER_DIR.parent           # auric-lang/
    AURORA_ROOT = AURIC_ROOT.parent            # aurora/  (AuroraOS repo root)
    RUNTIME_DIR = AURIC_ROOT / "runtime"
    TOOLS_DIR = AURIC_ROOT / "tools"
    AURORA_INCLUDE = AURORA_ROOT / "include"
    AURORA_SRC = AURORA_ROOT / "src"
    DEFAULT_BUILD_DIR = AURIC_ROOT / "build"

# AuroraOS sources the runtime reuses/links against.
AURORA_SCREEN = AURORA_SRC / "screen.c"
AURORA_I2C = AURORA_SRC / "i2c.c"

RUNTIME_C = RUNTIME_DIR / "auric_runtime.c"
RUNTIME_START = RUNTIME_DIR / "auric_start.s"
RUNTIME_LD = RUNTIME_DIR / "auric.ld"

DEFAULT_LOAD_ADDR = 0x22000000

CC = "arm-none-eabi-gcc"
OBJCOPY = "arm-none-eabi-objcopy"

INSTALL_HINT = (
    "arm-none-eabi-gcc was not found on PATH.\n"
    "Auric needs an ARM cross-compiler (devkitARM is the standard choice for\n"
    "3DS homebrew). Install devkitPro/devkitARM and make sure its bin directory\n"
    "is on PATH, e.g. C:\\devkitPro\\devkitARM\\bin (Windows) or\n"
    "/opt/devkitpro/devkitARM/bin (Unix). See https://devkitpro.org/wiki/Getting_Started"
)

# AuroraOS's exact ARM9 flags (from ../../Makefile: ARM9_ARCH / ARM9_CFLAGS /
# ARM9_LDFLAGS), plus -ffunction-sections/-fdata-sections so --gc-sections drops
# the unused parts of screen.c (logo/icon/console). Size-only; arch/ABI unchanged.
ARM9_ARCH = ["-mcpu=arm946e-s", "-march=armv5te", "-marm"]
ARM9_CFLAGS = ARM9_ARCH + [
    "-mthumb-interwork", "-ffreestanding", "-fno-builtin", "-nostdlib",
    "-nostartfiles", "-Wall", "-Wextra", "-g", "-O2",
    "-ffunction-sections", "-fdata-sections",
]
ARM9_ASFLAGS = ARM9_ARCH + ["-mthumb-interwork"]
ARM9_LDFLAGS = [
    "-T", str(RUNTIME_LD), "-nostdlib", "-nostartfiles",
    "-Wl,--build-id=none", "-Wl,--gc-sections",
]


class BuildError(Exception):
    """A failure in the native toolchain stage (gcc/objcopy)."""


def _find_tool(name: str) -> str:
    path = shutil.which(name)
    if path is None:
        raise BuildError(INSTALL_HINT if name == CC
                         else f"{name} was not found on PATH (part of devkitARM).")
    return path


def _run(cmd: list[str], verbose: bool) -> None:
    if verbose:
        print("  $ " + " ".join(cmd))
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0:
        out = (proc.stdout + proc.stderr).strip()
        raise BuildError(f"command failed ({proc.returncode}): {' '.join(cmd)}\n{out}")
    elif verbose and proc.stderr.strip():
        print(proc.stderr.strip())


def _pack(payload: Path, output: Path, load_addr: int, magic: str) -> None:
    """Invoke the AUR1 packer in-process (tools/aur_pack.py)."""
    sys.path.insert(0, str(TOOLS_DIR))
    import aur_pack  # type: ignore
    aur_pack.pack(payload, output, load_addr, load_addr,
                  magic.encode("ascii"))


def compile_file(src_path: Path, output: Path, *, build_dir: Path,
                 load_addr: int = DEFAULT_LOAD_ADDR, magic: str = "AUR1",
                 icon: Path | None = None, keep: bool = False,
                 verbose: bool = False) -> Path:
    """Compile an .aur file all the way to a packed AUR1 .bin.

    If `icon` is given it is a text icon (see compiler/icon.py); otherwise a
    generic default icon is embedded. The icon lives at a fixed offset in the
    payload so AuroraOS can show it on the home screen.
    """
    src = src_path.read_text(encoding="utf-8")

    # 1. Front end (pure Python). Raises AuricError on a program error.
    c_source = compile_to_c(src)
    icon_bytes = icon_mod.load_icon(icon) if icon else icon_mod.default_icon()
    head_asm = icon_mod.emit_header_asm(icon_bytes)

    # 2. Native toolchain.
    cc = _find_tool(CC)
    objcopy = _find_tool(OBJCOPY)

    stem = src_path.stem
    work = build_dir / stem
    work.mkdir(parents=True, exist_ok=True)

    gen_c = work / f"{stem}.c"
    gen_c.write_text(c_source, encoding="utf-8")
    head_s = work / "aur_head_gen.s"
    head_s.write_text(head_asm, encoding="utf-8")

    includes = ["-I" + str(AURORA_INCLUDE), "-I" + str(RUNTIME_DIR)]

    head_o = work / "aur_head_gen.o"
    prog_o = work / f"{stem}.o"
    runtime_o = work / "auric_runtime.o"
    screen_o = work / "screen.o"
    i2c_o = work / "i2c.o"
    start_o = work / "auric_start.o"
    elf = work / f"{stem}.elf"
    payload = work / f"{stem}.payload.bin"

    # Compile the icon header, generated program, runtime shim, and the AuroraOS
    # sources the runtime reuses (screen.c for drawing, i2c.c for the HOME poll).
    _run([cc, *ARM9_ASFLAGS, *includes, "-c", str(head_s), "-o", str(head_o)], verbose)
    _run([cc, *ARM9_CFLAGS, *includes, "-c", str(gen_c), "-o", str(prog_o)], verbose)
    _run([cc, *ARM9_CFLAGS, *includes, "-c", str(RUNTIME_C), "-o", str(runtime_o)], verbose)
    _run([cc, *ARM9_CFLAGS, *includes, "-c", str(AURORA_SCREEN), "-o", str(screen_o)], verbose)
    _run([cc, *ARM9_CFLAGS, *includes, "-c", str(AURORA_I2C), "-o", str(i2c_o)], verbose)
    _run([cc, *ARM9_ASFLAGS, "-c", str(RUNTIME_START), "-o", str(start_o)], verbose)

    # Link. Section placement (icon header first, then _start) is fixed by the
    # linker script, so object order here is not significant.
    _run([cc, *ARM9_LDFLAGS, str(head_o), str(start_o), str(prog_o), str(runtime_o),
          str(screen_o), str(i2c_o), "-o", str(elf), "-lgcc"], verbose)

    # objcopy -> raw binary payload.
    _run([objcopy, "-O", "binary", str(elf), str(payload)], verbose)

    # 3. Pack into an AUR1 container.
    output.parent.mkdir(parents=True, exist_ok=True)
    _pack(payload, output, load_addr, magic)

    if not keep:
        for f in (head_o, head_s, prog_o, runtime_o, screen_o, i2c_o, start_o,
                  elf, payload):
            f.unlink(missing_ok=True)

    return output


def _cmd_build(args: argparse.Namespace) -> int:
    src_path = Path(args.source)
    if not src_path.exists():
        print(f"Error: source file not found: {src_path}", file=sys.stderr)
        return 1
    output = Path(args.output) if args.output else src_path.with_suffix(".bin")
    build_dir = Path(args.build_dir) if args.build_dir else DEFAULT_BUILD_DIR
    icon = Path(args.icon) if args.icon else None
    if icon and not icon.exists():
        print(f"Error: icon file not found: {icon}", file=sys.stderr)
        return 1
    try:
        out = compile_file(src_path, output, build_dir=build_dir,
                           load_addr=args.load_addr, magic=args.magic,
                           icon=icon, keep=args.keep, verbose=args.verbose)
    except AuricError as e:
        print(f"{src_path}: {e}", file=sys.stderr)
        return 1
    except BuildError as e:
        print(f"Error: {e}", file=sys.stderr)
        return 1
    print(f"Built {out}")
    return 0


def _cmd_emit_c(args: argparse.Namespace) -> int:
    src_path = Path(args.source)
    if not src_path.exists():
        print(f"Error: source file not found: {src_path}", file=sys.stderr)
        return 1
    try:
        c_source = compile_to_c(src_path.read_text(encoding="utf-8"))
    except AuricError as e:
        print(f"{src_path}: {e}", file=sys.stderr)
        return 1
    if args.output:
        Path(args.output).write_text(c_source, encoding="utf-8")
        print(f"Wrote {args.output}")
    else:
        sys.stdout.write(c_source)
    return 0


def _cmd_version(_args: argparse.Namespace) -> int:
    print(f"aurc (Auric compiler) {__version__}")
    cc = shutil.which(CC)
    print(f"  arm-none-eabi-gcc: {cc or 'NOT FOUND on PATH'}")
    return 0


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="aurc", description="The Auric compiler for AuroraOS (3DS).")
    sub = parser.add_subparsers(dest="command", required=True)

    p_build = sub.add_parser("build", help="Compile an .aur file to an AUR1 .bin")
    p_build.add_argument("source", help="Auric source file (.aur)")
    p_build.add_argument("-o", "--output", help="Output .bin (default: <source>.bin)")
    p_build.add_argument("--magic", choices=("AUR1", "AOS1"), default="AUR1",
                         help="Container magic (AOS1 boots on the unmodified "
                              "loader; default AUR1)")
    p_build.add_argument("--load-addr", type=lambda v: int(v, 0),
                         default=DEFAULT_LOAD_ADDR, help="ARM9 load/entry address")
    p_build.add_argument("--icon", help="Text icon file (32x32) to embed; a "
                                        "default icon is used if omitted")
    p_build.add_argument("--build-dir", help="Intermediate build directory")
    p_build.add_argument("--keep", action="store_true",
                         help="Keep intermediate .c/.o/.elf artifacts")
    p_build.add_argument("-v", "--verbose", action="store_true",
                         help="Echo toolchain commands")
    p_build.set_defaults(func=_cmd_build)

    p_emit = sub.add_parser("emit-c", help="Transpile an .aur file to C (no gcc)")
    p_emit.add_argument("source", help="Auric source file (.aur)")
    p_emit.add_argument("-o", "--output", help="Output .c (default: stdout)")
    p_emit.set_defaults(func=_cmd_emit_c)

    p_ver = sub.add_parser("version", help="Print version and toolchain status")
    p_ver.set_defaults(func=_cmd_version)

    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_arg_parser()
    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
