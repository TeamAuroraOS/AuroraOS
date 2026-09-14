# PyInstaller spec for aurc.exe. Bundles the runtime, the AUR1 packer, and the
# AuroraOS headers and sources that apps link, so the exe needs no AuroraOS
# checkout.
#
# Build: python packaging/build_exe.py (from auric-lang/)
import os

from PyInstaller.utils.hooks import collect_submodules

HERE = SPECPATH                        # auric-lang/packaging/
AURIC = os.path.dirname(HERE)          # auric-lang/
AURORA = os.path.dirname(AURIC)        # aurora/  (AuroraOS repo root)

# The AuroraOS files an app build reuses, placed where aurc.py looks for them in
# the bundle.
import glob

AURORA_SOURCES = [
    ("screen.c", "aurora_src"),
    ("i2c.c", "aurora_src"),
    (os.path.join("os", "Gpu9.c"), os.path.join("aurora_src", "os")),
    ("wavload.c", "aurora_src"),
    ("ff.c", "aurora_src"),
    ("ffunicode.c", "aurora_src"),
    ("diskio.c", "aurora_src"),
    ("sdmmc.c", "aurora_src"),
    ("string.c", "aurora_src"),
]

datas = [
    (os.path.join(AURIC, "runtime", "auric_runtime.c"), "runtime"),
    (os.path.join(AURIC, "runtime", "auric_runtime.h"), "runtime"),
    (os.path.join(AURIC, "runtime", "auric_start.s"), "runtime"),
    (os.path.join(AURIC, "runtime", "auric.ld"), "runtime"),
    (os.path.join(AURIC, "tools", "aur_pack.py"), "tools"),
]
for src, dest in AURORA_SOURCES:
    datas.append((os.path.join(AURORA, "src", src), dest))
for h in sorted(glob.glob(os.path.join(AURORA, "include", "*.h"))):
    datas.append((h, "aurora_include"))

a = Analysis(
    [os.path.join(HERE, "aurc_main.py")],
    pathex=[AURIC],
    binaries=[],
    datas=datas,
    hiddenimports=collect_submodules("compiler"),
    hookspath=[],
    runtime_hooks=[],
    excludes=["tkinter", "numpy", "PyInstaller"],
    noarchive=False,
)
pyz = PYZ(a.pure)

exe = EXE(
    pyz,
    a.scripts,
    a.binaries,
    a.datas,
    [],
    name="aurc",
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=False,
    console=True,
    disable_windowed_traceback=False,
    target_arch=None,
    codesign_identity=None,
    entitlements_file=None,
)
