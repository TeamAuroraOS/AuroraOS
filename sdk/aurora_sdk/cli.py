"""Command-line interface for the Aurora Python SDK."""
from __future__ import annotations

import argparse
import shutil
import sys
import tempfile
from pathlib import Path

from .compiler import PythonBuildError, compile_file
from .project import create_project

# aurcc remains a public source module and is packaged alongside this SDK.
from aurcc import build as build_c


def build(project: Path, output: Path | None, build_dir: Path | None) -> int:
    source = project / "main.py" if project.is_dir() else project
    if not source.is_file():
        raise PythonBuildError(f"Python source not found: {source}")
    icon = source.parent / f"{source.parent.name}.icon"
    if not icon.exists():
        icon = None
    with tempfile.TemporaryDirectory(prefix="aurora-python-") as temp:
        generated = Path(temp) / "main.c"
        compile_file(source, generated)
        build_c([generated], output, icon=icon, build_dir=build_dir or
                (source.parent / "build"))
    return 0


def install(binary: Path, sd_root: Path) -> Path:
    if binary.suffix.lower() != ".bin" or not binary.is_file():
        raise PythonBuildError(f"an existing .bin app is required: {binary}")
    destination = sd_root / "Aurora" / "Apps" / "C" / binary.name
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(binary, destination)
    return destination


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="aurora-python")
    sub = parser.add_subparsers(dest="command", required=True)
    new = sub.add_parser("new", help="create a Python app project")
    new.add_argument("name")
    new.add_argument("--dir", type=Path)
    build_parser = sub.add_parser("build", help="build a Python app into AURC")
    build_parser.add_argument("project", type=Path)
    build_parser.add_argument("-o", "--output", type=Path)
    build_parser.add_argument("--build-dir", type=Path)
    install_parser = sub.add_parser(
        "install", help="copy an AURC app to a mounted SD-card root")
    install_parser.add_argument("binary", type=Path)
    install_parser.add_argument("--sd-root", type=Path, required=True)
    doctor = sub.add_parser("doctor", help="check Python SDK host prerequisites")

    args = parser.parse_args(argv)
    try:
        if args.command == "new":
            root = create_project(args.name, args.dir)
            print(f"Created {root}/main.py and {root.name}.icon")
            return 0
        if args.command == "doctor":
            print(f"Python: {sys.version.split()[0]}")
            print(f"arm-none-eabi-gcc: {shutil.which('arm-none-eabi-gcc') or 'NOT FOUND'}")
            print("Python source compiler: available")
            return 0
        if args.command == "install":
            destination = install(args.binary, args.sd_root)
            print(f"Installed {args.binary} to {destination}")
            return 0
        output = args.output or Path(args.project.stem + ".bin")
        return build(args.project, output, args.build_dir)
    except (PythonBuildError, OSError) as exc:
        print(f"Error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
