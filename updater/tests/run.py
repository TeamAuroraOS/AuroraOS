"""Host tests for system updates (docs/update.md), built for the PC with
devkitPro's msys gcc:

- the updater (updater/main.c), with the OS's JSON reader and BearSSL's
  SHA-256, against a fake SDK (fake_sdk.c) whose SD card is a folder and whose
  update server is a folder of files. Each scenario runs it once and checks
  what it left on the card.
- the OS's side (src/os/Update.c, test_os.c): restore, the firm's path and the
  clean-up at start, on the real FatFs over a RAM disk.

    py updater/tests/run.py [-v] [SCENARIO ...]    (os is one of them)
"""
import re
import argparse
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
BUILD = ROOT / "build" / "updater-test"
MSYS_BIN = Path(os.environ.get("MSYS_BIN", r"C:\devkitPro\msys2\usr\bin"))
EXE = BUILD / "updater_host.exe"
OS_EXE = BUILD / "os_update_host.exe"

SERVER = "https://update.aurora3ds.xyz"
FIRM = "/luma/payloads/Aurora.firm"


def env() -> dict:
    e = dict(os.environ)
    e["PATH"] = str(MSYS_BIN) + os.pathsep + e.get("PATH", "")
    for v in ("TMPDIR", "TMP", "TEMP"):
        e[v] = str(BUILD)
    return e


def rel(p) -> str:
    return os.path.relpath(p, ROOT).replace(chr(92), "/")


WARN = ["-Wall", "-Wextra", "-Wno-unused-parameter"]


def compile_units(units, verbose: bool) -> list:
    objs = []
    for files, flags in units:
        for f in files:
            obj = BUILD / (Path(f).stem + ".o")
            cmd = [str(MSYS_BIN / "gcc.exe"), "-c", "-O1", "-g", "-std=gnu11",
                   *WARN, *flags, f, "-o", rel(obj)]
            if verbose:
                print(" ".join(cmd))
            r = subprocess.run(cmd, cwd=ROOT, env=env(), capture_output=True,
                               text=True)
            if r.returncode or r.stderr.strip():
                print(r.stdout + r.stderr)
            if r.returncode:
                sys.exit("build failed")
            objs.append(rel(obj))
    return objs


def build_os(verbose: bool) -> None:
    """Update.c with the real FatFs (f_mkfs on, for the RAM disk), the
    container and JSON readers, and empty stubs for whatever else it calls:
    the screens, the network, which the tested paths do not reach."""
    inc = BUILD / "inc"
    inc.mkdir(parents=True, exist_ok=True)
    conf = (ROOT / "include" / "ffconf.h").read_text()
    (inc / "ffconf.h").write_text(re.sub(r"#define FF_USE_MKFS\s+0",
                                         "#define FF_USE_MKFS 1", conf))
    # ff.h takes ffconf.h from its own folder, so it comes along.
    shutil.copy(ROOT / "include" / "ff.h", inc / "ff.h")
    flags = ["-DAURORA_HOST_TEST", "-I" + rel(inc), "-Iinclude", "-Isrc/os",
             "-Ilib/bearssl/inc"]
    objs = compile_units([
        (["src/os/Update.c", rel(HERE / "test_os.c"), "src/ff.c",
          "src/ffunicode.c", "src/container.c", "src/os/Json.c"], flags),
    ], verbose)
    # BearSSL's SHA-256, built by build() for the updater.
    objs += [rel(BUILD / n) for n in ("sha2small.o", "dec32be.o", "enc32be.o")]
    stubs = BUILD / "os_stubs.c"
    stubs.write_text("")
    for _ in range(2):
        cmd = [str(MSYS_BIN / "gcc.exe"), "-c", rel(stubs), "-o",
               rel(BUILD / "os_stubs.o")]
        subprocess.run(cmd, cwd=ROOT, env=env(), check=True)
        cmd = [str(MSYS_BIN / "gcc.exe"), *objs, rel(BUILD / "os_stubs.o"),
               "-o", rel(OS_EXE)]
        r = subprocess.run(cmd, cwd=ROOT, env=env(), capture_output=True,
                           text=True)
        if not r.returncode:
            return
        missing = sorted(set(re.findall(r"undefined reference to `(\w+)'",
                                        r.stdout + r.stderr)))
        if not missing:
            print(r.stdout + r.stderr)
            sys.exit("link failed")
        stubs.write_text("".join(f"void {m}(void) {{}}\n" for m in missing))
    sys.exit("link failed after the stubs")


