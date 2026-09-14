# PICA200 GPU

AuroraOS drives the 3DS GPU (PICA200) for hardware framebuffer fills and blits.
This document covers what works, how it is wired, and the licence situation.

## Status

| Engine | What it does | State |
|--------|--------------|-------|
| PSC | Memory fill (hardware screen clear) | **working on hardware** |
| PPF texture copy | Raw linear -> linear blit | **working on hardware** |
| PPF display transfer | Rect copy with format conversion + v-flip | implemented, not yet exercised |
| Anti-aliased 2D primitives (CPU) | Coverage-blended edges, gradients | **working on hardware** |
| P3D | 3D command lists, shaders, vertices | **not implemented** |

Verified on a real console: hardware ID `0x00010002`, PSC fill and PPF texture
copy both reporting `DONE` with the expected control readbacks (`0x102` = PSC
done + 24-bit fill width, `0x100` = PPF done) and completing in under 200 poll
iterations.

The PPF texture copy is what presents every UI frame, see "The rendering path".
Both engines are also reachable from **Settings > GPU Test**.

## Why the GPU lives on the ARM11

The PICA200 register block is at `0x10400000`, which is **ARM11 I/O**. The ARM9
that runs the AuroraOS kernel cannot reach it, exactly as it cannot reach the
LCD brightness registers at `0x10202xxx`.

So the GPU driver follows the same split the audio, Wi-Fi and touchscreen code
already use: the work happens in the ARM11 core, and the ARM9 posts requests
through the shared command block.

```
ARM9  os_main.c / Gpu9.c            ARM11  Core11.c -> Gpu11.c
  |                                    |
  | writes params -> GpuShared         |
  | bumps AudioCtrl.cmd_seq  --------> | dispatches AUDIO_CMD_GPU
  |                                    | programs PSC / PPF registers
  | polls GpuShared.seq      <-------- | bumps seq, writes results back
```

`GpuShared` lives at `0x233C0000`, between the Wi-Fi shared block
(`0x233B0000`) and the audio PCM buffer (`0x23400000`).

One useful consequence: the GPU block is already powered when AuroraOS starts,
because the firm leaves the LCD controller driving the framebuffers. That
controller sits at `0x10400400` / `0x10400500`, inside this same register block,
and is already referenced by `aurora.h`. Only the engine clocks need enabling.

## Register map

All offsets are from `GPU_REG_BASE` = `0x10400000`.

| Offset | Name | Notes |
|--------|------|-------|
| `0x0000` | HW_ID | hardware ID; the read that proves the block is reachable |
| `0x0004` | CLOCK | write `0x00070100` to enable the PSC/PPF/P3D clocks |
| `0x0010` | PSC0_START | fill start, as **physical address >> 3** |
| `0x0014` | PSC0_END | fill end (exclusive), `>> 3` |
| `0x0018` | PSC0_VALUE | fill pattern |
| `0x001C` | PSC0_CNT | bit0 start/busy, bit1 done, bits8-9 fill width |
| `0x002C` | PSC1_CNT | second fill unit (quiesced, not used) |
| `0x0034` | BUSY | bit31 P3D busy, bit30 PPF busy |
| `0x0C00` | PPF_INPUT | source `>> 3` |
| `0x0C04` | PPF_OUTPUT | destination `>> 3` |
| `0x0C08` | PPF_DST_DIM | `width | height << 16` |
| `0x0C0C` | PPF_SRC_DIM | `width | height << 16` |
| `0x0C10` | PPF_FLAGS | bit0 v-flip, bit3 raw copy, bits8-11 in fmt, bits12-15 out fmt, bit16 block32 |
| `0x0C18` | PPF_CNT | bit0 start/busy, bit8 done |
| `0x0C20` | PPF_LEN | texture-copy byte count |
| `0x1000` | IRQ_ACK | write 0 to acknowledge a P3D completion |

Fill widths: `0` = 16-bit, `1` = 24-bit, `2` = 32-bit. AuroraOS framebuffers are
24-bit RGB8, so screen clears use width 1.

Pixel formats (PPF flag fields): `0` RGBA8, `1` RGB8, `2` RGB565, `3` RGB5A1,
`4` RGBA4.

## The two PPF modes

This distinction matters and is easy to get wrong:

