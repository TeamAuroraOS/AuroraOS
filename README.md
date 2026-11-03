# Aurora

  

[![Discord](https://img.shields.io/badge/Discord-Join%20us-5865F2?logo=discord&logoColor=white)](https://discord.gg/T25cytDaAK)

[![YouTube](https://img.shields.io/badge/YouTube-red?logo=youtube&logoColor=white)](https://www.youtube.com/@Aurora3DS)

![Platform](https://img.shields.io/badge/platform-Nintendo%203DS-red?logo=nintendo3ds&logoColor=white)

![Status](https://img.shields.io/badge/status-in%20development-yellow)

  

Aurora is a custom OS for the Nintendo 3DS. **Current version: Beta 7 v0.1.6.**
-  *Built using knowledge from GodMode9 and Luma source code.*

## What works

| Area | State | Notes |
|------|-------|-------|
| Home Menu + Settings | working | app grid, accent colors, three languages, About page with More Info (eMMC CID) and credits (the card's free space is counted while the page stays responsive); Settings > Reset deletes `USER.dat` so the setup runs again |
| Wallpaper themes | new | Settings > Theme lists the folders in `SD:/Aurora/themes` that hold a 400x240 `top.png` and a 320x240 `bottom.png`, and puts them on both screens instead of the built-in wallpaper; the choice is kept in `SD:/Aurora/theme.txt`; see [`docs/home.md`](docs/home.md) |
| Deleting apps | new | **Y** on an SD app > Delete App removes its `.bin` after asking, then offers to delete its data folder as well; see [`docs/home.md`](docs/home.md) |
| Home Menu pages and folders | working | up to 12 pages of 15; folders two levels deep; moving apps by dragging or with the D-pad; Power in the bar; saved in `SD:/Aurora/HomeMenu.txt`; see [`docs/home.md`](docs/home.md) |
| System menu | new | **L + Down + Select** on any screen and inside apps: Return to Home, Power Off, Cancel ("Aurore Tools"). In apps the ARM11 core draws it; Return to Home works in apps built with the Beta 7 runtimes (C SDK 1.2 or later, Auric), and an older app says to press HOME instead; see [`docs/home.md`](docs/home.md) |
| Icons, wallpaper, type | working | real art + Figtree from SD, with accents (the built-in 8x8 font draws accented letters too); a message at boot when `assets.pak` is missing or does not match; see [`docs/assets.md`](docs/assets.md) |
| File Explorer | working | browse the card, per-type icons, TXT/LOG viewer, hex editor; see [`docs/files.md`](docs/files.md) |
| File operations | working | copy, move, rename, delete and new folder, with long file names |
| Screenshots | working | L+R saves both screens as a BMP; see [`docs/input.md`](docs/input.md) |
| aShop (store) | working | the apps and games published on the Aurora Network: sections, news, app pages, search, and downloads into `SD:/Aurora/Apps` (apps in C into `SD:/Aurora/Apps/C`, marked "C SDK" under their name), checked by SHA-256. Needs a linked Aurora account (it offers to link one) and keeps the catalogue on the card for offline browsing. Apps are published at account.aurora3ds.xyz/developer. Works on a New 3DS (2026-10-05); see [`docs/store.md`](docs/store.md) |
| Aurora Account | working | Settings > Aurora Account (and the setup wizard's Account step) links the console to an account on account.aurora3ds.xyz: the console shows a code and a QR code, the person approves it on a phone or computer, and the token is kept in `SD:/Aurora/account.txt`. Create an account sends the person to sign up first. Works on a New 3DS (2026-10-04); see [`docs/account.md`](docs/account.md) |
| Terminal | working | **X** on the Home Menu: a Linux-style shell with a touch keyboard, file commands, `ping` and `curl` (new: HTTP and HTTPS) over Wi-Fi, `systemctl`, `info` and app launching; see [`docs/terminal.md`](docs/terminal.md) |
| Images | working | BMP, PNG and baseline JPEG, scaled to fit |
| WAV playback | working | 8/16-bit PCM, mono or stereo (MP3 not yet) |
| Display | working | GPU-composited; see [`docs/gpu.md`](docs/gpu.md) |
| Animations | working | screens slide over each other with parallax, dialogs pop up, the selection springs, tiles lift and lists scroll; each screen is triple buffered and switches frames at the vertical blank, without tearing; see [`docs/ui.md`](docs/ui.md) |
| GPU (PICA200) | working | PSC fill + PPF blit, verified on hardware |
| Audio | working | ARM11 CSND core, eight voices for apps; see [`docs/audio.md`](docs/audio.md) |
| System sounds | new | a startup sound after a cold boot, select, back and move clicks in the Home Menu, Settings and aShop, a looping song on the Home Menu that starts after the startup sound and stops when anything opens or the lid closes, and aShop's own looping music with a chime when an install finishes, from `assets.pak` (mono, 4-bit DSP-ADPCM: the 2.9 MB aShop song is 319 KB); Settings > Startup sound, Home music, aShop music and Menu sounds turn them off; reported working on a console (2026-11-01); the Home Menu's song is by LeweckBanana; see [`docs/audio.md`](docs/audio.md) |
| Touchscreen | working | CTR codec on the ARM11, with calibration in Settings |
| Circle pad | working | read from the same codec by the ARM11 core; also moves the selection in every menu, like the D-pad; see [`docs/input.md`](docs/input.md) |
| 3D models (GLB) | working on a New 3DS | **3D Model** on the Home Menu shows `SD:/Aurora/model.glb` with its textures: circle pad orbits, D-pad pans, Y and A zoom; the nearest part sets the 3D depth and is named on the bottom screen; see [`docs/glb.md`](docs/glb.md) |
| 3D (GPU) | working on a New 3DS | the PICA200's 3D pipeline from command lists, with vertex buffers and mipmapped textures; see [`docs/stereo3d.md`](docs/stereo3d.md) |
| 3D (software) | working | the ARM9 renderer, which draws the model viewer's fallback cube; see [`docs/soft3d.md`](docs/soft3d.md) |
| Stereoscopic 3D screen | working on a New 3DS | the 3D slider puts the top screen in 3D, with the parallax barrier on every 3DS model (and the New 3DS's movable mask); on an Old 3DS 3D switches on (the screen dims a little) but shows no depth yet (2026-10-23); see [`docs/stereo3d.md`](docs/stereo3d.md) |
| ARM11 core updates | working | a core from another build is swapped out without a power-off, from core 83 on; see [`docs/audio.md`](docs/audio.md) |
| Clock + battery | working | MCU over I2C, with Aurora's own clock offset in Settings > Clock (the RTC is never written); see [`docs/power.md`](docs/power.md) |
| Screen brightness | new | Settings > Brightness: five levels on both backlights, calibrated per panel (and for the top in 3D), set by the ARM11 core (core 119) and kept in `USER.dat`; see [`docs/power.md`](docs/power.md) |
| Closing the lid | new | both backlights go off and the OS (and apps built with this release's runtimes, C SDK 1.3) wait until the lid opens, with the apps' clocks stopped meanwhile; keys held as it opens are ignored. Sound keeps playing and the console stays powered: it is a pause, not the hardware's sleep mode (core 120); see [`docs/power.md`](docs/power.md) |
| Console model | working | New/Old from CFG11_SOCINFO ("New 3DS/2DS", "Old 3DS/2DS"); "N" in the status bar |
| Crash handler | working | register dump plus a three-beep error tone on a fault |
| Apps ([Auric](auric-lang/README.md)) | working | sound from the SD card; drawing on the bottom screen and touch input (Auric v0.4, works on a New 3DS, 2026-10-05); the network (Auric v0.5: HTTP and JSON, works on a New 3DS, 2026-10-08; v0.6 adds HTTPS); see [`docs/apps.md`](docs/apps.md) |
| Apps in C ([SDK](sdk/README.md)) | new | `#include <aurora_app.h>`: both screens, shapes, the Figtree fonts, PNG/JPEG/BMP images, buttons, touch, circle pad, sound, files, clock, the network (HTTP and, from SDK 1.4, HTTPS over the saved Wi-Fi network); the C library through newlib (`printf` on screen, `malloc`, `fopen` on the SD card); built by `sdk/aurcc.py` into `AURC` apps (the `AUR1` container with its own magic, so Aurora knows a C app) that go in `SD:/Aurora/Apps/C` and keep their data in `SD:/Aurora/Apps/C/<Name>`, which Aurora makes; HOME and crashes return to the Home Menu. Published on aShop like Auric apps. Checked on a PC against stubbed hardware; one C app, the CPU stress test, ran on a New 3DS (2026-10-08), the examples are untested on a console |
| CPU stress test | works on a New 3DS | `cpu-stress/`: the same benchmark as an AuroraOS C app and as Horizon homebrew, on ARM11 core 0 at 804 MHz in both (AuroraOS runs it through core 116's `AUDIO_CMD_RUN11`), with a graph and a results file; see [`cpu-stress/README.md`](cpu-stress/README.md) |
| Wi-Fi | working | Settings > Wi-Fi searches, saves a network and joins it: open and WPA2-PSK networks, the password typed on the console, an address over DHCP (New 3DS, 2026-10-03). The terminal's `ping` looks up names and pings any address. The firmware is copied from the console's own NAND the first time (read only; works on hardware). Core 114 adds a small TCP client for one HTTP exchange at a time, which Aurora accounts use (works on hardware, 2026-10-04); core 115 reads it faster, about 160 KB/s for aShop downloads (New 3DS, 2026-10-08). Auric and C apps can use the network too. A session that stops working ends by itself and the next request joins again (core 123, not yet on a console). Not yet: WPA3: [`docs/wifi.md`](docs/wifi.md) |
| HTTPS (TLS) | new | TLS 1.2 on the ARM9 (BearSSL) over a TCP connection the ARM11 core keeps open (from core 121): Aurora accounts and aShop now talk to the server over HTTPS, and C apps (SDK 1.4), Auric apps (0.6) and the terminal's `curl` can fetch `https://` addresses. Certificates are checked against 46 built-in root authorities and any added in `SD:/Aurora/certs`, with dates checked against the console's clock (never earlier than the build date). Checked on a PC against real servers, then reported working on a console (2026-11-01); see [`docs/tls.md`](docs/tls.md) |

The UI renders into a cached FCRAM backbuffer and the GPU moves each finished
screen to the panel in one blit, rather than rasterizing straight into uncached
VRAM. Icons and text are pre-rendered at the exact size they are drawn, so the
console scales nothing at runtime. Moving between screens is animated: a slide
is one GPU copy per frame, so it costs the ARM9 nothing.

## Source layout

Subsystems that span both CPUs carry the core they run on in the file name. The
ARM9 runs the OS; the ARM11 handles the hardware the ARM9 cannot reach.

| Subsystem | ARM9 | ARM11 |
|-----------|------|-------|
| Audio (codec output, CSND) | `src/os/Audio9.c` | `src/os/Audio11.c` |
| Touchscreen | `src/os/Touch9.c` | `src/os/Touch11.c` |
| Wi-Fi | `src/os/WiFi9.c`, `src/os/WiFiJoin.c` (the join, shared with apps), `src/os/AppNet.c` (the network for apps and `curl`), `src/os/WiFiTcp.c` (the TCP rings), `src/os/Crypto.c` (the PMK) | `src/os/WiFi11.c`, `src/os/Wpa11.c` (WPA2), `src/os/Net11.c` (DHCP, ARP, DNS, ping, TCP) |
| HTTPS | `src/os/Tls.c` (the TLS client, over BearSSL in `lib/bearssl`), `src/os/TlsRoots.c` (the roots, made by `tools/mkroots.py` from `assets/certs/roots.pem`) | the TCP connection in `src/os/WiFi11.c` |
| GPU | `src/os/Gpu9.c` | `src/os/Gpu11.c` |
| GPU 3D pipeline (P3D) | `src/os/P3d9.c` (command lists) | `src/os/P3d11.c` (runs them) |
| Stereoscopic top screen | `src/os/Stereo9.c` (slider, model) | `src/os/Stereo11.c` (mode, barrier) |
| Codec bus (shared by audio + touch) | | `src/os/Codec11.c` |
| Core entry and command loop | | `src/os/Core11.c` |
| System menu (L + Down + Select) | `src/os/HomeMenu.c` (`home_system_menu`, OS screens) | `src/os/SystemMenu11.c` (inside apps) |
| Screen brightness | `src/os/Audio9.c` (`audio_set_brightness`) | `src/os/Core11.c` |
| Lid closed | `include/lid.h` (the OS and the app runtimes wait) | `src/os/Core11.c` (`core11_sleep_poll`, backlights off) |
| New 3DS clock switch | `src/model.c` | `src/os/Clock11.c` |
| An app's code on the ARM11 (`AUDIO_CMD_RUN11`) | the app posts it | `src/os/Run11.c` |

The ARM11 files link into one core binary, which the ARM9 embeds and wakes;
`src/os/core11.h` carries what they share; `src/power.c` and `src/i2c.c` are
built into the core as well, for the system menu's Power Off. ARM9-only
modules keep plain names: `src/screen.c`, `src/power.c`, `src/i2c.c`,
`src/sdmmc.c`, `src/os/Timer9.c`,
`src/assets.c`, `src/ui.c` (with the cached wallpaper a theme replaces),
`src/image.c`, `src/jpeg.c`, `src/wav.c`,
`src/wavload.c`, `src/model.c`, the status bar in `src/os/StatusBar.c`, the File
Explorer in `src/os/Files.c` with its viewers in `src/os/FileView.c` and its
operations in `src/os/FileOps.c`, screenshots in `src/os/Screenshot.c`, touch
calibration in `src/os/TouchCal.c`, the terminal in `src/os/Terminal.c` with its
commands in `src/os/TermCmds.c`, the render test in `src/os/RenderTest.c`, and
the Home Menu in `src/os/HomeMenu.c` with its arrangement in
`src/os/HomeLayout.c`, the aShop store in `src/os/Store.c`, the startup sound,
menu sounds, the Home Menu's song and aShop's music in `src/os/UiSound.c`, Aurora accounts
in `src/os/Account.c` with the QR encoder in `src/os/Qr.c` and the HTTP
client both use in `src/os/Http.c`, the Wi-Fi
firmware copy in `src/os/FwDump.c` (with the read-only NAND reader in
`src/os/Nand.c` and the AES engine in `src/os/Aes.c`), the 3D Model screen in `src/os/Model3D.c` with its GLB loader in
`src/os/Glb.c` and JSON tokenizer in `src/os/Json.c`, and the software 3D
renderer in `src/os/Soft3D.c`. The model's vertex shader is
`src/os/model.v.pica`, assembled at build time by picasso (devkitPro's 3DS
tools) and converted by `tools/shbin2c.py`.

Apps have two toolchains. `auric-lang/` is the Auric language and its
compiler. `sdk/` is for apps in C: the public header `sdk/include/aurora_app.h`,
the runtime in `sdk/runtime/` (which links the OS's own drivers from `src/`
and devkitARM's newlib), the build tool `sdk/aurcc.py` and examples in
`sdk/examples/`. `cpu-stress/` is a CPU benchmark built both as an AuroraOS C
app and as Horizon homebrew, to compare the two systems.

Art, fonts and the OS's sounds are built into `Aurora/assets.pak` by
`tools/mkassets.py` (with `tools/png_read.py`, `tools/ttf.py` and
`tools/dspadpcm.py`) from `icons/`, `assets/fonts/` and `assets/audio/`, and
loaded by `src/assets.c` (art at boot, each sound when it plays). Run
`make assets` after changing any of them.

## Documentation

* [`docs/apps.md`](docs/apps.md): the app container format and loader
* [`docs/assets.md`](docs/assets.md): the SD asset pack, icons and fonts
* [`docs/files.md`](docs/files.md): the File Explorer, file operations, text viewer, hex editor, image and audio decoding
* [`docs/input.md`](docs/input.md): touch calibration, the circle pad and screenshots
* [`docs/terminal.md`](docs/terminal.md): the terminal, its keys and every command
* [`docs/gpu.md`](docs/gpu.md): PICA200 driver and the rendering path
* [`docs/home.md`](docs/home.md): the Home Menu: pages, folders, moving and deleting apps, the layout file, the system menu, wallpaper themes
* [`docs/store.md`](docs/store.md): aShop, the store: opening it, screens, downloads, the catalogue, publishing apps
* [`docs/account.md`](docs/account.md): Aurora accounts: linking and creating one, the token file, the console's HTTP and TCP client
* [`docs/glb.md`](docs/glb.md): the 3D Model screen and the GLB loader: what it reads, limits, textures, memory
* [`docs/soft3d.md`](docs/soft3d.md): the software 3D renderer, the model viewer's fallback
* [`docs/stereo3d.md`](docs/stereo3d.md): the 3D screen, the parallax barrier and the PICA200's 3D pipeline
* [`docs/ui.md`](docs/ui.md): rounded shapes, screen transitions and animation
* [`docs/audio.md`](docs/audio.md): CSND playback and the channel registers
* [`docs/power.md`](docs/power.md): MCU real-time clock, battery, power off and reboot, closing the lid, screen brightness, the About page
* [`docs/wifi.md`](docs/wifi.md): Wi-Fi bring-up, state and findings, and copying the firmware from the console
* [`docs/tls.md`](docs/tls.md): HTTPS: the TLS client, the roots it trusts, certificates on the SD card, the clock, randomness
* [`auric-lang/README.md`](auric-lang/README.md): the Auric language and compiler
* [`sdk/README.md`](sdk/README.md): writing apps in C: the API, the C library, memory, building with `aurcc.py`, publishing on aShop
* [`cpu-stress/README.md`](cpu-stress/README.md): the CPU stress test on AuroraOS and Horizon, and the results on a New 3DS

## License
Aurora is licensed **GPL-3.0** (see `LICENSE`). Two drivers are separate
**GPL-2.0** components, because GPL-2.0-only is incompatible with GPL-3.0 and
they keep the license of the code they derive from:

- **Wi-Fi driver**: see `LICENSE.wifi` and `docs/wifi.md` "License and credits".
  Derives from the ath6kl legacy driver as ported to the 3DS by **Octoblimp**.
- **PICA200 GPU driver**: see `docs/gpu.md` "License and credits". Derives from
  the Linux Nintendo 3DS PICA200 driver (`ctr_pica.c`).
`LICENSE.wifi` holds the GPL-2.0 text used by both of those components.

The New 3DS clock switch in `src/os/Clock11.c` follows the register sequence in
fastboot3DS and libn3ds (derrek and profi200, GPL-3.0, the same license as
Aurora); see `docs/gpu.md` "Render test".

The UI font is **Figtree** under the **SIL Open Font License 1.1** (Copyright
2022 The Figtree Project Authors). Its license is `assets/fonts/OFL.txt`, and
it covers both the TTFs in `assets/fonts/` and the glyph atlases rendered from
them into `Aurora/assets.pak`. The OFL is compatible with GPL-3.0 for
distribution; it is kept as-is and recorded here and in `docs/assets.md`.

HTTPS uses **BearSSL** by Thomas Pornin, under the **MIT license**
(`lib/bearssl/LICENSE.txt`), which both GPL-2.0 and GPL-3.0 code may use. The
root certificates in `assets/certs/roots.pem` (and `src/os/TlsRoots.c`, made
from them) come from the Mozilla CA store as curl publishes it, which is
distributed under the MPL-2.0; see `docs/tls.md`.

## How To Install:
- Place `Aurora.firm` in `SD:\luma\payloads`
- Place `AURORAOS.BIN` in root of SD (`SD:\`)
- Copy the built `Aurora\assets.pak` to the card as `SD:\Aurora\assets.pak`
  (optional: without it the UI falls back to its built-in icons and font)
- Optional: copy the `Aurora\Store` folder to `SD:\Aurora\Store` for aShop's
  banners and welcome art; see [`docs/store.md`](docs/store.md)
- Optional: copy apps such as `Games/Tetris.BIN` to `SD:\Aurora\Apps\`. Tetris
  plays sounds from `SD:\Aurora\Apps\TETRIS\` when they are there; see
  [`docs/apps.md`](docs/apps.md). Apps written in C are built with
  `py sdk/aurcc.py build ...` and go in `SD:\Aurora\Apps\C\`; see
  [`sdk/README.md`](sdk/README.md)
- Optional: wallpaper themes go in `SD:\Aurora\themes\<Name>\` as `top.png`
  (400x240) and `bottom.png` (320x240); choose one in Settings > Theme
- Wi-Fi needs your console's own Wi-Fi firmware. The first time you open
  Settings > Wi-Fi (or reach Network in the setup wizard), Aurora offers to
  copy it from the system NAND (read only, after a button code) to
  `SD:\Aurora\wifi\`; it can also be extracted on a
  PC with `tools/nwm_extract.py`. See [`docs/wifi.md`](docs/wifi.md). The
  saved network is in `SD:\Aurora\wifi\network.txt`
## How to Open:
- Make sure [loading custom firms](https://wiki.hacks.guide/wiki/3DS:Luma3DS/Configuration#Enable_loading_external_FIRMs_and_modules) is enabled.
- With system off, hold `START` while booting
- Select `Aurora` from the list
- Select `Boot Aurora`
## Home Menu controls:
- The D-pad picks an app and **A** opens it; a tap on a tile opens it
- **L** / **R**, or a swipe, turns the page
- **Y** opens the menu to move a tile, make, rename or remove a folder, or
  delete an app
- Hold a tile with the stylus to drag it; drop it on a folder to put it in, or
  on another app to make a folder of the two
- **B** leaves a folder
- The power icon at the top left of the touch screen turns the console off
- **START**, or the settings icon at the top right of the touch screen, opens
  Settings
- **X** opens the [terminal](docs/terminal.md)
- **L** + **R** together take a screenshot on any screen
- **L** + **Down** + **Select** opens the system menu, here and in apps
- More in [`docs/home.md`](docs/home.md)

### AI Disclaimer:
AI was used in the making of most documentation and some in-code comments. AI was used for the writing of arm assembly, Mainstream Corporate AI was not used. A local model was used on the PC of @DisLoPik.