def build(verbose: bool) -> None:
    BUILD.mkdir(parents=True, exist_ok=True)
    units = [
        (["updater/main.c"], ["-include", rel(HERE / "shim.h"), "-Isdk/include",
                              "-Iinclude", "-Ilib/bearssl/inc"]),
        ([rel(HERE / "fake_sdk.c")], ["-Isdk/include"]),
        ([rel(HERE / "test_main.c")], []),
        (["src/os/Json.c"], ["-Iinclude"]),
        (["lib/bearssl/src/hash/sha2small.c", "lib/bearssl/src/codec/dec32be.c",
          "lib/bearssl/src/codec/enc32be.c"],
         ["-Ilib/bearssl/inc", "-Ilib/bearssl/src"]),
    ]
    objs = compile_units(units, verbose)
    cmd = [str(MSYS_BIN / "gcc.exe"), *objs, "-o", rel(EXE)]
    r = subprocess.run(cmd, cwd=ROOT, env=env(), capture_output=True, text=True)
    if r.returncode:
        print(r.stdout + r.stderr)
        sys.exit("link failed")


def aos(payload: bytes) -> bytes:
    return struct.pack("<4sIIIIIIII", b"AOS1", 36, len(payload), 0x22000000,
                       0x22000000, 0, 0, 0, 0) + payload


def blob(n: int, seed: int) -> bytes:
    return bytes((i * 31 + seed * 7 + (i >> 9)) & 0xFF for i in range(n))


OLD_OS, NEW_OS = aos(blob(40000, 1)), aos(blob(52000, 2))
OLD_PAK, NEW_PAK = blob(30000, 3), blob(31000, 4)
OLD_FIRM, NEW_FIRM = blob(9000, 5), blob(9500, 6)


class Scene:
    def __init__(self, name: str):
        self.dir = BUILD / name
        shutil.rmtree(self.dir, ignore_errors=True)
        self.sd = self.dir / "sd"
        self.server = self.dir / "server"
        (self.server / "files").mkdir(parents=True)
        self.put("/AURORAOS.BIN", OLD_OS)
        self.put("/Aurora/assets.pak", OLD_PAK)
        self.put(FIRM, OLD_FIRM)
        self.put("/Aurora/Update/Update.bin", b"updater")
        self.plan(firm=FIRM)
        self.files = []
        self.env = {}

    def put(self, path: str, data: bytes) -> None:
        p = self.sd / path.lstrip("/")
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)

    def get(self, path: str):
        p = self.sd / path.lstrip("/")
        return p.read_bytes() if p.exists() else None

    def plan(self, firm: str) -> None:
        self.put("/Aurora/Update/plan.txt",
                 ("current=0.1.7-everest\r\nbuild=20261123\r\n"
                  "from=Beta 8 v0.1.7-everest\r\nfirm=%s\r\n" % firm).encode())

    def serve(self, name: str, kind: str, path: str, data: bytes) -> None:
        sha = hashlib.sha256(data).hexdigest()
        (self.server / "files" / sha).write_bytes(data)
        self.files.append({"name": name, "kind": kind, "path": path,
                           "sha256": sha, "size": len(data),
                           "url": f"{SERVER}/v1/files/{sha}"})

    def release(self, update=True) -> None:
        body = {"update": update, "current": "0.1.7-everest",
                "name": "Beta 9 v0.1.8", "version": "0.1.8",
                "tag": "Beta-9-v0.1.8", "published": "2026-12-01"}
        if update:
            body.update(size=sum(f["size"] for f in self.files), notes="Notes.",
                        updater={"url": f"{SERVER}/v1/files/{'0' * 64}",
                                 "sha256": "0" * 64, "size": 7},
                        files=self.files)
        (self.server / "manifest.json").write_text(json.dumps(body))

    def run(self, verbose: bool):
        e = env()
        e.update(UT_ROOT=str(self.sd), UT_SERVER=str(self.server),
                 UT_LOG=str(self.dir / "screen.log"),
                 UT_REPORT=str(self.dir / "report.bin"), **self.env)
        r = subprocess.run([str(EXE)], env=e, capture_output=True, text=True,
                           timeout=60)
        self.code = r.returncode
        self.log = (self.dir / "screen.log").read_text(errors="replace") \
            if (self.dir / "screen.log").exists() else ""
        rep = (self.dir / "report.bin").read_bytes() \
            if (self.dir / "report.bin").exists() else b"\0\0\0\0"
        size = struct.unpack("<I", rep[:4])[0]
        self.snapshot = rep[4:4 + size]
        if verbose:
            print(r.stdout, r.stderr, self.log, sep="\n")
        return self