- **Display transfer** (default) treats the *source* as tiled GPU render output
  and de-tiles it into a linear destination, converting format on the way. It is
  what you want to present 3D output, and the wrong thing for moving an
  ordinary linear buffer, which it would scramble.
- **Texture copy** (flags bit3) is a raw linear-to-linear blit with no tiling and
  no conversion. Both dimension registers are zero (meaning one contiguous run)
  and `PPF_LEN` carries the byte count. **This is the framebuffer blit
  primitive**, and what `gpu_texcopy()` uses.

## Using it

```c
#include "gpu.h"

gpu_init();                       /* enable clocks, read the hardware ID */

/* Hardware clear of the top screen (24-bit colour, packed b | g<<8 | r<<16). */
gpu_clear_fb((u32)VRAM_TOP_LA, TOP_FB_SIZE, 0x00301060);

/* Raw blit of a prepared linear buffer onto the top framebuffer. */
gpu_texcopy(src_addr, (u32)VRAM_TOP_LA, TOP_FB_SIZE);
```

Constraints the ARM11 side enforces, and the reasons:

- All addresses must be **8-byte aligned**, they are programmed as `addr >> 3`.
- `gpu_texcopy` lengths must be a multiple of 16.
- Display-transfer sources must be at least 64x16, the destination may not exceed
  the source, and a row (`width * bytes-per-pixel`) must be a multiple of 8, or
  of 16 for 24-bit RGB8.
- With `GPU_XF_BLOCK32`, every dimension must be a multiple of 32.

The 400x240 top framebuffer is stored column-major (memory runs down a 240-pixel
screen column, then steps to the next of 400 columns), so as a GPU image it is
**240 wide by 400 tall**. `240 * 3 = 720` is a multiple of 16, so it satisfies
the RGB8 row rule.

Every wait is bounded (`GPU_POLL_MAX`), so a wedged engine costs one failed
operation rather than hanging the ARM11, which must keep polling the
touchscreen.

## GPU Test screen

**Settings > GPU Test.**

- **A**: run `GPU_OP_INIT`: enable clocks, quiesce inherited engine state, read
  the hardware ID.
- **X**: PSC fill of the top framebuffer, cycling through three colours.
- **Y**: paint a gradient into the VRAM scratch bank with the CPU, then blit it
  to the top framebuffer with the PPF.
- **B**: back.
- **L**: benchmark. Reports, in real microseconds, the cost of one GPU round
  trip, a whole-screen PSC fill, the CPU filling the same screen, and a present.
- **R**: render test. See "Render test" below.

## The MPU was switched off

Read from the console with the `L` screen:

```
c1 0x00052078    bit 0 MPU = 0, bit 2 D-cache = 0, bit 12 I-cache = 0
c2 0x00000029    regions 0, 3 and 5 cacheable
c3 0x00000029    regions 0, 3 and 5 bufferable

region 5  0x20000000  128MB  C=1 B=1   FCRAM: code, buffers, shared blocks
region 7  0x18000000    8MB  C=0 B=0   VRAM
region 4  0x10000000    2MB  C=0 B=0   I/O registers
```

The firm leaves the region table complete and correct, FCRAM cacheable and
bufferable, VRAM and the device range not, and then hands over with the
protection unit **disabled**. With no unit there are no attributes, so nothing
was cached and nothing was buffered: every instruction fetch and every store ran
at raw FCRAM latency. That is the 77 cycles a single framebuffer byte cost, and
it is why every cache maintenance call in this tree was, until now, a no-op.

`os_mpu_enable()` in `os_launch.s` sets three bits in c1. It defines no regions;
it turns on what was already configured. Both caches are invalidated first,
because they hold whatever powered up in them.

**Holding SELECT at boot skips it.** The button register is device memory either
way, so the read works before the unit is on.

Two things made this safe to switch on. The ARM11 core copy is already bracketed
by `os_cache_sync()`, and the SD driver is PIO through `REG_SDFIFO32` rather
than DMA, so card data passes through the CPU and lands in the cache correctly.
The shared blocks each invalidate their own line before reading.

Measured with it on, against the same screen with it off:

```
                 caches off    caches on
cpu bg            90,038 us     8,949 us     10x (30x vs. byte stores)
edge patch         7,867 us       763 us     10x
bg paint           1,558 us       855 us
card               1,981 us     1,542 us
op (round trip)      402 us       551 us     slower, see below
blit                 626 us       824 us     slower, see below
Tetris              ~15 fps      137 fps
```

