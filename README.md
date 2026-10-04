# Aurora

  

[![Discord](https://img.shields.io/badge/Discord-Join%20us-5865F2?logo=discord&logoColor=white)](https://discord.gg/T25cytDaAK)

[![YouTube](https://img.shields.io/badge/YouTube-red?logo=youtube&logoColor=white)](https://www.youtube.com/@Aurora3DS)

![Platform](https://img.shields.io/badge/platform-Nintendo%203DS-red?logo=nintendo3ds&logoColor=white)

![Status](https://img.shields.io/badge/status-in%20development-yellow)

  

Aurora is a custom OS for the Nintendo 3DS. **Current version: Beta v0.1.3.**
-  *Built using knowledge from GodMode9 and Luma source code.*

## What works

| Area | State | Notes |
|------|-------|-------|
| Home Menu + Settings | working | app grid, accent colours, three languages, About page with More Info (eMMC CID) and credits |
| Home Menu pages and folders | working | up to 12 pages of 15; folders two levels deep; moving apps by dragging or with the D-pad; Power in the bar; saved in `SD:/Aurora/HomeMenu.txt`; see [`docs/home.md`](docs/home.md) |
| Icons, wallpaper, type | working | real art + Figtree from SD, with accents; see [`docs/assets.md`](docs/assets.md) |
| File Explorer | working | browse the card, per-type icons, TXT/LOG viewer, hex editor; see [`docs/files.md`](docs/files.md) |
| File operations | working | copy, move, rename, delete and new folder, with long file names |
| Screenshots | working | L+R saves both screens as a BMP; see [`docs/input.md`](docs/input.md) |
| aShop (store) | demo | browse sections, apps and news, app pages, search and downloads into `SD:/Aurora/Apps`; no download server yet, so a download installs from `SD:/Aurora/Store/Packages` or only shows its progress; see [`docs/store.md`](docs/store.md) |
| Terminal | working | **X** on the Home Menu: a Linux-style shell with a touch keyboard, file commands, `ping` over Wi-Fi, `systemctl`, `info` and app launching; see [`docs/terminal.md`](docs/terminal.md) |
| Images | working | BMP, PNG and baseline JPEG, scaled to fit |
| WAV playback | working | 8/16-bit PCM, mono or stereo (MP3 not yet) |
| Display | working | GPU-composited; see [`docs/gpu.md`](docs/gpu.md) |
| Animations | working | screens slide over each other with parallax, dialogs pop up, the selection springs, tiles lift and lists scroll; each screen is triple buffered and switches frames at the vertical blank, without tearing; see [`docs/ui.md`](docs/ui.md) |
| GPU (PICA200) | working | PSC fill + PPF blit, verified on hardware |
| Audio | working | ARM11 CSND core, eight voices for apps; see [`docs/audio.md`](docs/audio.md) |
| Touchscreen | working | CTR codec on the ARM11, with calibration in Settings |
| Circle pad | working | read from the same codec by the ARM11 core; see [`docs/input.md`](docs/input.md) |
| 3D models (GLB) | working on a New 3DS | **3D Model** on the Home Menu shows `SD:/Aurora/model.glb` with its textures: circle pad orbits, D-pad pans, Y and A zoom; the nearest part sets the 3D depth and is named on the bottom screen; see [`docs/glb.md`](docs/glb.md) |
| 3D (GPU) | working on a New 3DS | the PICA200's 3D pipeline from command lists, with vertex buffers and mipmapped textures; see [`docs/stereo3d.md`](docs/stereo3d.md) |
| 3D (software) | working | the ARM9 renderer, which draws the model viewer's fallback cube; see [`docs/soft3d.md`](docs/soft3d.md) |
| Stereoscopic 3D screen | working on a New 3DS | the 3D slider puts the top screen in 3D, with the parallax barrier on every 3DS model (and the New 3DS's movable mask); Old 3DS untested; see [`docs/stereo3d.md`](docs/stereo3d.md) |
| ARM11 core updates | working | a core from another build is swapped out without a power-off, from core 83 on; see [`docs/audio.md`](docs/audio.md) |
| Clock + battery | working | MCU over I2C, with Aurora's own clock offset in Settings > Clock (the RTC is never written); see [`docs/power.md`](docs/power.md) |
| Console model | working | New/Old from CFG11_SOCINFO; "N" in the status bar |
| Crash handler | working | register dump plus a three-beep error tone on a fault |
| Apps ([Auric](auric-lang/README.md)) | working | sound from the SD card; see [`docs/apps.md`](docs/apps.md) |
| Wi-Fi | working | Settings > Wi-Fi searches, saves a network and joins it: open and WPA2-PSK networks, the password typed on the console, an address over DHCP (New 3DS, 2026-10-03). The terminal's `ping` looks up names and pings any address. The firmware is copied from the console's own NAND the first time (read only; untested on hardware yet). Not yet: WPA3, and anything beyond ping (no TCP): [`docs/wifi.md`](docs/wifi.md) |

The UI renders into a cached FCRAM backbuffer and the GPU moves each finished
screen to the panel in one blit, rather than rasterising straight into uncached
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
| Wi-Fi | `src/os/WiFi9.c`, `src/os/Crypto.c` (the PMK) | `src/os/WiFi11.c`, `src/os/Wpa11.c` (WPA2), `src/os/Net11.c` (DHCP, ARP, DNS, ping) |
| GPU | `src/os/Gpu9.c` | `src/os/Gpu11.c` |
| GPU 3D pipeline (P3D) | `src/os/P3d9.c` (command lists) | `src/os/P3d11.c` (runs them) |
| Stereoscopic top screen | `src/os/Stereo9.c` (slider, model) | `src/os/Stereo11.c` (mode, barrier) |
| Codec bus (shared by audio + touch) | | `src/os/Codec11.c` |
| Core entry and command loop | | `src/os/Core11.c` |
| New 3DS clock switch | `src/model.c` | `src/os/Clock11.c` |

The ARM11 files link into one core binary, which the ARM9 embeds and wakes;
`src/os/core11.h` carries what they share. ARM9-only modules keep plain names:
`src/screen.c`, `src/power.c`, `src/i2c.c`, `src/sdmmc.c`, `src/os/Timer9.c`,
`src/assets.c`, `src/ui.c`, `src/image.c`, `src/jpeg.c`, `src/wav.c`,
`src/wavload.c`, `src/model.c`, the status bar in `src/os/StatusBar.c`, the File
Explorer in `src/os/Files.c` with its viewers in `src/os/FileView.c` and its
operations in `src/os/FileOps.c`, screenshots in `src/os/Screenshot.c`, touch
calibration in `src/os/TouchCal.c`, the terminal in `src/os/Terminal.c` with its
commands in `src/os/TermCmds.c`, the render test in `src/os/RenderTest.c`, and
the Home Menu in `src/os/HomeMenu.c` with its arrangement in
`src/os/HomeLayout.c`, the aShop store in `src/os/Store.c`, the Wi-Fi
firmware copy in `src/os/FwDump.c` (with the read-only NAND reader in
`src/os/Nand.c` and the AES engine in `src/os/Aes.c`), the 3D Model screen in `src/os/Model3D.c` with its GLB loader in
`src/os/Glb.c` and JSON tokenizer in `src/os/Json.c`, and the software 3D
renderer in `src/os/Soft3D.c`. The model's vertex shader is
`src/os/model.v.pica`, assembled at build time by picasso (devkitPro's 3DS
tools) and converted by `tools/shbin2c.py`.

Art and fonts are built into `Aurora/assets.pak` by `tools/mkassets.py` (with
`tools/png_read.py` and `tools/ttf.py`) from `icons/` and `assets/fonts/`, and
loaded at boot by `src/assets.c`. Run `make assets` after changing either.

## Documentation

* [`docs/apps.md`](docs/apps.md): the app container format and loader
* [`docs/assets.md`](docs/assets.md): the SD asset pack, icons and fonts
* [`docs/files.md`](docs/files.md): the File Explorer, file operations, text viewer, hex editor, image and audio decoding
* [`docs/input.md`](docs/input.md): touch calibration, the circle pad and screenshots
* [`docs/terminal.md`](docs/terminal.md): the terminal, its keys and every command
* [`docs/gpu.md`](docs/gpu.md): PICA200 driver and the rendering path
* [`docs/home.md`](docs/home.md): the Home Menu: pages, folders, moving apps, the layout file
* [`docs/store.md`](docs/store.md): aShop, the store: screens, the catalogue format, downloads, what is still a demo
* [`docs/glb.md`](docs/glb.md): the 3D Model screen and the GLB loader: what it reads, limits, textures, memory
* [`docs/soft3d.md`](docs/soft3d.md): the software 3D renderer, the model viewer's fallback
* [`docs/stereo3d.md`](docs/stereo3d.md): the 3D screen, the parallax barrier and the PICA200's 3D pipeline
* [`docs/ui.md`](docs/ui.md): rounded shapes, screen transitions and animation
* [`docs/audio.md`](docs/audio.md): CSND playback and the channel registers
* [`docs/power.md`](docs/power.md): MCU real-time clock, battery, power off and reboot
* [`docs/wifi.md`](docs/wifi.md): Wi-Fi bring-up, state and findings, and copying the firmware from the console
* [`auric-lang/README.md`](auric-lang/README.md): the Auric language and compiler

## License
Aurora is licensed **GPL-3.0** (see `LICENSE`). Two drivers are separate
**GPL-2.0** components, because GPL-2.0-only is incompatible with GPL-3.0 and
they keep the licence of the code they derive from:

- **Wi-Fi driver**: see `LICENSE.wifi` and `docs/wifi.md` "License and credits".
  Derives from the ath6kl legacy driver as ported to the 3DS by **Octoblimp**.
- **PICA200 GPU driver**: see `docs/gpu.md` "License and credits". Derives from
  the Linux Nintendo 3DS PICA200 driver (`ctr_pica.c`).
`LICENSE.wifi` holds the GPL-2.0 text used by both of those components.

The New 3DS clock switch in `src/os/Clock11.c` follows the register sequence in
fastboot3DS and libn3ds (derrek and profi200, GPL-3.0, the same licence as
Aurora); see `docs/gpu.md` "Render test".

The UI font is **Figtree** under the **SIL Open Font License 1.1** (Copyright
2022 The Figtree Project Authors). Its licence is `assets/fonts/OFL.txt`, and
it covers both the TTFs in `assets/fonts/` and the glyph atlases rendered from
them into `Aurora/assets.pak`. The OFL is compatible with GPL-3.0 for
distribution; it is kept as-is and recorded here and in `docs/assets.md`.

## How To Install:
- Place `Aurora.firm` in `SD:\luma\payloads`
- Place `AURORAOS.BIN` in root of SD (`SD:\`)
- Copy the built `Aurora\assets.pak` to the card as `SD:\Aurora\assets.pak`
  (optional: without it the UI falls back to its built-in icons and font)
- Optional: copy the `Aurora\Store` folder to `SD:\Aurora\Store` for aShop's
  banners; see [`docs/store.md`](docs/store.md)
- Optional: copy apps such as `Games/Tetris.BIN` to `SD:\Aurora\Apps\`. Tetris
  plays sounds from `SD:\Aurora\Apps\TETRIS\` when they are there; see
  [`docs/apps.md`](docs/apps.md)
- Wi-Fi needs your console's own Wi-Fi firmware. The first time you open
  Settings > Wi-Fi, Aurora offers to copy it from the system NAND (read only,
  after a button code) to `SD:\Aurora\wifi\`; it can also be extracted on a
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
- **Y** opens the menu to move a tile, make, rename or remove a folder
- Hold a tile with the stylus to drag it; drop it on a folder to put it in, or
  on another app to make a folder of the two
- **B** leaves a folder
- The power icon at the top left of the touch screen turns the console off
- **START**, or the settings icon at the top right of the touch screen, opens
  Settings
- **X** opens the [terminal](docs/terminal.md)
- **L** + **R** together take a screenshot on any screen
- More in [`docs/home.md`](docs/home.md)

### AI Disclaimer:
AI was used in the making of most documentation and some in-code comments. AI was used for the writing of arm assembly, Mainstream Corperate AI was not used. A local model was used on the PC of @DisLoPik.