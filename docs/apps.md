# AuroraOS app loader

The AuroraOS Home Menu (`src/os/os_main.c`) discovers and launches app
containers from the SD card, in addition to the built-in Power Off tile.

## Where apps live

```
SD:\Aurora\Apps\*.bin
```

* **One container per app**, `AUR1` (an [Auric](../auric-lang/README.md) app) or
  `AOS1`, the two share an identical header layout and both are accepted.
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

## Sound and other data files

An app's own files go in a folder beside it, named after it:

```
SD:\Aurora\Apps\TETRIS.BIN
SD:\Aurora\Apps\TETRIS\MOVE.WAV
SD:\Aurora\Apps\TETRIS\MUSIC.WAV
```

The Home Menu skips folders when it scans for apps, so they never become tiles.
An Auric app reads them with `load_sound()` (see the
[language reference](../auric-lang/docs/language.md)); the runtime links FatFs,
the SD driver and the WAV reader for it, and `--gc-sections` drops them again
from an app that loads nothing.

### Tetris

`Games/Tetris_Source/Tetris.aur` plays nine sounds from `SD:\Aurora\Apps\TETRIS\`.
They were made from the Game Boy Tetris sound effects and a background track in
`exclude/music/tetris-sounds`. **Those recordings are copyright material (the
effects are Nintendo's), so they are not in this repository or in
`TETRIS.BIN`.** Like the Wi-Fi firmware they stay in the gitignored `exclude/`
folder and go onto the card by hand; the licence is left as it is. Without the
folder the game runs silently.

| Card file | Source | Played when |
|-----------|--------|-------------|
| `MOVE.WAV` | (18) move_piece | a piece moves sideways |
| `ROTATE.WAV` | (19) rotate_piece | a piece turns |
| `LAND.WAV` | (27) piece_landed | a piece locks without clearing a line |
| `LINE.WAV` | (21) line_clear | one to three lines clear |
| `TETRIS.WAV` | (22) tetris_4_lines | four lines clear |
| `LEVELUP.WAV` | (23) level_up_jingle (V1.1) | the level goes up |
| `GAMEOVER.WAV` | (25) game_over | the stack reaches the top |
| `START.WAV` | (17) menu_sound | a game starts |
| `MUSIC.WAV` | tetris-bg-music | loops while playing |

Not used: (20) 2_player_sending_blocks, (23) unused_level_up_jingle (V1.0),
(24) sample_from_tetris_4_lines, (26) piece_falling_after_line_clear (Aurora's
Tetris has no clearing animation to play it over) and (28) the rocket ending.

The card copies were made with `auric-lang/tools/sound_prep.py`, which writes
mono 16-bit WAVs at 22,050 Hz under 8.3 names. The effects were raised 6 dB so
they carry over the music; the music comes out at 3.7 MB against the original's
16 MB, so it loads four times faster.

```
S=exclude/music/tetris-sounds
O=exclude/sd/Aurora/Apps/TETRIS
python auric-lang/tools/sound_prep.py -o $O --gain 6 \
  "$S/Tetris (GB) (18)-move_piece.wav=MOVE.WAV" \
  "$S/Tetris (GB) (19)-rotate_piece.wav=ROTATE.WAV" \
  "$S/Tetris (GB) (27)-piece_landed.wav=LAND.WAV" \
  "$S/Tetris (GB) (21)-line_clear.wav=LINE.WAV" \
  "$S/Tetris (GB) (22)-tetris_4_lines.wav=TETRIS.WAV" \
  "$S/Tetris (GB) (23)-level_up_jingle (V1.1).wav=LEVELUP.WAV" \
  "$S/Tetris (GB) (25)-game_over.wav=GAMEOVER.WAV" \
  "$S/Tetris (GB) (17)-menu_sound.wav=START.WAV"
python auric-lang/tools/sound_prep.py -o $O "$S/tetris-bg-music.wav=MUSIC.WAV"
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
   (`os_launch_stub`) to `0x25000000`, clear of both the staging buffer and the
   load region, and flushes caches so it is fetchable.
4. Jumps into the relocated stub, which copies the payload
   `0x24000000 -> 0x22000000`, cleans/invalidates the caches, and branches to the
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
generic icon. Icons are authored as a simple 32x32 text bitmap (32 lines, `#`
means on) and embedded by `aurc --icon`; `Games/Tetris_Source/Tetris.icon` is a
worked example.

## Returning to the home menu (HOME button)

Pressing **HOME** in an app returns to the Home Menu instantly. Because an app
loads at `0x22000000` and overwrites the running OS, the OS makes return possible
before it hands off:

1. Before launching, `os_install_return()` **snapshots the OS image**
   (`[_os_start .. _os_image_end)`) to `0x26000000`, records its size and a
   "ready" magic in a descriptor, and relocates a **return stub** to a fixed
   address (`AURORA_RETURN_STUB_ADDR`).
2. The Auric runtime polls the HOME button inside every built-in call, reading
   it from the MCU over I2C, since HOME is not on the HID pad. On a press it
   branches to the return stub.
3. The return stub restores the OS image from the snapshot, flushes caches, and
   jumps to the OS entry; the Home Menu restarts fresh (it re-scans apps).

The descriptor's "ready" magic gates this: an app **booted directly** as
`AURORAOS.BIN` (via the firm, with no OS behind it) finds no valid descriptor and
ignores HOME, so nothing branches into an uninstalled stub.

> HOME is polled from every built-in call, so any loop that draws or reads input
> keeps it responsive. Because reading the MCU costs a full I2C transaction and a
> game can call built-ins hundreds of times per frame, the poll is sampled every
> 64 calls rather than on each one; blocking calls such as `wait_key` poll on
> every iteration instead. A loop that calls no built-in at all will not respond
> to HOME.

## The existing boot flow is unchanged

`Aurora.firm -> "Boot Aurora" -> loads AURORAOS.BIN -> jumps` works exactly as
before. The only loader change is that `boot_aurora()` now calls the shared
`container.c` helpers and accepts an `AUR1` magic in addition to `AOS1`; an
`AOS1` `AURORAOS.BIN` still boots identically.

## Where the code lives

| File | Responsibility |
|------|----------------|
| `src/container.c`, `include/container.h` | shared `AOS1`/`AUR1` header parse + ARM9 load |
| `src/loader.c` | `boot_aurora()`, built on the shared parser |
| `src/os/os_main.c` | scan `SD:\Aurora\Apps`, sort, per-app icons, launch + HOME-return install |
| `src/os/os_launch.s` | relocatable app hand-off stub, return stub, cache sync |
| `src/os/os.ld` | `_os_image_end` symbol for the return snapshot |
| `include/loader.h` | app staging / return-contract / icon constants |
| `src/i2c.c`, `include/i2c.h` | `I2C_readRegBuf`, used to poll the MCU for the HOME button |
| `auric-lang/runtime/auric_runtime.c` | the app side: HOME return, GPU present, sound from the SD card |

## A worked example

`Games/` holds a complete app: `Tetris.BIN` with its source and icon in
`Games/Tetris_Source/`. It is built with

```
cd auric-lang
python -m compiler.aurc build ../Games/Tetris_Source/Tetris.aur \
    -o ../Games/Tetris.BIN --icon ../Games/Tetris_Source/Tetris.icon
```

and runs once copied to `SD:\Aurora\Apps\TETRIS.BIN`.