The GPU operations got *slower*, and that is correct. Every clean before a GPU
read used to be a no-op; now it writes dirty lines back to memory, which is the
whole point of it. Anything that was working only because the caches were off
would show up here first, as corrupted pixels.

Apps inherit the state. A Home Menu launch does not reset c1, so an Auric app
runs with the unit and caches on, which is where Tetris's speed-up came from.
That makes the app runtime's own cache maintenance real as well:
`auric-lang/runtime/auric_start.s` carries copies of `os_dcache_clean` and
`os_dcache_clean_range` for `Gpu9.c`, because an app links that file but not
the OS's launch stub. An app booted directly as `AURORAOS.BIN` has no OS before
it, so it runs with the unit off, and the same calls are harmless no-ops.

## The cost of a CPU pixel

Measured on hardware with the `L` benchmark, and the single most useful number
in this tree:

```
cpu bg 265732us     CPU filling one 320x240 screen
bg paint  798us     the same background, painted from its cache
psc       565us     GPU solid-filling the same screen
blit      641us     GPU moving a finished screen to the panel
op        405us     a GPU round trip, however small the work
```

**254 ms for a screen fill is about 222 cycles per pixel, or 74 cycles per byte
stored.** That is a full FCRAM round trip per store. ARM946E-S allocates cache
lines on a read miss but *not* on a write miss, so a buffer the CPU only writes
never enters the cache: every store goes to the write buffer and out to memory,
and a tight store loop simply saturates it. Aurora never configures the MPU, so
whatever the firm left is what applies.

The practical rules that follow:

* **Never compose a full screen with the CPU per frame.** Opening Settings used
  to paint two backgrounds, 571 ms of it, which is exactly what a half-second
  stall felt like.
* **Prefer copying to computing.** The background does not change between
  frames, so it is composed once into FCRAM (`ui_bg_build`) and painted from
  that copy: a GPU blit at 656 us, against 254 ms to recompute. The patch path
  copies the rect's column runs from the same cache rather than re-deriving the
  gradient and re-blending the pattern.
* **Write words, not bytes.** The cost scales with the number of store
  instructions, not bytes, so a `u32` store moves four bytes for the price of
  one. `clear_screen` and `copy_run` both do this.
* **Do cache maintenance by index, not by address, above a few KB.** The
  buffer has to leave the cache before the GPU writes it, or blended drawing
  reads the previous frame back. Doing that by address costs one CP15 operation
  per line *of the range*, and each costs about 82 cycles: over a 230 KB
  framebuffer that is 7,200 of them, measured at 8.8 ms, to drop at most the
  128 lines a 4 KB cache can even hold. `os_dcache_flush()` does the same job
  in 128 index operations, about 157 us.
* **Flush before the write, not after.** Cleaning and invalidating first leaves
  nothing dirty, so there is no stale CPU line to be written back over the
  GPU's output afterwards. Invalidating *after* a GPU write, without cleaning,
  would be the other option, but it discards unrelated dirty data too.

What is still expensive is any large blended area, because blending has to read
what is already there. That read allocates a line, so the writes that follow it
hit the cache; a blended fill is actually cheaper per pixel than a plain one,
which is the opposite of what one would guess.

## Input latency and the present pipeline

Making the drawing cheaper turned out not to be what made the UI feel slow.
Four things did, and none of them were pixels:

**The loop slept 8 ms between polls.** Every screen ended with `delay(60000)`,
a nop loop that disassembles to six instructions at roughly nine cycles each;
at the ARM9's 67 MHz that is about 8 ms. The loop could not look at input more
than about 120 times a second, and slept that long again after every redraw.
`ui_idle()` in `src/ui.c` replaces it with a hardware-timer wait of 1 ms.

**Directions did not repeat.** `get_keys_down()` is edge-triggered, so holding
the D-pad moved the cursor once and stopped. Most of what read as slowness was
not a slow move, it was needing a second press to move again. Directions now
repeat after 350 ms at 70 ms intervals; everything else stays edge-triggered,
because holding A must not launch an app repeatedly.

