# Aurora

  

[![Discord](https://img.shields.io/badge/Discord-Join%20us-5865F2?logo=discord&logoColor=white)](https://discord.gg/h6jpnGdUJ)

[![YouTube](https://img.shields.io/badge/YouTube-red?logo=youtube&logoColor=white)](https://www.youtube.com/@Aurora3DS)

![Platform](https://img.shields.io/badge/platform-Nintendo%203DS-red?logo=nintendo3ds&logoColor=white)

![Status](https://img.shields.io/badge/status-in%20development-yellow)

  

Aurora is a custom OS for the Nintendo 3DS. **Current version: v0.0.9.**
-  *Built using knowledge from GodMode9 and Luma source code.*

## What works

| Area | State | Notes |
|------|-------|-------|
| Home Menu + Settings | working | app grid, accent colours, three languages |
| Display | working | GPU-composited; see [`docs/gpu.md`](docs/gpu.md) |
| GPU (PICA200) | working | PSC fill + PPF blit, verified on hardware |
| Audio | working | ARM11 core, test tone and `.aaf` playback |
| Touchscreen | working | CTR codec on the ARM11 |
| Clock + battery | working | MCU over I2C; see [`docs/power.md`](docs/power.md) |
| Crash handler | working | register dump on an ARM9 or ARM11 fault |
| Apps ([Auric](auric-lang/README.md)) | working | see [`docs/apps.md`](docs/apps.md) |
| Wi-Fi | **paused** | firmware boots, HTC handshake unsolved: [`docs/wifi.md`](docs/wifi.md) |

The UI renders into a cached FCRAM backbuffer and the GPU moves each finished
screen to the panel in one blit, rather than rasterising straight into uncached
VRAM.

## Documentation

* [`docs/apps.md`](docs/apps.md): the app container format and loader
* [`docs/gpu.md`](docs/gpu.md): PICA200 driver and the rendering path
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

## How To Install:
- Place `Aurora.firm` in `SD:\luma\payloads`
- Place `AURORA.BIN` in root of SD (`SD:\`)
## How to Open:
- Make sure [loading custom firms](https://wiki.hacks.guide/wiki/3DS:Luma3DS/Configuration#Enable_loading_external_FIRMs_and_modules) is enabled.
- With system off, hold `START` while booting
- Select `Aurora` from the list
- Select `Boot Aurora`