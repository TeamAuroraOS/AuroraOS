"""PyInstaller entry point for the standalone aurc.exe.

Kept separate from compiler/aurc.py so the frozen build has a clean, single
top-level script that just delegates to the real driver.
"""
import sys

from compiler.aurc import main

if __name__ == "__main__":
    sys.exit(main())
