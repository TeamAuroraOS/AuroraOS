# Aurora

  

[![Discord](https://img.shields.io/badge/Discord-Join%20us-5865F2?logo=discord&logoColor=white)](https://discord.gg/h6jpnGdUJ)

[![YouTube](https://img.shields.io/badge/YouTube-red?logo=youtube&logoColor=white)](https://www.youtube.com/@Aurora3DS)

![Platform](https://img.shields.io/badge/platform-Nintendo%203DS-red?logo=nintendo3ds&logoColor=white)

![Status](https://img.shields.io/badge/status-in%20development-yellow)

  

Aurora is a custom OS for the Nintendo 3DS. **Current version: Beta v0.1.0.**
-  *Built using knowledge from GodMode9 and Luma source code.*

## What works

| Area | State | Notes |
|------|-------|-------|
| Home Menu + Settings | working | app grid, accent colours, three languages, About page |
| Icons, wallpaper, type | working | real art + Figtree from SD, with accents; see [`docs/assets.md`](docs/assets.md) |
| File Explorer | working | browse the card, per-type icons, TXT/LOG viewer, hex editor; see [`docs/files.md`](docs/files.md) |
| Images | working | BMP, PNG and baseline JPEG, scaled to fit |
| WAV playback | working | 8/16-bit PCM, mono or stereo (MP3 not yet) |
| Display | working | GPU-composited; see [`docs/gpu.md`](docs/gpu.md) |
| GPU (PICA200) | working | PSC fill + PPF blit, verified on hardware |
| Audio | working | ARM11 CSND core, eight voices for apps; see [`docs/audio.md`](docs/audio.md) |
| Touchscreen | working | CTR codec on the ARM11 |
| Clock + battery | working | MCU over I2C; see [`docs/power.md`](docs/power.md) |
| Console model | working | New/Old from CFG11_SOCINFO; "N" in the status bar |
| Crash handler | working | register dump plus a three-beep error tone on a fault |
| Apps ([Auric](auric-lang/README.md)) | working | sound from the SD card; see [`docs/apps.md`](docs/apps.md) |
| Wi-Fi | **paused** | firmware boots, HTC handshake unsolved: [`docs/wifi.md`](docs/wifi.md) |

The UI renders into a cached FCRAM backbuffer and the GPU moves each finished
screen to the panel in one blit, rather than rasterising straight into uncached
VRAM. Icons and text are pre-rendered at the exact size they are drawn, so the
console scales nothing at runtime.

## Source layout

Subsystems that span both CPUs carry the core they run on in the file name. The
ARM9 runs the OS; the ARM11 handles the hardware the ARM9 cannot reach.

| Subsystem | ARM9 | ARM11 |
|-----------|------|-------|
| Audio (codec output, CSND) | `src/os/Audio9.c` | `src/os/Audio11.c` |
| Touchscreen | `src/os/Touch9.c` | `src/os/Touch11.c` |
| Wi-Fi | `src/os/WiFi9.c` | `src/os/WiFi11.c` |
| GPU | `src/os/Gpu9.c` | `src/os/Gpu11.c` |
| Codec bus (shared by audio + touch) | | `src/os/Codec11.c` |
| Core entry and command loop | | `src/os/Core11.c` |
| New 3DS clock switch | `src/model.c` | `src/os/Clock11.c` |

The ARM11 files link into one core binary, which the ARM9 embeds and wakes;
`src/os/core11.h` carries what they share. ARM9-only modules keep plain names:
`src/screen.c`, `src/power.c`, `src/i2c.c`, `src/sdmmc.c`, `src/os/Timer9.c`,
`src/assets.c`, `src/ui.c`, `src/image.c`, `src/jpeg.c`, `src/wav.c`,
`src/wavload.c`, `src/model.c`, the status bar in `src/os/StatusBar.c`, the File
Explorer in `src/os/Files.c` with its viewers in `src/os/FileView.c`, and the
render test in `src/os/RenderTest.c`.

Art and fonts are built into `Aurora/assets.pak` by `tools/mkassets.py` (with
`tools/png_read.py` and `tools/ttf.py`) from `icons/` and `assets/fonts/`, and
loaded at boot by `src/assets.c`. Run `make assets` after changing either.

## Documentation

* [`docs/apps.md`](docs/apps.md): the app container format and loader
* [`docs/assets.md`](docs/assets.md): the SD asset pack, icons and fonts
* [`docs/files.md`](docs/files.md): the File Explorer, text viewer, hex editor, image and audio decoding
* [`docs/gpu.md`](docs/gpu.md): PICA200 driver and the rendering path
* [`docs/audio.md`](docs/audio.md): CSND playback and the channel registers
* [`docs/power.md`](docs/power.md): MCU real-time clock and battery
* [`docs/wifi.md`](docs/wifi.md): Wi-Fi bring-up, state and findings
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
- Optional: copy apps such as `Games/Tetris.BIN` to `SD:\Aurora\Apps\`. Tetris
  plays sounds from `SD:\Aurora\Apps\TETRIS\` when they are there; see
  [`docs/apps.md`](docs/apps.md)
## How to Open:
- Make sure [loading custom firms](https://wiki.hacks.guide/wiki/3DS:Luma3DS/Configuration#Enable_loading_external_FIRMs_and_modules) is enabled.
- With system off, hold `START` while booting
- Select `Aurora` from the list
- Select `Boot Aurora`

### AI Disclaimer:
AI was used in the making of most documentation and some in-code comments. Mainstream Corperate AI was not used. A local model was used on the PC of @DisLoPik.