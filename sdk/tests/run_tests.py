"""Run the Aurora SDK test layers from any working directory.

Examples:
    python sdk/tests/run_tests.py
    python sdk/tests/run_tests.py --cross-build
"""
from __future__ import annotations

import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

SDK = Path(__file__).resolve().parents[1]
ROOT = SDK.parent


def run(command: list[str], *, env: dict[str, str] | None = None) -> None:
    print("$ " + " ".join(command), flush=True)
    subprocess.run(command, cwd=ROOT, env=env, check=True)


def host_tests() -> None:
    run([sys.executable, str(SDK / "tests" / "test_aurcc.py")])
    run([sys.executable, str(SDK / "tests" / "test_python_sdk.py")])


def cross_build() -> None:
    """Build the checked-in Python fixture and validate its AURC container."""
    env = dict(os.environ)
    env["PYTHONPATH"] = os.pathsep.join(
        [str(SDK), env["PYTHONPATH"]] if env.get("PYTHONPATH")
        else [str(SDK)]
    )
    with tempfile.TemporaryDirectory(prefix="aurora-sdk-test-") as directory:
        out = Path(directory) / "PythonFixture.bin"
        build_dir = Path(directory) / "build"
        fixture = SDK / "tests" / "fixtures" / "python_app"
        run([
            sys.executable, "-m", "aurora_sdk.cli", "build", str(fixture),
            "--output", str(out), "--build-dir", str(build_dir),
        ], env=env)
        data = out.read_bytes()
        if data[:4] != b"AURC":
            raise AssertionError(f"unexpected app magic: {data[:4]!r}")
        if len(data) <= 36:
            raise AssertionError("generated app has no payload")
        print(f"Validated {out} ({len(data):,} bytes)", flush=True)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Run Aurora SDK tests")
    parser.add_argument(
        "--cross-build", action="store_true",
        help="also compile the Python fixture with arm-none-eabi-gcc",
    )
    args = parser.parse_args(argv)
    host_tests()
    if args.cross_build:
        cross_build()
    print("Aurora SDK tests passed.", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
