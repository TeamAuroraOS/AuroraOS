# Auric

**Coding too hard? Try Auric!**

Auric is a small, statically-typed programming language for the
[AuroraOS](../README.md) ecosystem on the Nintendo 3DS. You write `.aur` source;
`aurc` compiles it — by transpiling to freestanding C and reusing AuroraOS's own
ARM9 toolchain — into a bootable **`AUR1`** app container that runs on the same
loader that boots AuroraOS itself.

```auric
fn main() {
    clear(BLACK);
    print("Coding too hard? Try Auric!", 40, 108, AURORA);
    print("Press A to exit.", 40, 140, WHITE);
    wait_key(KEY_A);
}
```

## How it works

```
 hello.aur
   │  lexer → parser → type checker → C code generator   (pure Python, compiler/)
   ▼
 hello.c                       generated freestanding C
   │  arm-none-eabi-gcc        AuroraOS's exact ARM9 flags (from ../Makefile)
   │  + runtime/auric_runtime.c + ../src/screen.c + runtime/auric_start.s
   ▼
 hello.elf  →  objcopy -O binary  →  hello.payload.bin
   │  tools/aur_pack.py
   ▼
 hello.bin                     AUR1 container (36-byte header + ARM9 payload)
```

The generated program is linked at `0x22000000` and run in place — exactly like
AuroraOS's own payload (`src/os/os.ld`). The runtime shim maps the four Auric
built-ins onto AuroraOS's `draw_string` / `clear_screen` / `get_keys_down` /
`delay`, reusing the real `src/screen.c` for drawing.

## Prerequisites

* **Python 3.10+** (for `aurc`; no third-party packages needed).
* **devkitARM** (`arm-none-eabi-gcc` + `arm-none-eabi-objcopy`) on your `PATH` —
  the standard 3DS-homebrew cross-compiler. Get it from
  <https://devkitpro.org/wiki/Getting_Started>. `aurc` detects it and prints an
  install pointer if it is missing.

Check your setup:

```
python -m compiler.aurc version
```

## Quickstart

From the `auric-lang/` directory:

```
# Compile the example to an AUR1 container:
python -m compiler.aurc build examples/hello.aur -o dist/hello.bin

# Inspect the container header:
python tools/aur_pack.py info dist/hello.bin

# Just see the generated C (no gcc needed):
python -m compiler.aurc emit-c examples/hello.aur
```

Useful `build` flags: `--verbose` (echo toolchain commands), `--keep` (keep the
intermediate `.c`/`.o`/`.elf`), `--magic AOS1` (see below), `--load-addr`.

## Running it on a 3DS

The `AUR1` header has the **identical 36-byte layout** as AuroraOS's `AOS1`
header (`include/loader.h`); only the 4-byte magic differs, and the ARM11 fields
are zero (Auric apps are ARM9-only).

There are two ways to run an app, both now supported by AuroraOS:

**1. From the Home Menu (recommended).** The updated AuroraOS Home Menu scans
`SD:\Aurora\Apps` for app containers, lists them alphabetically, and launches
the selected one (see [`../docs/apps.md`](../docs/apps.md)):

```
python -m compiler.aurc build examples/hello.aur -o HELLO.BIN
# copy HELLO.BIN to SD:\Aurora\Apps\ , boot Aurora, pick it on the home grid
```

**2. As `AURORAOS.BIN` (the boot payload).** The loader's shared parser now
accepts both `AOS1` and `AUR1` magic, so a default `AUR1` build renamed to
`AURORAOS.BIN` boots directly:

```
python -m compiler.aurc build examples/hello.aur -o AURORAOS.BIN
# copy to the SD root, boot Aurora.firm, pick "Boot Aurora"
```

> **Historical note.** Before Part 3, the stock loader checked only for `"AOS1"`
> magic. If you are running an *older* AuroraOS build, use `--magic AOS1` — the
> `AUR1` and `AOS1` files are byte-identical apart from those four magic bytes.
>
> **Press HOME to return** to the Aurora home menu at any time — the runtime
> polls the HOME button inside every built-in, and the Home Menu restores itself
> when you press it. (This works for apps launched from the home menu; an app
> booted directly as `AURORAOS.BIN` has no menu to return to, so it ignores HOME.)

## Standalone Windows executable (`aurc.exe`)

You can package `aurc` as a single Windows `.exe` so end users don't need Python
installed. The exe bundles the compiler, the runtime shim, the packer, and a
snapshot of the AuroraOS headers + `screen.c` it compiles against, so it is
self-contained — it does **not** need an AuroraOS checkout.

**Build it** (needs `pip install pyinstaller`), from `auric-lang/`:

```
python packaging/build_exe.py
# -> auric-lang/dist/aurc.exe   (a single self-contained executable)
```

**Use it** — the exe takes the same commands as `python -m compiler.aurc`:

```
aurc.exe version                        # prints version + toolchain status
aurc.exe build hello.aur -o hello.bin   # compile to an AUR1 container
aurc.exe build hello.aur -o AURORAOS.BIN --magic AOS1   # drop-in boot test
```

> **Hard dependency:** `arm-none-eabi-gcc` (devkitARM) must still be on `PATH`
> at *run* time — an ARM cross-compiler cannot be bundled into a Windows exe.
> If it is missing, `aurc.exe` fails with a clear message pointing at
> <https://devkitpro.org/wiki/Getting_Started>. Everything else (the Python
> runtime, the runtime shim, the AuroraOS sources it needs) is inside the exe.

The default devkitARM install puts the compiler at
`C:\devkitPro\devkitARM\bin`; add that to your `PATH` (the devkitPro installer
can do this for you).

## Project layout

```
auric-lang/
  compiler/    lexer, parser, AST, type checker, C codegen, and the aurc driver
  runtime/     C shim (built-ins → AuroraOS API), crt0 (auric_start.s), linker script
  tools/       aur_pack.py — AUR1 container packer/inspector (forked from aos_pack.py)
  examples/    hello.aur, demo.aur
  docs/        language.md — the full v0.1 language reference
  tests/       unittest suite for every compiler stage + end-to-end build
  packaging/   PyInstaller spec + build_exe.py for the standalone aurc.exe
```

## Language

See [`docs/language.md`](docs/language.md) for the complete reference. In brief:
`fn` functions, `let` bindings, `if`/`else`, `while`, the types `int` / `float`
/ `bool` / `string`, the usual arithmetic/comparison/logical operators, and the
four built-ins above.

## Tests

Zero-dependency, standard-library only:

```
python tests/run_tests.py
```

This covers the lexer, parser, type checker, and code generator, plus an
end-to-end compile of the examples (that part is skipped automatically if
`arm-none-eabi-gcc` is not on `PATH`).

## Status

* **Part 1 — language + compiler:** done. `hello.aur` compiles to a bootable
  container; the compiler stages are tested.
* **Part 2 — standalone Windows `aurc.exe`:** done. `python packaging/build_exe.py`
  produces a self-contained `dist/aurc.exe` (runtime toolchain still required).
* **Part 3 — AuroraOS app loader** (`SD:\Aurora\Apps`): done. The Home Menu
  scans, sorts, shows each app's **own icon**, and launches `AUR1`/`AOS1` apps via
  a shared container parser; **HOME returns** to the menu. See
  [`../docs/apps.md`](../docs/apps.md).
