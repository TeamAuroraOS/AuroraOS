# CPU stress test

One CPU benchmark and stress test, built twice: as an AuroraOS app and as
homebrew for Nintendo's own 3DS firmware (Horizon), so the two systems can be
compared on the same console. Both run the same workloads on the same core,
show the same two screens and write the same report.

| Folder | Runs on | Results file |
|--------|---------|--------------|
| `aurora/` | AuroraOS, as a C app (the SDK in `../sdk`), with core 116 or later | `SD:/Aurora/stress.txt` |
| `horizon/` | Nintendo's firmware, as a `.3dsx` from the Homebrew Launcher (libctru) | `SD:/Aurora/nintendo_stress.txt` |
| `common/` | both: the workloads (`work.c`), the phases, the screens, the report | |

## The same core on both

The 3DS has an ARM9 and an ARM11 (MPCore with a VFPv2 FPU: two cores on an
Old 3DS, four on a New 3DS). Horizon runs apps on the ARM11; AuroraOS runs apps
on the ARM9 and keeps the ARM11 for its own core (graphics, sound, touch,
Wi-Fi). So that both versions measure the same thing:

* **The workloads run on ARM11 core 0 in both.** Horizon runs them in the
  app's main thread. The AuroraOS version hands them to AuroraOS's core as
  jobs (`AUDIO_CMD_RUN11`, from core 116; `docs/audio.md`, *Running an app's
  code on the ARM11*), each about a quarter of a second long, while the ARM9
  draws the screens and reads the battery.
* **The same machine code.** `common/work.c` is built for the ARM11 with the
  same compiler and flags on both (`-march=armv6k -mtune=mpcore
  -mfloat-abi=hard -O2`, loops kept as loops, no unaligned accesses); the two
  builds' `work_run` come out instruction for instruction the same. The VFP
  runs in RunFast mode (flush-to-zero, default NaN) on both.
* **The same memory system.** AuroraOS's ARM11 normally runs with no caches
  at all; for a job the core turns on the MMU with FCRAM cached, the L1
  caches, branch prediction and the VFP, as Horizon has them, and puts its own
  state back afterwards.
* **The same clock.** On a New 3DS both switch the ARM11 to 804 MHz at start
  and back to 268 MHz at the end, with the 2 MB L2 cache off (AuroraOS does
  not drive the L2 controller). The Horizon version's X button on the start
  screen also offers 804 MHz with the L2 cache on, and 268 MHz, for extra
  data points.

What still differs, and shows in the report: Horizon's kernel can interrupt
the app's thread (other threads, interrupts), while an AuroraOS job runs with
interrupts off; and during the stress the Horizon version also runs workers
on the other cores Horizon lets an app use (core 1 at 80%, core 2 on a New
3DS), which AuroraOS has none of (its ARM11 core 0 is the only one running).
Compare the one-core results and "This core"; "All cores" is the whole CPU an
app gets.

## What it does

Press A on the start screen. The run takes about a minute and a half with the
default 60 s of stress:

