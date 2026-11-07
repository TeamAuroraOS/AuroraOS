# Source layout

Where the code for each part of Aurora lives. Back to the
[README](../README.md).

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