FAILS = []


def check(scene: str, what: str, ok: bool) -> None:
    print(("  ok   " if ok else "  FAIL ") + what)
    if not ok:
        FAILS.append(f"{scene}: {what}")


def standard(s: Scene) -> None:
    s.serve("AURORAOS.BIN", "os", "/AURORAOS.BIN", NEW_OS)
    s.serve("assets.pak", "assets", "/Aurora/assets.pak", OLD_PAK)
    s.serve("Aurora.firm", "firm", "", NEW_FIRM)


def unchanged(name: str, s: Scene) -> None:
    check(name, "the card's OS is unchanged", s.get("/AURORAOS.BIN") == OLD_OS)
    check(name, "the card's assets are unchanged",
          s.get("/Aurora/assets.pak") == OLD_PAK)
    check(name, "the card's firm is unchanged", s.get(FIRM) == OLD_FIRM)
    check(name, "nothing is staged for the return stub", s.snapshot == b"")


def sc_update(v):
    s = Scene("update")
    standard(s)
    s.release()
    s.run(v)
    n = "update"
    check(n, "left through exit(0)", s.code == 0)
    check(n, "the new OS is in place", s.get("/AURORAOS.BIN") == NEW_OS)
    check(n, "the new firm is in place", s.get(FIRM) == NEW_FIRM)
    check(n, "assets.pak, already current, was not downloaded",
          "assets.pak ->" not in s.log and s.get("/Aurora/assets.pak") == OLD_PAK)
    b = "/Aurora/Update/backup/"
    check(n, "the old OS is in the backup", s.get(b + "AURORAOS.BIN") == OLD_OS)
    check(n, "the old firm is in the backup", s.get(b + "Aurora.firm") == OLD_FIRM)
    check(n, "the unchanged assets.pak is not", s.get(b + "assets.pak") is None)
    info = (s.get(b + "info.txt") or b"").decode()
    check(n, "info.txt names the old version and the firm",
          "name=Beta 8 v0.1.7-everest" in info and f"firm={FIRM}" in info)
    res = (s.get("/Aurora/Update/result.txt") or b"").decode()
    check(n, "result.txt reports success",
          "ok=1" in res and "to=Beta 9 v0.1.8" in res)
    check(n, "plan.txt and new/ are gone",
          s.get("/Aurora/Update/plan.txt") is None
          and not (s.sd / "Aurora/Update/new").exists())
    check(n, "the new OS payload waits for the return stub",
          s.snapshot == NEW_OS[36:])
    check(n, "asked with the current version and build",
          "current=0.1.7-everest&build=20261123" in s.log)


def sc_corrupt(v):
    s = Scene("corrupt")
    standard(s)
    s.release()
    s.env["UT_CORRUPT"] = hashlib.sha256(NEW_OS).hexdigest()
    s.run(v)
    n = "corrupt"
    check(n, "stopped with a message", "did not download" in s.log)
    check(n, "tried twice, then stopped before the firm",
          s.log.count("[download") == 2)
    unchanged(n, s)
    check(n, "no result.txt (nothing was swapped)",
          s.get("/Aurora/Update/result.txt") is None)


def sc_rollback(v):
    s = Scene("rollback")
    standard(s)
    s.serve("extra.bin", "other", "/Aurora/extra.bin", blob(100, 9))
    s.release()
    s.env["UT_FAIL_RENAME_ONCE"] = FIRM  # the last swap fails
    s.run(v)
    n = "rollback"
    check(n, "stopped with a message", "could not be moved" in s.log)
    check(n, "said nothing was changed", "and some could not" not in s.log)
    unchanged(n, s)
    check(n, "the extra file placed before was taken back",
          s.get("/Aurora/extra.bin") is None)
    res = (s.get("/Aurora/Update/result.txt") or b"").decode()
    check(n, "result.txt says nothing changed",
          "ok=0" in res and "Nothing was changed" in res)


