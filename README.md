# Aurora

[![Discord](https://img.shields.io/badge/Discord-Join%20us-5865F2?logo=discord&logoColor=white)](https://discord.gg/T25cytDaAK)
[![YouTube](https://img.shields.io/badge/YouTube-red?logo=youtube&logoColor=white)](https://www.youtube.com/@Aurora3DS)
![Platform](https://img.shields.io/badge/platform-Nintendo%203DS-red?logo=nintendo3ds&logoColor=white)
![Status](https://img.shields.io/badge/status-in%20development-yellow)

Aurora is a custom OS for the Nintendo 3DS. **Current version: Beta 8 v0.1.7-nova7.**

*Built using knowledge from GodMode9 and Luma source code.*

It has a Home Menu with pages, folders and themes, a File Explorer, a terminal,
Wi-Fi with HTTPS, an app store (aShop), stereoscopic 3D, and apps written in
[Auric](auric-lang/README.md) or [C](sdk/README.md). The full list of what
works is in [`docs/features.md`](docs/features.md).

## How to install

- Copy `Aurora.firm` to `SD:/luma/payloads/`
- Copy `AURORAOS.BIN` to the root of the SD card
- Copy `Aurora/assets.pak` to `SD:/Aurora/assets.pak` (optional: without it the
  UI falls back to its built-in icons and font)

Optional:

- Copy the `Aurora/Store` folder to `SD:/Aurora/Store` for aShop's banners and
  welcome art
- Apps such as `Games/Tetris.BIN` go in `SD:/Aurora/Apps/`, apps written in C
  in `SD:/Aurora/Apps/C/`; see [`docs/apps.md`](docs/apps.md) and
  [`sdk/README.md`](sdk/README.md)
- Wallpaper themes go in `SD:/Aurora/themes/<Name>/` as `top.png` (400x240) and
  `bottom.png` (320x240); pick one in Settings > Theme

Wi-Fi needs the console's own Wi-Fi firmware. The first time you open
Settings > Wi-Fi, Aurora offers to copy it from the system NAND (read only) to
`SD:/Aurora/wifi/`. See [`docs/wifi.md`](docs/wifi.md).

## How to open

- Make sure [loading custom firms](https://wiki.hacks.guide/wiki/3DS:Luma3DS/Configuration#Enable_loading_external_FIRMs_and_modules) is enabled
- With the system off, hold `START` while booting
- Select `Aurora`, then `Boot Aurora`

## Controls

| Input | Action |
|-------|--------|
| D-pad + **A**, or tap a tile | open an app |
| **L** / **R**, or swipe | turn the page |
| **Y** | move a tile, make, rename or remove a folder, delete an app |
| Hold a tile with the stylus | drag it; drop it on a folder to put it in, or on an app to make a folder |
| **B** | leave a folder |
| **START**, or the settings icon | Settings |
| **X** | [terminal](docs/terminal.md) |
| **L** + **R** | screenshot, on any screen |
| **L** + **Down** + **Select** | system menu, here and in apps |

The power icon at the top left of the touch screen turns the console off. More
in [`docs/home.md`](docs/home.md).

## Documentation

* [`docs/features.md`](docs/features.md): everything that works, and its state
* [`docs/source-layout.md`](docs/source-layout.md): where the code for each part lives
* [`docs/home.md`](docs/home.md): the Home Menu, the system menu, wallpaper themes
* [`docs/apps.md`](docs/apps.md): the app container format and loader
* [`docs/store.md`](docs/store.md): aShop, the store, and publishing apps
* [`docs/account.md`](docs/account.md): Aurora accounts
* [`docs/files.md`](docs/files.md): the File Explorer, viewers, image and audio decoding
* [`docs/terminal.md`](docs/terminal.md): the terminal and every command
* [`docs/input.md`](docs/input.md): touch calibration, the circle pad, screenshots
* [`docs/power.md`](docs/power.md): clock, battery, power, the lid, brightness
* [`docs/wifi.md`](docs/wifi.md): Wi-Fi bring-up and copying the firmware
* [`docs/tls.md`](docs/tls.md): HTTPS and the certificates it trusts
* [`docs/audio.md`](docs/audio.md): CSND playback and the ARM11 core
* [`docs/gpu.md`](docs/gpu.md): the PICA200 driver and the rendering path
* [`docs/ui.md`](docs/ui.md): shapes, screen transitions and animation
* [`docs/assets.md`](docs/assets.md): the SD asset pack, icons and fonts
* [`docs/stereo3d.md`](docs/stereo3d.md): the 3D screen and the PICA200's 3D pipeline
* [`docs/glb.md`](docs/glb.md): the 3D Model screen and the GLB loader
* [`docs/soft3d.md`](docs/soft3d.md): the software 3D renderer
* [`docs/video.md`](docs/video.md): video playback in apps
* [`auric-lang/README.md`](auric-lang/README.md): the Auric language and compiler
* [`sdk/README.md`](sdk/README.md): writing apps in C
* [`cpu-stress/README.md`](cpu-stress/README.md): the CPU stress test and its results

## License

Aurora is licensed **GPL-3.0** (see `LICENSE`), with these exceptions:

- The **Wi-Fi driver** (from the ath6kl legacy driver, ported to the 3DS by
  **Octoblimp**) and the **PICA200 GPU driver** (from the Linux Nintendo 3DS
  driver `ctr_pica.c`) stay **GPL-2.0**, the license of the code they derive
  from, since GPL-2.0-only is incompatible with GPL-3.0. The text is in
  `LICENSE.wifi`; credits are in `docs/wifi.md` and `docs/gpu.md`.
- The New 3DS clock switch in `src/os/Clock11.c` follows fastboot3DS and libn3ds
  (derrek and profi200, GPL-3.0).
- The **Figtree** font is under the **SIL Open Font License 1.1** (Copyright 2022
  The Figtree Project Authors), in `assets/fonts/OFL.txt`. It covers the TTFs in
  `assets/fonts/` and the glyph atlases made from them in `Aurora/assets.pak`.
- **BearSSL** by Thomas Pornin is under the **MIT license**
  (`lib/bearssl/LICENSE.txt`).
- The root certificates in `assets/certs/roots.pem` (and `src/os/TlsRoots.c`)
  come from the Mozilla CA store as curl publishes it, under the MPL-2.0; see
  `docs/tls.md`.

### AI Disclaimer:
AI was used in the making of most documentation and some in-code comments. AI was used for the writing of arm assembly, Mainstream Corporate AI was not used. A local model was used on the PC of @DisLoPik.
