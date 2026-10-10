# AuroraOS app loader

The AuroraOS Home Menu (`src/os/os_main.c`, `src/os/HomeMenu.c`) discovers
and launches app containers from the SD card, alongside the built-in Music,
Files, 3D Model and aShop, and lets them be arranged in pages and folders
([`home.md`](home.md)). The
File Explorer and the [terminal](terminal.md) (`./Tetris.bin`, or `Tetris` for
an app in `Aurora\Apps` or `Aurora\Apps\C`) start apps the same way.

## Where apps live

```
SD:\Aurora\Apps\*.bin       Auric apps, aShop downloads
SD:\Aurora\Apps\C\*.bin     C apps, aShop downloads of C apps
SD:\Aurora\Apps\C\<Name>\   each C app's data, made by Aurora
```

* **One container per app**: `AUR1` (an [Auric](../auric-lang/README.md) app),
  `AURC` (a [C app](../sdk/README.md)) or `AOS1`. All three share one header
  layout and only the magic differs; `aurora_magic_kind()` in
  `src/container.c` tells them apart for the loader, the Home Menu, Files and
  the Terminal.
* **Two folders.** C apps go in `Aurora\Apps\C`; the Home Menu lists both
  folders together and the Terminal looks in both, `Aurora\Apps` first. An
  app runs from either, and a C app is known by its `AURC` magic wherever it
  is: the Home Menu shows "C SDK" under its name (other SD apps have "SD
  App"), as aShop does. In
  `HomeMenu.txt` an app in the `C` folder is named `C/Name.bin`.
* **Display name** = the file name with its `.bin` extension removed. For
  example `SD:\Aurora\Apps\Snake.bin` shows as **Snake**. Long names work, and
  the extension matches in any letter case; a name too long for the Home Menu's
  card is cut with "...".
* Up to 160 apps are read, **sorted alphabetically**. Where each one sits,
  and in which folder, is kept in `SD:\Aurora\HomeMenu.txt`; an app it does
  not mention yet goes at the end of Home ([`home.md`](home.md)).
* Each app carries **its own icon**, embedded in the binary; the Home Menu reads
  and displays it (see *Per-app icons* below). Apps without one get a generic
  icon.
* The built-in Music, Files, 3D Model and aShop are arranged the same way. If
  there is no SD card or neither folder, the menu shows just those.
* **aShop** ([`store.md`](store.md)) installs apps here too, C apps in
  `Aurora\Apps\C`, and the Home Menu shows a new one as soon as aShop closes,
  without a restart.

Building an app and putting it in place:

```
# Auric, from auric-lang/
python -m compiler.aurc build examples/hello.aur -o HELLO.BIN
# then copy HELLO.BIN to SD:\Aurora\Apps\ on the card

# C, from the repository root
py sdk/aurcc.py build sdk/examples/hello -o Hello.bin
# then copy Hello.bin to SD:\Aurora\Apps\C\ on the card
```

## Apps in C

Apps can be written in C with the SDK in [`sdk/`](../sdk/README.md): one
header, `sdk/include/aurora_app.h`, for drawing on both screens (shapes, the
OS's Figtree fonts, PNG/JPEG/BMP images with alpha), buttons, touch and the
circle pad, sound, files and system information, plus devkitARM's newlib for
the C library (`printf` to an on-screen console, `malloc`, `fopen` on the SD
card, `time`). `sdk/aurcc.py` compiles them with the OS's ARM9 flags, links
the SDK runtime (`sdk/runtime/`) with the OS's own drivers from `src/`, and
packs the result as `AURC`, so the Home Menu, Files and the Terminal start a C
app like an Auric one, and know it is a C app. C apps go in
`SD:\Aurora\Apps\C`. They are published on aShop like Auric apps
(`store.md`, *Apps in C*).

**A C app's data** goes in `SD:\Aurora\Apps\C\<Name>\`, where `<Name>` is the
app's file name without `.bin` (`SD:\Aurora\Apps\C\Bricks\` for `Bricks.bin`),
wherever the `.bin` itself is. Before it starts an `AURC` app, the OS makes
that folder (and `Aurora\Apps\C` above it) if it is missing, so the app can
save there from the start and find its files next time; the SDK's
`app_dir()` names the same folder. Both use `aurora_c_data_dir()` in
`src/container.c`. An app built with `--magic AUR1` (for an older OS) gets no
folder made and calls `fs_mkdir(app_dir())` itself.

Examples: `sdk/examples/hello`, `demo` (shapes, fonts, painting, saving a
BMP), `bricks` (a breakout game that keeps its high score) and `net` (the
network).

A C app has run on a New 3DS: the CPU stress test (`cpu-stress/`, 2026-10-08)
drew both screens, read the buttons and the battery, and wrote its results
file. The examples have not been tried on a console yet.

What was checked on a PC (2026-10-08): the examples and the
runtime build without warnings; the runtime, run on the PC against stubbed
hardware, a RAM disk formatted by the real FatFs and scripted input, passes
150 checks (paths, the file calls behind stdio, renames, truncation, the
working directory, descriptor limits, the app's path and data folder for each
way of starting it, the data folder rule and making it, the magic kinds, the
clock with the user's offset, USER.dat settings, PNG/JPEG/BMP decoding with
alpha, WAV loading, nothing drawn outside the screens); the demo, Bricks and
the console (wrapping, scrolling, ANSI colours) were run frame by frame and
the frames looked at, with the demo's BMP save and Bricks' high score going
through the SDK's file layer.

## The network

Auric and C apps can use the network over the Wi-Fi network saved in
Settings > Wi-Fi: join it, look up names, and make plain HTTP requests (no
HTTPS yet) and downloads. C apps call `net_connect()`, `http_get()` and the
rest (`sdk/README.md`, *The network*); Auric has `net_connect()`,
`http_get()`, `http_post()`, `http_download()` and `json_string()` and its
kin (`auric-lang/docs/language.md`, *The network*). Both are built on
`src/os/AppNet.c`, which posts the ARM11 core's Wi-Fi commands itself, since
the app has replaced the OS (docs/wifi.md, *Apps on the network*). Like
sound, it needs an app started from the Home Menu, Files or the Terminal.
Checked on a PC against a simulated core (2026-10-08); Auric's
`examples/net.aur` then worked on a New 3DS the same day. The SDK's `net`
example has not been tried on a console yet.

## Sound and other data files

An Auric app's own files go in a folder beside it, named after it (a C app's
go in `SD:\Aurora\Apps\C\<Name>\`, above):

```
SD:\Aurora\Apps\TETRIS.BIN
SD:\Aurora\Apps\TETRIS\MOVE.WAV
SD:\Aurora\Apps\TETRIS\MUSIC.WAV
```

The Home Menu skips folders when it scans for apps, so they never become tiles.
An Auric app reads them with `load_sound()` (see the
[language reference](../auric-lang/docs/language.md)); the runtime links FatFs,
the SD driver and the WAV reader for it, and `--gc-sections` drops them again
from an app that loads nothing. A C app finds its folder with `app_dir()`,
which follows the app's file name, from the launch block below.

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
is `os_launch_app()` in `os_main.c`, with `src/os/os_launch.s`; the File
Explorer and the terminal are handed the same function:

1. Mounts the SD card and opens the selected container.
2. Uses the **shared parser** `aurora_parse_header()` / `aurora_load_arm9()`
   (`src/container.c`) to validate the `AOS1`/`AUR1`/`AURC` magic and read
   the ARM9 payload into a **staging buffer** at `0x24000000`. For an `AURC`
   app it then makes the app's data folder, `Aurora/Apps/C/<Name>`
   (`aurora_c_data_make()`).
3. Relocates a small, position-independent copy-and-jump **stub**
   (`os_launch_stub`) to `0x25000000`, clear of both the staging buffer and the
   load region, and flushes caches so it is fetchable.
4. Writes the launch block (below), then jumps into the relocated stub, which
   copies the payload `0x24000000 -> 0x22000000`, cleans/invalidates the
   caches, and branches to the app's entry point.

### The launch block

Just before an app starts, `os_launch_info()` writes the path it was started
from at `AURORA_LAUNCH_INFO_ADDR` (`0x25008100`, `include/loader.h`): the
magic `AURORA_LAUNCH_MAGIC` ("ALN1") as a word, then the path as a
NUL-terminated string of at most `AURORA_LAUNCH_PATH_MAX` (256) bytes, as the
Home Menu (`Aurora/Apps/C/X.bin`) or Files and the Terminal (`0:/...`) give it.
A path too long is left out (the magic stays 0). The C SDK's `app_path()` and
`app_dir()` read it once and clear the magic. There is no block for an app
booted directly by the launcher, or for a `--magic AUR1` build run by an older
OS (which lists only `Aurora\Apps`); there the SDK takes
`/Aurora/Apps/<name>.bin` if that file exists and `/Aurora/Apps/C/<name>.bin`
otherwise, with the name `aurcc.py` stored from the output file.

The same `container.c` helpers are used by the launcher firm's `boot_aurora()`
(`src/loader.c`), so there is exactly one copy of the header/magic logic.

## Per-app icons

Every Auric or C app payload begins with a fixed header the Home Menu reads:

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
means on) and embedded by `aurc --icon` or `aurcc.py --icon` (which also takes
a 32x32 PNG); `Games/Tetris_Source/Tetris.icon` is a worked example.

## Returning to the home menu (HOME button)

Pressing **HOME** in an app returns to the Home Menu instantly. Because an app
loads at `0x22000000` and overwrites the running OS, the OS makes return possible
before it hands off:

1. Before launching, `os_install_return()` **snapshots the OS image**
   (`[_os_start .. _os_image_end)`) to `0x26000000`, records its size and a
   "ready" magic in a descriptor, and relocates a **return stub** to a fixed
   address (`AURORA_RETURN_STUB_ADDR`).
2. The app's runtime polls the HOME button, reading it from the MCU over I2C,
   since HOME is not on the HID pad: Auric's inside every built-in call, the C
   SDK's once a frame in `app_loop()`/`hid_scan()` (and while `hid_wait()` and
   `sys_sleep()` wait). On a press it branches to the return stub; the C SDK
   first runs `exit()`, so `atexit()` functions run and stdio files are
   closed, and puts its newest frame back in framebuffer A.
3. The return stub restores the OS image from the snapshot, flushes caches, and
   jumps to the OS entry; the Home Menu restarts fresh (it re-scans apps).

The descriptor's "ready" magic gates this: an app **booted directly** as
`AURORAOS.BIN` (via the firm, with no OS behind it) finds no valid descriptor and
ignores HOME, so nothing branches into an uninstalled stub.

> In Auric, HOME is polled from every built-in call, so any loop that draws or
> reads input keeps it responsive. Because reading the MCU costs a full I2C
> transaction and a game can call built-ins hundreds of times per frame, the
> poll is sampled every 64 calls rather than on each one; blocking calls such
> as `wait_key` poll on every iteration instead. A loop that calls no built-in
> at all will not respond to HOME.

### After a crash

The OS's ARM9 exception vectors point into the OS image, which an app
replaces, so a C app installs the crash handler again at start (it links
`src/os/crash.c`). In an app the crash screen's countdown line reads "HOME:
back.  Power off in 10s...": `crash.c` polls the hook `g_crash_poll`, which
the SDK sets, and HOME then goes back to the Home Menu through the return stub
(after switching back to the CPU mode the app started in). The OS never sets
the hook, so its own crash screen is unchanged. Auric apps do not install a
crash handler.

## The existing boot flow is unchanged

`Aurora.firm -> "Boot Aurora" -> loads AURORAOS.BIN -> jumps` works exactly as
before. The only loader change is that `boot_aurora()` now calls the shared
`container.c` helpers and accepts `AUR1` and `AURC` magics in addition to
`AOS1`; an `AOS1` `AURORAOS.BIN` still boots identically.

## Where the code lives

| File | Responsibility |
|------|----------------|
| `src/container.c`, `include/container.h` | shared header parse + ARM9 load, `aurora_magic_kind()` (`AOS1`/`AUR1`/`AURC`), a C app's data folder (`aurora_c_data_dir()`, `aurora_c_data_make()`) |
| `src/loader.c` | `boot_aurora()`, built on the shared parser |
| `src/os/os_main.c` | scan `SD:\Aurora\Apps` and `SD:\Aurora\Apps\C`, sort, per-app icons, launch block, launch + HOME-return install |
| `src/os/os_launch.s` | relocatable app hand-off stub, return stub, cache sync |
| `src/os/os.ld` | `_os_image_end` symbol for the return snapshot |
| `include/loader.h` | app staging / return-contract / icon constants |
| `src/i2c.c`, `include/i2c.h` | `I2C_readRegBuf`, used to poll the MCU for the HOME button |
| `auric-lang/runtime/auric_runtime.c` | the app side: HOME return, GPU present of both screens, touch, sound from the SD card |
| `auric-lang/runtime/auric_net.c` | Auric's network built-ins: requests, parameters, the reply's text and lines, JSON paths |
| `src/os/AppNet.c`, `include/appnet.h` | the network for apps (C and Auric): join, DNS, ping, TCP, HTTP, downloads |
| `sdk/` | apps in C: `include/aurora_app.h`, the runtime in `runtime/`, the build tool `aurcc.py`, examples; see [`sdk/README.md`](../sdk/README.md) |

## A worked example

`Games/` holds a complete app: `Tetris.BIN` with its source and icon in
`Games/Tetris_Source/`. It is built with

```
cd auric-lang
python -m compiler.aurc build ../Games/Tetris_Source/Tetris.aur \
    -o ../Games/Tetris.BIN --icon ../Games/Tetris_Source/Tetris.icon
```

and runs once copied to `SD:\Aurora\Apps\TETRIS.BIN`.
