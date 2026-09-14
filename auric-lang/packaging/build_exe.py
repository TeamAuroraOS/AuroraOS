"""Build auric-lang/dist/aurc.exe with PyInstaller (run from auric-lang/). The
exe still needs arm-none-eabi-gcc on PATH.
"""
import sys
from pathlib import Path

PACKAGING_DIR = Path(__file__).resolve().parent
AURIC_ROOT = PACKAGING_DIR.parent
SPEC = PACKAGING_DIR / "aurc.spec"
DIST = AURIC_ROOT / "dist"
WORK = AURIC_ROOT / "build" / "pyinstaller"


def main() -> int:
    try:
        import PyInstaller.__main__ as pyi
    except ImportError:
        print("PyInstaller is not installed. Install it with:\n"
              "    python -m pip install pyinstaller", file=sys.stderr)
        return 1

    print(f"Building aurc.exe -> {DIST / 'aurc.exe'}")
    pyi.run([
        str(SPEC),
        "--distpath", str(DIST),
        "--workpath", str(WORK),
        "--noconfirm",
    ])
    exe = DIST / ("aurc.exe" if sys.platform == "win32" else "aurc")
    if exe.exists():
        print(f"\nDone: {exe}")
        return 0
    print("\nBuild finished but the expected executable was not found.",
          file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