| Phase | Time | Does |
|-------|------|------|
| Idle | 5 s | nothing: the baseline temperature |
| Clock | 3 s | 256 dependent adds a pass in ARM instructions: one cycle each, so adds per second is the clock in MHz |
| Integer | 5 s | shifts, multiplies, rotates and a data-dependent branch: millions of iterations a second |
| Float | 5 s | four multiplies and four adds an iteration in single precision: MFLOPS |
| Memory | 6 s | read, then write, then copy a 4 MB buffer (bigger than every cache, the New 3DS's L2 too): MB/s, where MB is 1,000,000 bytes |
| Stress | 30 s to 10 min (L/R) | rounds of all three at once (integer, float, a 64 KB copy) as fast as it can, plus the other cores on Horizon |
| Cooldown | 10 s | nothing again, to see the temperature come back |

Every second it records:

* **Load**: the share of that second core 0 spent running the workloads. The
  rest is the screens (drawn on core 0 under Horizon, presented by core 0
  under AuroraOS) and, in the idle phases, nothing. About 95% while working.
* **Speed**: stress rounds per second of work, and the clock measured again
  (5 ms of adds). A slower CPU would show as the white line dropping.
* **Battery temperature**, charge and voltage from the power chip (MCU
  registers `0x0A` to `0x0D`, see `../docs/power.md`). The 3DS has no CPU
  temperature sensor software can read; the battery's is the only one.

The report also gives each one-core result per MHz.

## The screens

**Top**: the graph of the whole run. Green columns are the load, the white
line the stress speed (100% is the fastest second), the orange line the
battery temperature (right-hand scale). The coloured strip under it shows the
phases; the grey cursor is now.

**Bottom**: the clock setting, the phase and its progress, the results so far,
the load and clock this second, and the battery (temperature, charge,
voltage, `+` while charging).

Buttons: **A** start, **L/R** (or left/right) the stress length, **START**
quit; during the run **START** or **B** stops early (the results are still
saved, marked as stopped); afterwards **A** runs it again.

## The results file

Plain text, the same layout from both, lines ending with CR LF: the system,
console, CPU, clock setting, compiler flags and date; the one-core results
with their per-MHz figures; the stress (rounds per second while working and
overall, the slowest and fastest second, the load, the clock under load, the
other cores and all cores together); the temperature (start, highest, end,
rise); the battery; then one line per second with the time, phase, load,
rounds/s, other cores' rounds/s, MHz, temperature, charge and voltage, ready
to paste into a spreadsheet.

## Building and running

**AuroraOS**: the app needs AuroraOS with ARM11 core 116 or later, so build and
copy the OS first (`make os` at the repository root, then `AURORAOS.BIN` to the
SD card's root; the OS replaces the running core at the next boot). Then, from
anywhere:

```
py cpu-stress/aurora/build.py
```

It builds the ARM11 job (`aurora/arm11/job.c` and `common/work.c`, linked at
`0x25100000`) into `aurora/build/job11_blob.h`, then the app with the SDK.
Copy `cpu-stress/aurora/CPUStress.bin` to `SD:/Aurora/Apps/C/` and start
**CPUStress** from the Home Menu. An older core is refused with a message.

**Horizon**: with devkitPro's devkitARM and libctru (the `3ds-dev` group),
from PowerShell or the devkitPro shell, where `DEVKITARM` is set:

```
cd cpu-stress/horizon
make
```

Copy `cpu-stress.3dsx` to `SD:/3ds/` and start **CPU Stress Test** from the
Homebrew Launcher. The 804 MHz switch goes through `ptm:sysm`, which Luma3DS
lets homebrew use; if it is refused the report says so and the clock shows
what it got. Do not open the HOME Menu during the run: the time the app is
suspended spoils that second.

## Files

| File | |
|------|--|
| `common/work.c`, `work.h` | the workloads: no C library, no state but the `WorkCtx` passed in |
| `common/stress.c`, `stress.h` | the phases, the job sizing and timing, samples, both screens and the report; what each platform supplies |
| `common/draw.c`, `draw.h`, `font8x8.h` | rectangles, lines and 8x8 text in the framebuffer layout both systems share |
| `aurora/main.c` | the AuroraOS platform: posts the jobs and the 804 MHz switch to the core, the SDK for the rest |
| `aurora/arm11/job.c`, `job11.h`, `job11.ld` | the ARM11 job: its entry, the block it shares with the app, its link at `0x25100000` |
| `aurora/build.py`, `CPUStress.icon` | the build, and the Home Menu icon |
| `horizon/source/main.c` | the Horizon platform: libctru, the 804 MHz and L2 settings through `ptm:sysm`, threads on the other cores |
| `horizon/Makefile`, `icon.png` | devkitPro's 3DS application template, pointed at `../common` |

## Status

Built for both (2026-10-08, devkitARM gcc 16.1), with the workload code
checked identical between them. Both ran to the end on a New 3DS on
2026-10-08 (804 MHz, L2 cache off, a 60 s stress) and wrote their results, so
core 116's `AUDIO_CMD_RUN11` and the 804 MHz switch work there. An Old 3DS is
untested. If the AuroraOS version hangs as you press A, the first job did (the
MMU set-up in `src/os/Run11.c`); if it hangs as it opens, the 804 MHz switch
did; either way the console has to be turned off.

That run, ARM11 core 0 on both:

| | AuroraOS | Horizon |
|---|---|---|
| Clock | 800.4 MHz | 799.3 MHz |
| Integer | 39.0 M iterations/s | 40.1 M iterations/s |
| Float | 117.6 MFLOPS | 123.4 MFLOPS |
| Memory read / write / copy | 157.5 / 254.6 / 107.1 MB/s | 180.4 / 417.7 / 173.7 MB/s |
| Stress, core 0 | 761.8 rounds/s, 0.5% spread | 571.1 rounds/s, 2.6% spread |
| Stress, every core the app gets | 723 rounds/s (one core) | 1936 rounds/s (four cores) |

Computing is about equal. Horizon writes memory faster, perhaps because of
the write-allocate setting `Run11.c` gives FCRAM. On core 0 alone AuroraOS's
stress runs faster and steadier, since nothing interrupts a job and no other
core shares the memory; Horizon gives an app the other cores too.