**Every present flushed both caches, twice.** `gpu_run()` called
`os_cache_sync()` before and after posting, and that routine cleans and
*invalidates* the whole D-cache and invalidates the entire 8 KB I-cache. Two
presents per screen change meant four complete cache wipes per keypress, so the
drawing code was re-fetched cold from FCRAM every frame. The I-cache part was
pure waste: nothing wrote instructions, only pixels. `os_dcache_clean()` and
`os_dcache_clean_range()` in `os_launch.s` clean without invalidating and never
touch the I-cache, so the framebuffer the CPU just wrote stays cached and
readable for the next redraw.

**The ARM9 waited for each blit.** `gpu_texcopy_async()` posts and returns;
`ui_idle()` collects the result during the wait that follows. A posted blit is
still reading the backbuffer, so every primitive in `screen.c` that writes one
calls `screen_touch()` first, which waits if a copy is reading that buffer.
That is one compare in the common case and makes the property hold by
construction, rather than by auditing 43 present call sites.

Two traps found while doing this, both worth knowing:

* `timer_ready()` is not a query. It restarts the timer and recalibrates
  against the real-time clock, which blocks across two RTC second boundaries.
  Calling it per frame would have frozen the UI outright. `timer_calibrated()`
  is the cheap check; calibration happens once at start-up.
* `GpuShared.seq` is written by the ARM11, so a clean cannot refresh it. Reading
  it after switching from clean-and-invalidate to clean-only latched a stale
  value, and the first completion check then passed against an operation that
  had never run. It needs an explicit line invalidate.

## What a GPU request costs

An operation used to cost about 0.6-0.75 ms against roughly 40 us of actual
work, because the ARM9 slept `delay(20000)` before its first completion check
and the ARM11 only looked for work every `spin(20000)`. At that price
offloading anything small was worse than doing it on the CPU, which is what
made "move the drawing to the GPU" a non-starter.

Now the ARM9 spins on the sequence counter with a single-line cache invalidate
instead of sleeping, and the ARM11 checks for work 16 times per touchscreen
sample rather than once. Touch is still sampled at its old rate.

Worth knowing before planning more offloading: the DMA engines move bytes, they
cannot blend. Anti-aliased edges, smoothed text and icons all read the existing
pixel and mix into it, which PSC and PPF cannot do at any speed. That work needs
P3D, not these engines.

Results are shown on the bottom screen: core version, GPU ready flag, hardware
ID, the step reached, the error code, the BUSY register before and after, the
engine control readback, poll iterations, and a successful-operation count.

The tests write to the **top** screen so the readout on the bottom stays legible.
Scratch space is `0x18000000`: VRAM runs `0x18000000..0x18600000` and the
framebuffers start at `0x18300000`, so the first bank is free.

**What to check first on hardware.** The hardware ID is the load-bearing result.
If it reads back `0x00000000` or `0xFFFFFFFF`, the register access itself is not
working and nothing downstream is meaningful. If the ID looks plausible but a
fill reports `TIMEOUT`, the clock enable is suspect. If a fill reports `DONE`
but the screen does not change, the engine ran but the address or fill width is
wrong.

## The rendering path

Aurora originally rasterised straight into VRAM. That is the slow way round on
this hardware, for two reasons:

1. **VRAM is uncached from the ARM9.** Every `draw_pixel` issued three separate
   byte stores that each went out to the bus. A screen of text is tens of
   thousands of them.
2. **`draw_pixel` was called per pixel.** Each call re-did bounds checks and a
   `x * screen_height` multiply, 64 times per character, 1024 times per icon.

So the UI now renders like this:

```
CPU rasterises into a cached FCRAM backbuffer   (VRAM_TOP_BACK / VRAM_BOT_BACK)
                    |
   screen_present_top() / screen_present_bottom()
                    |
        GPU PPF texture copy -> the panel        (one DMA, ~20-40 us)
```

`VRAM_TOP_LA` and `VRAM_BOT_A` are now *variables* naming the current draw
target, not fixed addresses, so all existing drawing code was unchanged. They
point at the panels by default; `screen_use_backbuffer(1)` redirects them to
FCRAM. `VRAM_TOP_PHYS` / `VRAM_BOT_PHYS` always name the real framebuffers, for
code that must reach the panel directly.

Backbuffering is enabled in `os_main()` **only if `gpu_init()` succeeds**, so a
GPU failure leaves the original direct-to-VRAM path intact. Enabling it seeds the
backbuffers from the current panel contents, so a screen that only redraws part
of itself cannot flash uninitialised memory.

