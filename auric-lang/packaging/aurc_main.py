"""PyInstaller entry point for aurc.exe; delegates to compiler/aurc.py."""
import sys

from compiler.aurc import main

if __name__ == "__main__":
    sys.exit(main())
