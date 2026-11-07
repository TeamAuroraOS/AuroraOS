"""Builds and runs the SDK runtime's host tests: the real runtime and OS
sources, compiled for the PC with devkitPro's msys gcc, against a model of the
ARM11 core, scripted servers and a FatFs RAM disk (see README.md).

    py sdk/tests/host/run.py [-v] [--keep] [TEST ...]

TEST names a test_*.c file without the prefix and suffix (sockets, dirent);
all of them run by default. The build goes to sdk/build/host.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
BUILD = ROOT / "sdk" / "build" / "host"
MSYS_BIN = Path(os.environ.get("MSYS_BIN", r"C:\devkitPro\msys2\usr\bin"))

GCC = MSYS_BIN / "gcc.exe"

# The real code under test, copied with its inline assembly taken out (cache
# and coprocessor operations, which mean nothing on a PC).
SOURCES = [
    "sdk/runtime/app_socket.c",
    "sdk/runtime/app_dirent.c",
    "sdk/runtime/app_net.c",
    "sdk/runtime/app_fs.c",
    "src/os/AppNet.c",
    "src/os/WiFiTcp.c",
    "src/os/WiFiUdp.c",
    "src/os/Net11.c",
    "src/ff.c",
    "src/ffunicode.c",
]
# Sources only one test links, alone: the ARM11 core's own code shares names
# with the app side (net_ping, for one), as two binaries may.
ONLY = {"net11": ["Net11.c"]}
# The harness's own files: those standing in for the OS, and those standing
# in for runtime files it does not build (with the runtime's headers).
HARNESS = ["stubs.c", "core_model.c", "ramdisk.c"]
HARNESS_RT = ["stubs_rt.c"]
# Names the runtime defines that the PC's C library has too.
RENAMED = ["app_socket.c", "app_dirent.c"]

WARN = ["-Wall", "-Wextra", "-Wno-unused-parameter", "-Wno-pointer-to-int-cast",
        "-Wno-int-to-pointer-cast", "-Wno-unused-function"]


def strip_asm(text: str) -> str:
    """Removes every __asm__ statement, balancing its parentheses."""
    out, i = [], 0
    pat = re.compile(r"__asm__\s*(volatile\s*)?\(")
    while True:
        m = pat.search(text, i)
        if not m:
            out.append(text[i:])
            return "".join(out)
        out.append(text[i:m.start()])
        j, depth = m.end(), 1
        while depth:
            c = text[j]
            if c == '"':
                j += 1
                while text[j] != '"':
                    j += 2 if text[j] == "\\" else 1
            elif c == "(":
                depth += 1
            elif c == ")":
                depth -= 1
            j += 1
        # A statement ends with ';'; as an expression it leaves a 0.
        k = j
        while text[k] in " \t":
            k += 1
        if text[k] == ";":
            out.append(";")
            i = k + 1
        else:
            out.append("0")
            i = j


def devkitarm() -> Path:
    """devkitARM's folder. Its DEVKITARM variable is often an MSYS path such
    as /opt/devkitpro/devkitARM, which Windows Python cannot open."""
    gcc = shutil.which("arm-none-eabi-gcc")
    for c in (os.environ.get("DEVKITARM"), gcc and Path(gcc).parents[1],
              r"C:\devkitPro\devkitARM"):
        if c and (Path(c) / "arm-none-eabi" / "include" / "sys" /
                  "dirent.h").exists():
            return Path(c)
    raise SystemExit("devkitARM not found (set DEVKITARM)")


def prepare() -> None:
    src = BUILD / "src"
    inc = BUILD / "include"
    src.mkdir(parents=True)
    (inc / "sys").mkdir(parents=True)
    for rel in SOURCES:
        p = ROOT / rel
        (src / p.name).write_bytes(strip_asm(p.read_bytes().decode("ascii"))
                                   .encode("ascii"))
    # FatFs with f_mkfs, for the RAM disk. ff.h comes along so its
    # "ffconf.h" is this one.
    conf = (ROOT / "include" / "ffconf.h").read_bytes().decode("ascii")
    conf, n = re.subn(r"#define FF_USE_MKFS\s+0", "#define FF_USE_MKFS 1", conf)
    if n != 1:
        raise SystemExit("ffconf.h: FF_USE_MKFS not found")
    (inc / "ffconf.h").write_bytes(conf.encode("ascii"))
    shutil.copy(ROOT / "include" / "ff.h", inc / "ff.h")
    # devkitARM's <dirent.h> (libsysbase's DIR), which app_dirent.c fills in,
    # with DIR_ITER lifted out of its iosupport.h: the rest of that header is
    # newlib's locks and devoptab, which the PC's C library does not have.
    dk = devkitarm() / "arm-none-eabi" / "include" / "sys"
    dirent = (dk / "dirent.h").read_bytes().decode("ascii")
    io = (dk / "iosupport.h").read_bytes().decode("ascii")
    m = re.search(r"typedef struct \{[^}]*\} DIR_ITER;", io)
    if not m or "#include <sys/iosupport.h>" not in dirent:
        raise SystemExit("devkitARM's dirent.h is not as expected")
    dirent = dirent.replace("#include <sys/iosupport.h>", m.group(0))
    dirent = dirent.replace("#include <sys/syslimits.h>",
                            "#include <limits.h>")
    (inc / "sys" / "dirent.h").write_bytes(dirent.encode("ascii"))
    (inc / "dirent.h").write_bytes(b"#include <sys/dirent.h>\n")


def mp(p) -> str:
    """A path as msys gcc takes it everywhere, -include included: /d/..."""
    s = Path(p).as_posix()
    return "/" + s[0].lower() + s[2:] if len(s) > 1 and s[1] == ":" else s


def env() -> dict:
    e = dict(os.environ)
    e["PATH"] = str(MSYS_BIN) + os.pathsep + e.get("PATH", "")
    tmp = BUILD / "tmp"
    tmp.mkdir(exist_ok=True)
    for k in ("TMP", "TEMP", "TMPDIR"):
        e[k] = str(tmp)
    return e


def cc(args: list, verbose: bool) -> None:
    cmd = [str(GCC)] + args
    if verbose:
        print(" ".join(cmd))
    r = subprocess.run(cmd, env=env(), capture_output=True, text=True)
    if r.stdout or r.stderr:
        sys.stdout.write(r.stdout + r.stderr)
    if r.returncode:
        raise SystemExit(f"compile failed: {args[-1]}")


def build(tests: list, verbose: bool) -> list:
    b = mp(BUILD)
    os_inc = ["-I" + mp(BUILD / "include"),
              "-I" + mp(ROOT / "include"),
              "-I" + mp(ROOT / "src" / "os")]
    sdk_inc = ["-I" + mp(HERE),
               "-I" + mp(ROOT / "sdk" / "include"),
               "-I" + mp(ROOT / "sdk" / "runtime")]
    base = ["-c", "-O1", "-g", "-std=gnu11", "-DAURORA_HOST_TEST"] + WARN
    objs = []
    for name in [Path(s).name for s in SOURCES]:
        o = f"{b}/{name}.o"
        extra = ["-include", mp(HERE / "rename.h")] \
            if name in RENAMED else []
        # The runtime's own headers first, as aurcc.py builds it.
        cc(base + extra + os_inc[:1] + sdk_inc + os_inc[1:] +
           [f"{b}/src/{name}", "-o", o], verbose)
        objs.append(o)
    for name in HARNESS:
        o = f"{b}/{name}.o"
        cc(base + os_inc + ["-I" + mp(HERE),
                            mp(HERE / name), "-o", o], verbose)
        objs.append(o)
    for name in HARNESS_RT:
        o = f"{b}/{name}.o"
        cc(base + os_inc[:1] + sdk_inc + os_inc[1:] +
           [mp(HERE / name), "-o", o], verbose)
        objs.append(o)
    exes = []
    for t in tests:
        o = f"{b}/test_{t}.o"
        cc(base + ["-include", mp(HERE / "rename.h")] + os_inc[:1] +
           sdk_inc + os_inc[1:] + [mp(HERE / f"test_{t}.c"), "-o", o],
           verbose)
        exe = BUILD / f"test_{t}.exe"
        mine = ONLY.get(t, [])
        held = {n for names in ONLY.values() for n in names} - set(mine)
        # A core test links the core's code alone.
        use = [x for x in objs
               if (Path(x).name[:-2] in mine if mine
                   else Path(x).name[:-2] not in held)]
        cmd = [str(GCC), o] + use + ["-o", mp(exe)]
        if verbose:
            print(" ".join(cmd))
        r = subprocess.run(cmd, env=env(), capture_output=True, text=True)
        if r.returncode:
            sys.stdout.write(r.stdout + r.stderr)
            raise SystemExit(f"link failed: test_{t}")
        exes.append((t, str(exe)))
    return exes


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("tests", nargs="*")
    ap.add_argument("-v", "--verbose", action="store_true")
    ap.add_argument("--keep", action="store_true",
                    help="keep the build folder from the last run")
    a = ap.parse_args(argv)
    if not GCC.exists():
        print(f"msys gcc not found at {GCC} (set MSYS_BIN)")
        return 2
    known = sorted(p.stem[5:] for p in HERE.glob("test_*.c"))
    tests = a.tests or known
    for t in tests:
        if t not in known:
            print(f"no test called {t}; there are: {', '.join(known)}")
            return 2
    if BUILD.exists() and not a.keep:
        shutil.rmtree(BUILD)
    if not BUILD.exists():
        prepare()
    failed = 0
    for t, exe in build(tests, a.verbose):
        # Each test is its own process, so one cannot leave state for another.
        r = subprocess.run([exe], env=env(), capture_output=True, text=True,
                           timeout=600)
        sys.stdout.write(r.stdout + r.stderr)
        if r.returncode:
            failed += 1
            print(f"test_{t}: FAILED (exit {r.returncode})")
    print("all host tests passed" if not failed else f"{failed} test(s) failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