`screen.c` is linked into the FIRM payload as well as the OS, and the FIRM has no
ARM11 core. The present therefore goes through the `g_screen_blit` hook, which
the OS points at `gpu_texcopy()`; when it is NULL the present falls back to a CPU
word copy. That keeps `screen.c` free of any hard dependency on the GPU driver.

**The crash handler calls `screen_use_backbuffer(0)` first.** A crash screen must
never depend on a backbuffer that something else has to present; the fault may
well be the ARM11 core or the GPU itself.

**Auric apps present through the GPU too.** An app replaces the OS at
`0x22000000`, but the ARM11 core lives at `0x23000000` and keeps running, so it
is still there to service GPU requests. `Gpu9.c` is therefore linked into every
Auric app, and `buffered(true)` installs `gpu_texcopy` as the blit hook, but
only when the Home Menu did the launching, since that is what guarantees the
ARM11 core is up. A directly booted app keeps the CPU copy rather than stalling
on a GPU that will never reply. See `auric-lang/docs/language.md`.

Shapes are drawn by coverage rather than a hard in/out test: `draw_pixel_alpha`
blends into what is already there, and the rounded-corner and upscaled-bitmap
paths in `src/screen.c` use it, which is what removed the stair-stepping. Panels
are filled with `draw_vgradient` / `draw_gradient_round_rect` instead of flat
colour.

Icons and text no longer scale at all. They are pre-rendered at the exact size
the UI draws them and blitted 1:1 from the SD asset pack; see
[`assets.md`](assets.md). The bilinear upscaling that preceded this is still in
`draw_icon_scaled`, used only as the fallback when the pack is absent.

That path is worth understanding, because both of its problems came from the
same place. Magnifying 1bpp art cannot add detail, so every edge became a ramp
`scale` pixels wide: the smoothing *was* the blur. And it cost about 48
operations per destination pixel (four filtered fetches with bounds checks,
three interpolations, a framebuffer offset recomputed per pixel) against roughly
six for a coverage blit. Supersampling would have been worse still and no
sharper, because at an integer scale every subsample of a destination pixel
falls inside the same source pixel, so coverage only ever comes out 0 or full.

Rounded corners are now table-driven. Coverage inside a corner depends only on
the radius, so it is computed once per radius and cached (`corner_table` in
`src/screen.c`). It used to call `isqrt32`, a 16-iteration loop, once per corner
pixel: a settings row draws a radius-10 ring and a radius-8 card, about 650
isqrt calls, so a six-row redraw spent roughly 63,000 loop iterations on corners
alone. That is what made Settings feel heavier than the Home Menu.

The rasterisers were also rewritten to walk columns with a pointer rather than
call `draw_pixel` per pixel. The framebuffer runs down a screen column before
stepping to the next, so a glyph column is one contiguous descending run: one
multiply per column instead of one per pixel, and no per-pixel call or bounds
test. `draw_icon_scaled` now fast-paths scale 1 to `draw_icon_32` instead of
issuing 1024 one-pixel rectangle fills.

## Not implemented: P3D

Actual 3D rendering needs a lot more: command-list assembly, vertex buffers,
shader binaries, and render-target setup, plus the extra initialisation the
reference driver performs (registers `0x1080`, `0x10C0`, `0x10D0`, `0x1914`,
`0x0050`, `0x0054`). None of that is here. AuroraOS deliberately programs only
the two DMA engines, which need no pipeline state and are the parts that make the
existing 2D UI faster.

If P3D is added later, the display-transfer path already implemented is what
presents its output.

## Render test

**Settings > GPU Test, R.** Sixty seconds of UI-shaped drawing on the top
screen, then the mean frame rate. `src/os/RenderTest.c`.

Each frame blits the cached wallpaper, moves six gradient cards with an icon and
a label each, and presents through the normal async path, so it measures what
the Home Menu does rather than a raw fill. The bottom screen shows the running
numbers four times a second, and B stops early.

Frames are counted per second of the calibrated timer, and each sample is
divided by the microseconds that second really spanned, so a frame straddling
the boundary does not skew it. The result is the mean of those samples; the
bottom screen adds the min, max, total frames and elapsed time.

### New 3DS hardware

On a New 3DS the test first asks for the fast ARM11 clock: 804 MHz on the retail
SoC (CFG11_SOCINFO bit 2), 536 MHz on the LGR1. `aurora_n3ds_hardware()` in
`src/model.c` posts `AUDIO_CMD_N3DS`, and `src/os/Clock11.c` does the work.

