# What works

Every feature in Aurora and its state. Back to the
[README](../README.md).

| Area | State | Notes |
|------|-------|-------|
| Home Menu + Settings | working | app grid, accent colors, four languages, About page with More Info (eMMC CID) and credits (the card's free space is counted while the page stays responsive); Settings > Reset deletes `USER.dat` so the setup runs again |
| Wallpaper themes | new | Settings > Theme lists the folders in `SD:/Aurora/themes` that hold a 400x240 `top.png` and a 320x240 `bottom.png`, and puts them on both screens instead of the built-in wallpaper; the choice is kept in `SD:/Aurora/theme.txt`; see [`docs/home.md`](home.md) |
| Deleting apps | new | **Y** on an SD app > Delete App removes its `.bin` after asking, then offers to delete its data folder as well; see [`docs/home.md`](home.md) |
| Home Menu pages and folders | working | up to 12 pages of 15; folders two levels deep; moving apps by dragging or with the D-pad; Power in the bar; saved in `SD:/Aurora/HomeMenu.txt`; see [`docs/home.md`](home.md) |
| System menu | new | **L + Down + Select** on any screen and inside apps: Return to Home, Power Off, Cancel ("Aurore Tools"). In apps the ARM11 core draws it; Return to Home works in apps built with the Beta 7 runtimes (C SDK 1.2 or later, Auric), and an older app says to press HOME instead; see [`docs/home.md`](home.md) |
| Icons, wallpaper, type | working | real art + Figtree from SD, with accents (the built-in 8x8 font draws accented letters too); a message at boot when `assets.pak` is missing or does not match; see [`docs/assets.md`](assets.md) |
| File Explorer | working | browse the card, per-type icons, TXT/LOG viewer, hex editor; see [`docs/files.md`](files.md) |
| File operations | working | copy, move, rename, delete and new folder, with long file names |
| Screenshots | working | L+R saves both screens as a BMP; see [`docs/input.md`](input.md) |
| aShop (store) | working | the apps and games published on the Aurora Network: sections, news, app pages, search, and downloads into `SD:/Aurora/Apps` (apps in C into `SD:/Aurora/Apps/C`, marked "C SDK" under their name), checked by SHA-256. Needs a linked Aurora account (it offers to link one) and keeps the catalogue on the card for offline browsing. Apps are published at account.aurora3ds.xyz/developer. Works on a New 3DS (2026-10-05); see [`docs/store.md`](store.md) |
| Aurora Account | working | Settings > Aurora Account (and the setup wizard's Account step) links the console to an account on account.aurora3ds.xyz: the console shows a code and a QR code, the person approves it on a phone or computer, and the token is kept in `SD:/Aurora/account.txt`. Create an account sends the person to sign up first. Works on a New 3DS (2026-10-04); see [`docs/account.md`](account.md) |
| Terminal | working | **X** on the Home Menu: a Linux-style shell with a touch keyboard, file commands, `ping` and `curl` (new: HTTP and HTTPS) over Wi-Fi, `systemctl`, `info` and app launching; see [`docs/terminal.md`](terminal.md) |
| Images | working | BMP, PNG and baseline JPEG, scaled to fit |
| WAV playback | working | 8/16-bit PCM, mono or stereo (MP3 not yet) |
| Display | working | GPU-composited; see [`docs/gpu.md`](gpu.md) |
| Animations | working | screens slide over each other with parallax, dialogs pop up, the selection springs, tiles lift and lists scroll; each screen is triple buffered and switches frames at the vertical blank, without tearing; see [`docs/ui.md`](ui.md) |
| GPU (PICA200) | working | PSC fill + PPF blit, verified on hardware |
| Audio | working | ARM11 CSND core, eight voices for apps; see [`docs/audio.md`](audio.md) |
| System sounds | new | a startup sound after a cold boot, select, back and move clicks in the Home Menu, Settings and aShop, a looping song on the Home Menu that starts after the startup sound and stops when anything opens or the lid closes, and aShop's own looping music with a chime when an install finishes, from `assets.pak` (mono, 4-bit DSP-ADPCM: the 2.9 MB aShop song is 319 KB); Settings > Startup sound, Home music, aShop music and Menu sounds turn them off; reported working on a console (2026-11-01); the Home Menu's song is by LeweckBanana; see [`docs/audio.md`](audio.md) |
| Touchscreen | working | CTR codec on the ARM11, with calibration in Settings |
| Circle pad | working | read from the same codec by the ARM11 core; also moves the selection in every menu, like the D-pad; see [`docs/input.md`](input.md) |
| 3D models (GLB) | working on a New 3DS | **3D Model** on the Home Menu shows `SD:/Aurora/model.glb` with its textures: circle pad orbits, D-pad pans, Y and A zoom; the nearest part sets the 3D depth and is named on the bottom screen; see [`docs/glb.md`](glb.md) |
| 3D (GPU) | working on a New 3DS | the PICA200's 3D pipeline from command lists, with vertex buffers and mipmapped textures; see [`docs/stereo3d.md`](stereo3d.md) |
| 3D (software) | working | the ARM9 renderer, which draws the model viewer's fallback cube; see [`docs/soft3d.md`](soft3d.md) |
| Stereoscopic 3D screen | working on a New 3DS | the 3D slider puts the top screen in 3D, with the parallax barrier on every 3DS model (and the New 3DS's movable mask); on an Old 3DS 3D switches on (the screen dims a little) but shows no depth yet (2026-10-23); see [`docs/stereo3d.md`](stereo3d.md) |
| ARM11 core updates | working | a core from another build is swapped out without a power-off, from core 83 on; see [`docs/audio.md`](audio.md) |
| Clock + battery | working | MCU over I2C, with Aurora's own clock offset in Settings > Clock (the RTC is never written); see [`docs/power.md`](power.md) |
| Screen brightness | new | Settings > Brightness: five levels on both backlights, calibrated per panel (and for the top in 3D), set by the ARM11 core (core 119) and kept in `USER.dat`; see [`docs/power.md`](power.md) |
| Closing the lid | new | both backlights go off and the OS (and apps built with this release's runtimes, C SDK 1.3) wait until the lid opens, with the apps' clocks stopped meanwhile; keys held as it opens are ignored. Sound keeps playing and the console stays powered: it is a pause, not the hardware's sleep mode (core 120); see [`docs/power.md`](power.md) |
| Video in apps | new | H.264 and AAC on the extra ARM11 cores, through FFmpeg, for C apps built with `--video`: up to 240p on an Old 3DS, 480p on a New 3DS at full power; see [`docs/video.md`](video.md). Not yet played on a console; `sdk/examples/videotest` tests it |
| Full power (New 3DS/2DS) | new | 804 MHz with the 2 MB L2 cache from boot, with a switch in Settings; hold SELECT at boot to leave it off; see [`docs/power.md`](power.md). Works on a New 3DS (2026-11-02) |
| ARM11 caches and cores | new | the core runs its own code cached, on a clock that holds at any speed, and can start the other ARM11 cores (`systemctl status cores` tests them); see [`docs/audio.md`](audio.md). Works on a New 3DS (2026-11-02) |
| Console model | working | New/Old from CFG11_SOCINFO ("New 3DS/2DS", "Old 3DS/2DS"); "N" in the status bar |
| Crash handler | working | register dump plus a three-beep error tone on a fault |
| Apps ([Auric](../auric-lang/README.md)) | working | sound from the SD card; drawing on the bottom screen and touch input (Auric v0.4, works on a New 3DS, 2026-10-05); the network (Auric v0.5: HTTP and JSON, works on a New 3DS, 2026-10-08; v0.6 adds HTTPS); see [`docs/apps.md`](apps.md) |
| Apps in C ([SDK](../sdk/README.md)) | new | `#include <aurora_app.h>`: both screens, shapes, the Figtree fonts, PNG/JPEG/BMP images, buttons, touch, circle pad, sound, files, clock, the network (HTTP and, from SDK 1.4, HTTPS over the saved Wi-Fi network); the C library through newlib (`printf` on screen, `malloc`, `fopen` on the SD card); built by `sdk/aurcc.py` into `AURC` apps (the `AUR1` container with its own magic, so Aurora knows a C app) that go in `SD:/Aurora/Apps/C` and keep their data in `SD:/Aurora/Apps/C/<Name>`, which Aurora makes; HOME and crashes return to the Home Menu. Published on aShop like Auric apps. Checked on a PC against stubbed hardware; one C app, the CPU stress test, ran on a New 3DS (2026-10-08), the examples are untested on a console |
| CPU stress test | works on a New 3DS | `cpu-stress/`: the same benchmark as an AuroraOS C app and as Horizon homebrew, on ARM11 core 0 at 804 MHz in both (AuroraOS runs it through core 116's `AUDIO_CMD_RUN11`), with a graph and a results file; see [`cpu-stress/README.md`](../cpu-stress/README.md) |
| Wi-Fi | working | Settings > Wi-Fi searches, saves a network and joins it: open and WPA2-PSK networks, the password typed on the console, an address over DHCP (New 3DS, 2026-10-03). The terminal's `ping` looks up names and pings any address. The firmware is copied from the console's own NAND the first time (read only; works on hardware). Core 114 adds a small TCP client for one HTTP exchange at a time, which Aurora accounts use (works on hardware, 2026-10-04); core 115 reads it faster, about 160 KB/s for aShop downloads (New 3DS, 2026-10-08). Auric and C apps can use the network too. A session that stops working ends by itself and the next request joins again (core 123, not yet on a console). Not yet: WPA3: [`docs/wifi.md`](wifi.md) |
| Network streams | new | an HTTP/1.1 connection that stays open while the core keeps drawing and playing (core 124): `appnet_stream_*`, `curl --speed`; see [`docs/wifi.md`](wifi.md). Works on a New 3DS, about 300 KB/s (2026-11-02) |
| HTTPS (TLS) | new | TLS 1.2 on the ARM9 (BearSSL) over a TCP connection the ARM11 core keeps open (from core 121): Aurora accounts and aShop now talk to the server over HTTPS, and C apps (SDK 1.4), Auric apps (0.6) and the terminal's `curl` can fetch `https://` addresses. Certificates are checked against 46 built-in root authorities and any added in `SD:/Aurora/certs`, with dates checked against the console's clock (never earlier than the build date). Checked on a PC against real servers, then reported working on a console (2026-11-01); see [`docs/tls.md`](tls.md) |

The UI renders into a cached FCRAM backbuffer and the GPU moves each finished
screen to the panel in one blit, rather than rasterizing straight into uncached
VRAM. Icons and text are pre-rendered at the exact size they are drawn, so the
console scales nothing at runtime. Moving between screens is animated: a slide
is one GPU copy per frame, so it costs the ARM9 nothing.
