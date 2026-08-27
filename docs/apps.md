# AuroraOS app loader

The AuroraOS Home Menu (`src/os/os_main.c`) discovers and launches app
containers from the SD card, in addition to the built-in Power Off tile.

## Where apps live

```
SD:\Aurora\Apps\*.bin
```

* **One container per app**, `AUR1` (an [Auric](../auric-lang/README.md) app) or
  `AOS1` — the two share an identical header layout and both are accepted.
* **Display name** = the file name with its `.BIN` extension removed. For
  example `SD:\Aurora\Apps\SNAKE.BIN` shows as **SNAKE**. (FatFs is built without
  long file names, so names are 8.3 and upper-case.)
* Apps are **sorted alphabetically** before being placed in the home grid.
* Each app carries **its own icon**, embedded in the binary; the Home Menu reads
  and displays it (see *Per-app icons* below). Apps without one get a generic
  icon.
* The scan fills the grid slots first, then appends a permanent **Power Off**
  tile; remaining slots stay empty. If there is no SD card or no `Aurora\Apps`
  folder, the menu simply shows Power Off.

Building an app and putting it in place:

```
# from auric-lang/
python -m compiler.aurc build examples/hello.aur -o HELLO.BIN
# then copy HELLO.BIN to SD:\Aurora\Apps\ on the card
```

## How launching works

The running Home Menu lives at `0x22000000`, which is also where apps load, so
it cannot copy an app over itself while it is executing there. The launch path
(all in `os_main.c` + `src/os/os_launch.s`):

1. Mounts the SD card and opens the selected container.
2. Uses the **shared parser** `aurora_parse_header()` / `aurora_load_arm9()`
   (`src/container.c`) to validate the `AOS1`/`AUR1` magic and read the ARM9
   payload into a **staging buffer** at `0x24000000`.
3. Relocates a small, position-independent copy-and-jump **stub**
   (`os_launch_stub`) to `0x25000000` — clear of both the staging buffer and the
   load region — and flushes caches so it is fetchable.
4. Jumps into the relocated stub, which copies the payload
   `0x24000000 → 0x22000000`, cleans/invalidates the caches, and branches to the
   app's entry point.

The same `container.c` helpers are used by the launcher firm's `boot_aurora()`
(`src/loader.c`), so there is exactly one copy of the header/magic logic.

## Per-app icons

Every Auric app payload begins with a fixed header the Home Menu reads:

```
payload offset 0 : b _start            (branch over the icon into the crt0)
payload offset 4 : "AURICON1"          (8-byte magic)
payload offset 12: 32x32 1bpp icon     (128 bytes, ICON_SIZE * ICON_ROW_BYTES)
```

Because the entry address is the payload start, execution begins at the branch
and skips the icon block. During the scan, `read_app_icon()` (`os_main.c`) reads
the magic and, if present, the 128 icon bytes straight into the app's grid slot;
the existing `draw_icon_32` renders it. Apps whose magic is absent fall back to a
generic icon. Icons are authored as a simple 32x32 text bitmap and embedded by
`aurc --icon` (see the Auric docs and `sample/`).

## Returning to the home menu (HOME button)

Pressing **HOME** in an app returns to the Home Menu instantly. Because an app
loads at `0x22000000` and overwrites the running OS, the OS makes return possible
before it hands off:

1. Before launching, `os_install_return()` **snapshots the OS image**
   (`[_os_start .. _os_image_end)`) to `0x26000000`, records its size and a
   "ready" magic in a descriptor, and relocates a **return stub** to a fixed
   address (`AURORA_RETURN_STUB_ADDR`).
2. The Auric runtime polls the HOME button — via the MCU over I2C, since HOME is
   not on the HID pad — inside every built-in call. On a press it branches to the
   return stub.
3. The return stub restores the OS image from the snapshot, flushes caches, and
   jumps to the OS entry; the Home Menu restarts fresh (it re-scans apps).

The descriptor's "ready" magic gates this: an app **booted directly** as
`AURORAOS.BIN` (via the firm, with no OS behind it) finds no valid descriptor and
ignores HOME, so nothing branches into an uninstalled stub.

> HOME is polled during built-in calls (`delay`, `wait_key`, `print`, `clear`,
> `fill_rect`), so keep a `delay(...)` in any long-running loop — as the sample
> app does — and HOME stays responsive.

## The existing boot flow is unchanged

`Aurora.firm → "Boot Aurora" → loads AURORAOS.BIN → jumps` works exactly as
before. The only loader change is that `boot_aurora()` now calls the shared
`container.c` helpers and accepts an `AUR1` magic in addition to `AOS1`; an
`AOS1` `AURORAOS.BIN` still boots identically.

## Files touched (Part 3)

| File | Change |
|------|--------|
| `src/container.c`, `include/container.h` | **new** — shared `AOS1`/`AUR1` header parse + ARM9 load |
| `src/loader.c` | `boot_aurora()` refactored to use the shared parser |
| `src/os/os_main.c` | scan `SD:\Aurora\Apps`, sort, per-app icons, launch + HOME-return install |
| `src/os/os_launch.s` | **new** — relocatable app hand-off stub, return stub, cache sync |
| `src/os/os.ld` | `_os_image_end` symbol for the return snapshot |
| `include/loader.h` | app staging / return-contract / icon constants |
| `src/i2c.c`, `include/i2c.h` | added `I2C_readRegBuf` to poll the MCU for the HOME button |
| `Makefile` | OS payload now links the SD/FatFs stack, `container.c`, and the stubs |