The register is CFG11_MPCORE_CLKCNT (0x10141300): bits 0-2 request a mode, bit
15 is set when a change has applied, bits 16-18 read back the mode in effect. A
request only applies once every ARM11 core is in WFI, and interrupt 88 then
wakes core 0. The sequence follows fastboot3DS and libn3ds: route interrupt 88
to core 0, set the extra-memory and L2C enable bits in CFG11_MPCORE_CNT, request
the mode, WFI until bit 15, then set the GPU's New 3DS bits. The L2 cache
controller itself is left off.

Aurora's ARM11 normally polls with interrupts masked and never sleeps, and a WFI
that never returns would take audio, touch and the present with it. So:

| Guard | Why |
|-------|-----|
| Fire core 0's private timer once and check the CPU interface sees it | if no interrupt reaches the core, WFI would never return; the switch is skipped |
| Keep that timer running during the wait, give up after about a second | the other core may not be parked in WFI, and then the change never applies |
| Withdraw a request that did not apply | left in place, it could apply later whenever both cores idled |
| Save and restore every GIC and timer register touched | the rest of the core expects the interrupt state it had |

Both screens show the outcome, and the bottom one adds the raw register before
and after: `Clock 00000000 to 00050005` is a switch from mode 0 to mode 5.

Only the ARM11 gets faster. The ARM9, which does the drawing, keeps its clock,
and the present is a GPU blit the ARM11 only starts, so expect this test's
number to move little; the gain is for work done on the ARM11. Spin-count
bounds there get shorter in real time at 3x, so `GPU_POLL_MAX` was tripled. The
codec's `SPI_GUARD` stays far above a transfer either way, and the `sleep_ms`
delays run only at start-up, before any switch. The mode holds until reboot.

Not yet tested on hardware.

## License and credits

**This GPU code is licensed GPL-2.0**, separately from the rest of AuroraOS,
which is GPL-3.0. The files are `include/gpu.h`, `src/os/Gpu11.c`,
`src/os/Gpu9.c`, and the GPU Test screen in `src/os/os_main.c`. The full GPL-2.0
text is in `LICENSE.wifi` at the repository root; despite the name it is the
plain licence text, and it covers both GPL-2.0 components.

The register map and the init / fill / transfer sequences derive from the **Linux
Nintendo 3DS PICA200 driver** (`ctr_pica.c`, GPL-2.0), part of a Linux-on-3DS
port. GPL-2.0-only is **incompatible with GPL-3.0**, so this code keeps its
original licence rather than being relicensed. Any binary linking it is covered
by GPL-2.0, the same situation as the Wi-Fi driver (see `docs/wifi.md`).

Specific facts sourced from that driver: the register offsets tabulated above;
the clock-enable value `0x00070100`; the quiesce sequence that clears the low
byte of the PSC control registers and bits 8-15 of the PPF control register; the
P3D interrupt-acknowledge idiom (write 0 to `0x1000`, then read `0x0034` to flush
the posted write); the PPF programming order and its completion/acknowledge
protocol on bit 8; and the transfer parameter validation rules.

Related reference sources reviewed alongside it, also GPL-2.0 and **not**
incorporated: `ctr_gpio.c` (a generic `gpio-mmio` driver, addresses come from the
device tree) and `smp.c` (New3DS core 2/3 bring-up and SOCMODE clocking).

Other sources: GBATEK "3DS Video", 3dbrew GPU/GPUREG documentation, and libctru's
`GX_MemoryFill` / `GX_DisplayTransfer` / `GX_TextureCopy` for the mode semantics.

The render test (`src/os/RenderTest.c`) and the New 3DS clock switch
(`src/os/Clock11.c`, with its ARM9 side in `src/model.c`) are **not** part of the
GPL-2.0 GPU code; they are GPL-3.0 like the rest of AuroraOS. The clock sequence
follows **fastboot3DS** `source/arm11/hardware/cpu.c` and **libn3ds**
`source/arm11/drivers/pdn.c`, both by derrek and profi200 under GPL-3.0-or-later,
which is compatible. Register layouts come from GBATEK "3DS Config Registers"
and libn3ds `gic.h` / `timer.h`. `smp.c` above was not used for it.
