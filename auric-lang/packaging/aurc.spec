# PyInstaller spec for the standalone Auric compiler (aurc.exe).
#
# Bundles, alongside the frozen Python front end:
#   * the runtime shim + crt0 + linker script (runtime/)
#   * the AUR1 packer (tools/aur_pack.py)
#   * a snapshot of the AuroraOS headers + src/screen.c the runtime compiles
#     against, so the exe is self-contained and needs no AuroraOS checkout.
#
# Build it with:  python packaging/build_exe.py   (run from auric-lang/)
# or directly:    pyinstaller packaging/aurc.spec --distpath dist --workpath build/pyinstaller
import os

from PyInstaller.utils.hooks import collect_submodules

HERE = SPECPATH                        # auric-lang/packaging/
AURIC = os.path.dirname(HERE)          # auric-lang/
AURORA = os.path.dirname(AURIC)        # aurora/  (AuroraOS repo root)

# The AuroraOS files the runtime build reuses: screen.c + i2c.c and exactly the
# headers they (transitively) include and the runtime needs (loader.h carries
# the HOME-return contract addresses; i2c.h the MCU access).
AURORA_HEADERS = ["aurora.h", "aurora_logo.h", "font.h", "icons.h",
                  "loader.h", "i2c.h"]

datas = [
    (os.path.join(AURIC, "runtime", "auric_runtime.c"), "runtime"),
    (os.path.join(AURIC, "runtime", "auric_runtime.h"), "runtime"),
    (os.path.join(AURIC, "runtime", "auric_start.s"), "runtime"),
    (os.path.join(AURIC, "runtime", "auric.ld"), "runtime"),
    (os.path.join(AURIC, "tools", "aur_pack.py"), "tools"),
    (os.path.join(AURORA, "src", "screen.c"), "aurora_src"),
    (os.path.join(AURORA, "src", "i2c.c"), "aurora_src"),
]
for h in AURORA_HEADERS:
    datas.append((os.path.join(AURORA, "include", h), "aurora_include"))

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
