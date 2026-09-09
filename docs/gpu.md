# PICA200 GPU

AuroraOS drives the 3DS GPU (PICA200) for hardware framebuffer fills and blits.
This document covers what works, how it is wired, and the licence situation.

## Status

| Engine | What it does | State |
|--------|--------------|-------|
| PSC | Memory fill (hardware screen clear) | **working on hardware** |
| PPF texture copy | Raw linear -> linear blit | **working on hardware** |
| PPF display transfer | Rect copy with format conversion + v-flip | implemented, not yet exercised |
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
ARM9  os_main.c / gpu9.c            ARM11  audio11.c -> gpu11.c
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
is still there to service GPU requests. `gpu9.c` is therefore linked into every
Auric app, and `buffered(true)` installs `gpu_texcopy` as the blit hook, but
only when the Home Menu did the launching, since that is what guarantees the
ARM11 core is up. A directly booted app keeps the CPU copy rather than stalling
on a GPU that will never reply. See `auric-lang/docs/language.md`.

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

## License and credits

**This GPU code is licensed GPL-2.0**, separately from the rest of AuroraOS,
which is GPL-3.0. The files are `include/gpu.h`, `src/os/gpu11.c`,
`src/os/gpu9.c`, and the GPU Test screen in `src/os/os_main.c`. The full GPL-2.0
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
