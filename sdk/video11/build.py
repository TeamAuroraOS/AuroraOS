#!/usr/bin/env python3
"""Builds the video engine: sdk/video11 and the vendored FFmpeg in lib/ffmpeg, for
the ARM11 cores other than core 0. The result is a flat binary aurcc.py embeds in an
app built with --video, which copies it to VIDEO_ENGINE_ADDR and starts it through
AUDIO_CMD_SMP (docs/video.md).

Objects are kept and rebuilt when their source is newer than they are, or when a
header in either tree is. FFmpeg is about 150 files, so the first build takes a
minute or two.

Run directly to build and see the size: py sdk/video11/build.py [-v]
"""
import concurrent.futures as cf
import os
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
FFMPEG = REPO / "lib" / "ffmpeg"
INCLUDE = REPO / "include"
DEFAULT_OUT = HERE.parent / "build" / "video11"

CC = "arm-none-eabi-gcc"
OBJCOPY = "arm-none-eabi-objcopy"
ENGINE = ["engine.c", "convert11.c", "threads11.c", "libc11.c"]
# The cores' code: hard-float, with newlib's matching multilib, and unaligned
# access, since a job runs with SCTLR.U set.
ARCH = ["-march=armv6k", "-mtune=mpcore", "-mfloat-abi=hard", "-mfpu=vfp", "-marm"]
FF_FLAGS = ARCH + ["-O3", "-std=c17", "-fomit-frame-pointer", "-ffunction-sections",
                   "-fdata-sections", "-fno-math-errno", "-fno-signed-zeros",
                   "-fno-tree-vectorize", "-mfp16-format=ieee", "-fpermissive", "-w",
                   "-DHAVE_AV_CONFIG_H", "-D_ISOC99_SOURCE", "-D_FILE_OFFSET_BITS=64",
                   "-D_LARGEFILE_SOURCE", "-D_POSIX_C_SOURCE=200112",
                   "-D_XOPEN_SOURCE=600", "-I", str(FFMPEG)]
ENGINE_FLAGS = ARCH + ["-O2", "-g", "-std=gnu17", "-Wall", "-Wextra", "-DHAVE_AV_CONFIG_H",
                       "-Wno-unused-parameter", "-Wno-deprecated-declarations",
                       "-ffunction-sections", "-fdata-sections",
                       "-I", str(HERE), "-I", str(INCLUDE), "-I", str(FFMPEG)]


def tool_env():
    """gcc falls back to C:\\WINDOWS for its temp files unless these are set."""
    env = dict(os.environ)
    tmp = env.get("TEMP") or env.get("TMP") or str(Path.home())
    for k in ("TMP", "TEMP", "TMPDIR"):
        env[k] = env.get(k) or tmp
    return env


def run(cmd, verbose, cwd=None):
    if verbose:
        print("  $ " + " ".join(str(c) for c in cmd))
    r = subprocess.run([str(c) for c in cmd], env=tool_env(), capture_output=True,
                       text=True, cwd=cwd)
    if r.returncode:
        raise SystemExit(f"failed: {' '.join(str(c) for c in cmd[:3])} ...\n{r.stdout}{r.stderr}")
    return r.stdout


def obj_for(out, src, root):
    rel = src.relative_to(root)
    return out / root.name / rel.with_suffix(".o")


def sources():
    listed = (FFMPEG / "sources.txt").read_text(encoding="ascii").split()
    return [FFMPEG / s for s in listed]


def build(out=DEFAULT_OUT, verbose=False):
    """Returns the path of the flat binary."""
    if not shutil.which(CC):
        raise SystemExit(f"{CC} was not found on PATH (devkitARM)")
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    bin_path = out / "video11.bin"
    newest_h = max([p.stat().st_mtime for p in HERE.glob("*.h")] +
                   [(INCLUDE / n).stat().st_mtime for n in ("video.h", "smp.h")])
    ff_h = max(p.stat().st_mtime for p in FFMPEG.rglob("*.h"))

    jobs = []
    objs = []
    for src in sources():
        o = obj_for(out, src, FFMPEG)
        objs.append(o)
        if not o.exists() or o.stat().st_mtime < max(src.stat().st_mtime, ff_h):
            jobs.append((src, o, FF_FLAGS))
    for name in ENGINE:
        src = HERE / name
        o = obj_for(out, src, HERE)
        objs.append(o)
        if not o.exists() or o.stat().st_mtime < max(src.stat().st_mtime, newest_h, ff_h):
            jobs.append((src, o, ENGINE_FLAGS))
    if jobs:
        print(f"Compiling the video engine ({len(jobs)} files)...")

    def one(job):
        src, o, flags = job
        o.parent.mkdir(parents=True, exist_ok=True)
        run([CC, *flags, "-c", src, "-o", o], verbose)

    with cf.ThreadPoolExecutor(os.cpu_count() or 4) as ex:
        list(ex.map(one, jobs))

    elf = out / "video11.elf"
    rsp = out / "objects.rsp"
    # Relative, since a response file is read by the Windows linker as it stands,
    # whichever Python wrote it.
    rsp.write_text("\n".join(o.relative_to(out).as_posix() for o in objs),
                   encoding="ascii")
    newest_o = max(o.stat().st_mtime for o in objs)
    ld = HERE / "video11.ld"
    if (not bin_path.exists() or bin_path.stat().st_mtime < max(newest_o, ld.stat().st_mtime)):
        run([CC, *ARCH, "-T", ld, "-nostartfiles", "-Wl,--gc-sections",
             "-Wl,--build-id=none", "-Wl,--no-warn-rwx-segments",
             "-Wl,--no-warn-execstack", "-Wl,-Map," + str(out / "video11.map"),
             "@objects.rsp", "-Wl,--start-group", "-lc", "-lm", "-lgcc",
             "-Wl,--end-group", "-o", elf], verbose, cwd=out)
        run([OBJCOPY, "-O", "binary", elf, bin_path], verbose)
    return bin_path


def main():
    path = build(verbose="-v" in sys.argv[1:])
    print(f"Video engine: {path.stat().st_size:,} bytes at {path}")


if __name__ == "__main__":
    main()