def sc_stuck(v):
    s = Scene("stuck")
    standard(s)
    s.release()
    s.env["UT_FAIL_RENAME"] = FIRM  # the firm can neither go in nor back
    s.run(v)
    n = "stuck"
    check(n, "says some files could not be put back",
          "and some could not be put back" in s.log)
    check(n, "the OS swapped before was put back",
          s.get("/AURORAOS.BIN") == OLD_OS)
    check(n, "the old firm is kept in the backup",
          s.get("/Aurora/Update/backup/Aurora.firm") == OLD_FIRM)
    res = (s.get("/Aurora/Update/result.txt") or b"").decode()
    check(n, "result.txt tells the OS", "ok=0" in res and "put back" in res)
    check(n, "nothing is staged for the return stub", s.snapshot == b"")


def sc_uptodate(v):
    s = Scene("uptodate")
    s.release(update=False)
    s.run(v)
    n = "uptodate"
    check(n, "says it is up to date", "AuroraOS is up to date" in s.log)
    unchanged(n, s)


def sc_battery(v):
    s = Scene("battery")
    standard(s)
    s.release()
    s.env.update(UT_BATTERY="12", UT_CHARGING="0")
    s.run(v)
    n = "battery"
    check(n, "refuses on a low battery", "battery is at 12%" in s.log)
    check(n, "before asking the server", "[GET" not in s.log)
    unchanged(n, s)
    s2 = Scene("battery_charging")
    standard(s2)
    s2.release()
    s2.env.update(UT_BATTERY="12", UT_CHARGING="1")
    s2.run(v)
    check(n, "goes ahead on the charger", s2.get("/AURORAOS.BIN") == NEW_OS)


def sc_nofirm(v):
    s = Scene("nofirm")
    s.plan(firm="")
    standard(s)
    s.release()
    s.run(v)
    n = "nofirm"
    check(n, "the OS was updated", s.get("/AURORAOS.BIN") == NEW_OS)
    check(n, "the firm was left alone", s.get(FIRM) == OLD_FIRM)
    res = (s.get("/Aurora/Update/result.txt") or b"").decode()
    check(n, "result.txt explains", "note=The firm was not updated" in res)


def sc_badpath(v):
    s = Scene("badpath")
    standard(s)
    s.serve("evil.bin", "other", "/Aurora/../../evil.bin", b"x")
    s.release()
    s.run(v)
    n = "badpath"
    check(n, "refuses a path with ..", "will not write" in s.log)
    unchanged(n, s)


def sc_noreturn(v):
    s = Scene("noreturn")
    standard(s)
    s.release()
    s.env["UT_NO_RETURN"] = "1"
    s.run(v)
    n = "noreturn"
    check(n, "installed", s.get("/AURORAOS.BIN") == NEW_OS)
    check(n, "asks for a power cycle when it cannot start it",
          s.code == 4 and "Turn the console off and on" in s.log)


def sc_noplan(v):
    s = Scene("noplan")
    (s.sd / "Aurora/Update/plan.txt").unlink()
    standard(s)
    s.release()
    s.run(v)
    n = "noplan"
    check(n, "refuses to run without the OS's plan",
          "Start the update from" in s.log)
    unchanged(n, s)


def sc_os(v):
    r = subprocess.run([str(OS_EXE)], env=env(), capture_output=True,
                       text=True, timeout=60)
    for line in r.stdout.splitlines():
        line = line.rstrip()
        if line.startswith("  FAIL"):
            FAILS.append("os: " + line[7:])
        if line and line not in ("all passed", "FAILED"):
            print(line)
    if r.returncode not in (0, 1):
        FAILS.append(f"os: exit {r.returncode} {r.stderr.strip()}")


SCENARIOS = {
    "update": sc_update, "corrupt": sc_corrupt, "rollback": sc_rollback,
    "stuck": sc_stuck,
    "uptodate": sc_uptodate, "battery": sc_battery, "nofirm": sc_nofirm,
    "badpath": sc_badpath, "noreturn": sc_noreturn, "noplan": sc_noplan,
    "os": sc_os,
}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("-v", "--verbose", action="store_true")
    ap.add_argument("scenarios", nargs="*")
    a = ap.parse_args()
    build(a.verbose)
    build_os(a.verbose)
    for name in a.scenarios or SCENARIOS:
        print(name)
        SCENARIOS[name](a.verbose)
    print(f"\n{len(FAILS)} failed" if FAILS else "\nall passed")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
