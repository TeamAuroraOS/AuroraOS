# AuroraOS SDK testing

This document covers the host, cross-build, and console tests for the C and
Python SDKs. Run tests from the repository root unless a command says
otherwise.

## 1. Host prerequisites

Required for host-only tests:

- Python 3.8 or newer
- A checked-out AuroraOS repository

Required for cross-build tests:

- `arm-none-eabi-gcc`
- `arm-none-eabi-objcopy`
- `arm-none-eabi-nm`
- devkitARM's newlib (`libnewlib-arm-none-eabi` on Debian/Ubuntu)

Check the toolchain with:

```sh
python sdk/aurcc.py doctor
python -m pip --version
```

The doctor command must report all three ARM tools and the SDK source
directories as `[ok]`. If `arm-none-eabi-gcc` is missing, install devkitARM
from the [devkitPro setup guide](https://devkitpro.org/wiki/Getting_Started)
and put its `bin` directory on `PATH`. Do not use devkitPro's MSYS Python for
Windows host-side commands if it cannot read normal Windows paths.

## 2. Install the Python SDK locally

The package is intentionally dependency-free:

```sh
python -m pip install --upgrade pip
python -m pip install ./sdk
aurora-python doctor
```

Expected `doctor` output includes the Python version, an ARM compiler path (or
`NOT FOUND` when only host tests are being run), and:

```text
Python source compiler: available
```

For an editable checkout during development:

```sh
python -m pip install --editable ./sdk
```

## 3. Run host-only tests

The complete host test runner works without an ARM toolchain:

```sh
python sdk/tests/run_tests.py
```

It runs:

1. The existing C SDK/compiler tests in
   [`tests/test_aurcc.py`](tests/test_aurcc.py).
2. Python source validation, project scaffolding, and generated-C tests in
   [`tests/test_python_sdk.py`](tests/test_python_sdk.py).

The command should finish with:

```text
Aurora SDK tests passed.
```

Run either suite separately when debugging:

```sh
python sdk/tests/test_aurcc.py
python sdk/tests/test_python_sdk.py
```

The tests use the standard library `unittest` runner, so no pytest install is
required.

## 4. Run cross-build tests

The cross-build test compiles the checked-in Python fixture at
[`tests/fixtures/python_app/`](tests/fixtures/python_app/) through the native
SDK pipeline and verifies the generated container starts with `AURC`.

```sh
python sdk/tests/run_tests.py --cross-build
```

This creates all intermediate objects in a temporary directory and does not
modify `sdk/build`. A successful run reports a validated `.bin` size. The
fixture is a build test only; it is also used for the hardware smoke test
below.

Build the shipped C examples independently:

```sh
for app in hello demo bricks net; do
  python sdk/aurcc.py build "sdk/examples/$app" \
    -o "/tmp/aurora-$app.bin" \
    --build-dir "/tmp/aurora-sdk-build-$app"
done
```

Build a Python project manually:

```sh
aurora-python new MyGame
aurora-python build MyGame -o /tmp/MyGame.bin \
  --build-dir /tmp/aurora-python-build
```

Verify the container magic and header size:

```sh
python - <<'PY'
from pathlib import Path
import struct

path = Path("/tmp/MyGame.bin")
data = path.read_bytes()
magic, offset, payload_size = struct.unpack("<4sII", data[:12])
assert magic == b"AURC", magic
assert offset == 36, offset
assert offset + payload_size == len(data)
print(f"{path}: {len(data):,} bytes, valid AURC header")
PY
```

## 5. CLI and negative tests

Confirm project creation and clean failure for unsupported Python:

```sh
tmp="$(mktemp -d)"
aurora-python new SmokeTest --dir "$tmp"
test -f "$tmp/SmokeTest/main.py"
test -f "$tmp/SmokeTest/SmokeTest.icon"

cat > "$tmp/bad.py" <<'PY'
import os

def main():
    print("unsupported")
PY

if aurora-python build "$tmp/bad.py" -o "$tmp/bad.bin"; then
  echo "unexpected successful build" >&2
  exit 1
fi
```

The last command must fail with an explicit unsupported-import error. Other
expected build-time failures include:

- Missing `main()`.
- A second function besides `main()`.
- Calls outside `aurora.<function>(...)`.
- Keyword arguments.
- Unsupported expressions or Python statements.
- A missing or non-32x32 icon when one is supplied.
- A native app image exceeding the existing 15 MiB app-image limit.

These checks are important: this SDK compiles a defined Python subset and does
not silently claim arbitrary CPython compatibility.

## 6. SD-card installation test

Only perform this test with a mounted Aurora SD card or a disposable directory
that mirrors its layout. Do not point `--sd-root` at the repository root.

For a mounted card:

```sh
aurora-python install /tmp/MyGame.bin --sd-root /media/$USER/AURORA
test -f /media/$USER/AURORA/Aurora/Apps/C/MyGame.bin
sync
```

On Windows, use the mounted drive root, for example:

```powershell
aurora-python install MyGame.bin --sd-root E:\
Test-Path E:\Aurora\Apps\C\MyGame.bin
```

Safely eject the card after `sync` or the operating system's eject action.

## 7. Console smoke test

Use an Aurora console running Beta 6 v0.1.5 or newer, because older versions
reject `AURC` containers.

1. Insert the prepared SD card and boot AuroraOS.
2. Confirm the app appears under the C SDK apps location in the Home Menu.
3. Launch `MyGame`.
4. Confirm the top screen is cleared and the generated app draws its test
   shape/text.
5. Press a recognized button and confirm the app exits to the Home Menu.
6. Launch it again and confirm it can be relaunched after returning Home.
7. Launch it from the Terminal or Files app, if available, and confirm the
   same app image starts.
8. Press HOME while the frame loop is active and confirm the Home Menu returns.

For the fixture, the expected result is a black top screen with an Aurora
colored circle. The fixture exits when any button is pressed.

Record the following for each hardware run:

- AuroraOS version and console model.
- App filename and byte size.
- Launch source (Home Menu, Files, or Terminal).
- Whether drawing, input, HOME, and relaunch worked.
- Any crash-screen text or terminal output.

## 8. Troubleshooting

### `arm-none-eabi-gcc was not found`

Install devkitARM and update `PATH`, then rerun `python sdk/aurcc.py doctor`.
Host-only tests do not require the ARM compiler.

### `aurora-python: command not found`

The package's scripts directory is not on `PATH`. Run
`python -m pip show auroraos-sdk`, add the reported environment's `bin`
directory to `PATH`, or use:

```sh
python -m aurora_sdk.cli doctor
```

### The build rejects valid-looking Python

Check the supported subset in [sdk/README.md](README.md). The compiler is
intentionally not a general Python interpreter. Reduce the app to `main()`,
`import aurora`, local integer variables, control flow, literals, and the
documented `aurora` calls.

### The app does not appear on the console

Verify the file is under `Aurora/Apps/C`, has an `.bin` suffix, starts with
`AURC`, and is being tested on a supported AuroraOS version. Rebuild with a
different output name if the Home Menu is retaining stale metadata.

### The app builds but crashes on hardware

Re-run the fixture cross-build, test the smallest generated app first, and
record the crash-screen text. Confirm the binary was copied completely and
that the console was not running an older copy from another app directory.
