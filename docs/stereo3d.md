# Stereoscopic 3D and the PICA200's 3D pipeline

| Piece | File |
|-------|------|
| Top screen 2D/3D mode, parallax barrier, New 3DS mask | `src/os/Stereo11.c` (ARM11) |
| Stereo presents, mode-switch op | `src/os/Gpu11.c` (ARM11), `src/os/Gpu9.c` |
| Model, 3D slider, 3D LED | `src/os/Stereo9.c`, `include/stereo.h` |
| P3D command-list runner | `src/os/P3d11.c` (ARM11) |
| Command-list builder | `src/os/P3d9.c`, `include/p3d.h` |
| Vertex shader | `src/os/model.v.pica`, built by `tools/shbin2c.py` |
| The 3D Model screen | `src/os/Model3D.c`; see [`glb.md`](glb.md) |

**Status: working on a New 3DS** (P3D, the 3D screen and the barrier,
confirmed 2026-10-02 with the cube). The Old 3DS and 3DS XL paths (plain
barrier, 3D LED) are untested. Textured models drawn from vertex buffers work
as well; see [`glb.md`](glb.md).

## Nothing has to be dumped

Both halves are documented well enough to implement from published sources,
and other bare-metal software already does each of them:

* **The stereo display** (two framebuffers, the parallax barrier, the slider)
  is in GBATEK and 3dbrew, and libn3ds (profi200, derrek; the library under
  open_agb_firm) implements it from Nintendo's GSP module.
* **The New 3DS barrier** is a liquid-crystal mask driven by an I2C expander.
  TuxSH documented it in libctru's `qtm.h` and `qtmc.h`, down to the
  register writes.
* **The 3D pipeline (P3D)** is the PICA200's command lists, shaders and render
  targets. GBATEK has a register reference and a triangle sample, citro3d and
  libctru encode every register, and open_agb_firm runs P3D command lists on
  bare metal today.

Console data is only needed for refinements, all optional:

| Data | Where it lives | What it would add |
|------|----------------|-------------------|
| Slider calibration (`SVR2` min/max) | HWCAL in NAND (`ro/sys/HWCAL0.dat`), config block `0x00120000` | exact slider ends; Aurora uses bounds that cover every console's factory range instead |
| Backlight curves for 3D | HWCAL, block `0x00050002` | GSP makes the top screen brighter in 3D (half the pixels reach each eye); Aurora does not yet |
| 2D/3D switching delays | HWCAL, block `0x00050006` | a few frames; Aurora waits two frames |
| New 3DS barrier centre | config block `0x00180001` (QTM calibration) | the head-tracking centre position; Aurora uses the fixed pattern, which needs none |

HWCAL sits in the console's encrypted NAND. libn3ds reads a copy dumped from
the console to `sdmc:/3ds/HWCAL.dat` (a NAND browser such as GodMode9 can make
one); Aurora could do the same later.

## How the stereo display works

### Two pictures, one screen

The top LCD is 800 pixels wide behind a parallax barrier: in 3D, every other
column of pixels is seen by one eye only. Its display controller (PDC0,
`0x10400400`) fetches either one 400-pixel picture and doubles each line (2D),
or a left and a right picture interleaved column by column (3D):

| Register | 2D (as Luma3DS leaves it) | 3D (GSP, via libn3ds) |
|----------|---------------------------|------------------------|
| `+0x24` lines per frame - 1 | 413 | 827 |
| `+0x2C`, `+0x30`, `+0x34` border, blank, sync start | 402 | 802 |
| `+0x40` interrupt lines | 402-406 | 802-806 |
| `+0x64` picture lines | 2-402 | 2-802 |
| `+0x70` format bits 4-6 | `0x40` (double each line) | `0x20` (left and right interleaved) |

The 2D mode doubles the clock as well as the lines, so both modes refresh at
the same 16.7 ms. `Stereo11.c` writes libn3ds's full 3D preset, after saving
what was there, and puts the saved values back for 2D. The panel is forced
black (`0x10202204`) for two frames around the change, as GSP does.

The right eye's address goes in `+0x94` / `+0x98`, next to the left eye's
`+0x68` / `+0x6C`. Aurora's three-framebuffer presents gained three right-eye
twins in VRAM (`GPU_FB_TOP_RA..RC`). A 2D frame points both eyes at the same
framebuffer, so the screen can stay in 3D mode and still show 2D pictures
correctly.

### The barrier

| Model | What switches the barrier on |
|-------|------------------------------|
| 3DS, 3DS XL | `0x10202004 = 0x0A390A39` (2.5 ms on/off PWM), then `0x10202000 = 0x00010001` |
| New 3DS, New 3DS XL | the same registers, and the mask expander below |
| 2DS, New 2DS XL | no barrier: 3D is never switched on |

The values come from libn3ds `LCD_setParallaxBarrier()`, written from GSP.
GBATEK lists the bits but was unsure of the value. Rosalina (Luma3DS) checks
bit 0 of `0x10202000` to know the top screen is in 3D on every model, which is
why Aurora writes it on the New 3DS too.

The New 3DS replaces the plain barrier with a mask of twelve repeating units
it can move, for "super-stable 3D". Per libctru `qtmc.h`:

1. Set GPIO3 bit 11 (`0x10147020`/`0x10147022`) as an output, high. This
   releases the expander from reset.
2. On I2C bus `0x10161000`, device `0x40` (a TI TCA6416A), set all pins to
   outputs (registers `0x06`, `0x07` = 0), then write 0 (every unit clear) to
   the outputs (`0x02`, `0x03`).
3. Write the pattern: bits 0-11 are the units (1 = opaque, bit 11 leftmost)
   and bit 12 is the polarity. **Then keep alternating it with
   `pattern ^ 0x1FFF`, about 100 times a second, for as long as it is on.**
   Liquid crystal held at one polarity carries a DC voltage; Nintendo's QTM
   alternates it at all times.

Aurora uses the pattern QTM uses with super-stable 3D off, `111100000111`, and
X on the 3D Model screen steps through QTM's twelve head-tracking positions in
case another one suits a console better. The ARM11 owns the expander's bus
(the ARM9 never uses bus `0x10161000`). Its core loop, the vertical-blank waits
and the P3D wait all call `stereo11_tick()`. That function times itself on the
core's private watchdog, used as a free-running counter, and writes the next
phase every 10 ms at 268 MHz (3.3 ms at 804 MHz). Going back to 2D writes the
all-clear pattern and stops alternating. All-clear with the common electrode
low puts no voltage across any unit.

How the 13 bits split between the expander's two output registers is
inferred: low byte to `0x02`, high byte to `0x03`, the way the chip's register
pair works and QTM's description reads. If the New 3DS shows no 3D, this is
the first thing to question.

### The slider and the LED

The MCU reports the slider's raw ADC value in register `0x08` (0-255). HWCAL's
`SVR2_Min` and `SVR2_Max` map it to 0.0-1.0. Factory values are 0x07-0x1B and
0xF2-0xFD, so Aurora treats 0x20 and below as off and 0xF0 and above as full.
The bottom screen shows the raw value.

MCU register `0x7F` byte 9 is the model (0 3DS, 1 3DS XL, 2 New 3DS, 3 2DS,
4 New 3DS XL, 5 New 2DS XL). On the 3DS and 3DS XL, MCU register `0x2C`
switches the 3D LED (1 on, 0 off); the New models have none.

### Eyes

Both renderers use the same off-axis stereo. Each eye's camera moves sideways
by half the eye distance, and its picture shifts back so that a point at the
convergence distance falls on the same pixel for both. That distance is the
depth of the model's nearest visible vertex, found every frame, so the closest
part sits on the screen's surface and the rest goes behind it (see
[`glb.md`](glb.md)). The eye distance is 3.5% of it at the slider's top. The
left eye is the negative offset. `p3d_persp_stereo_tilt()` puts this into
citro3d's `Mtx_PerspStereoTilt` layout. The software renderer's `eye` and
`conv` fields (`soft3d.h`) do the same in fixed point.

## How P3D runs on Aurora

The GPU's registers are ARM11-only, so the work splits:

1. **The ARM9 builds a command list** (`P3d9.c`) in a 16-byte-aligned buffer
   in FCRAM. Each entry is a parameter, a header (register, byte mask, count,
   consecutive or not), and the rest of the parameters, padded to 8 bytes.
   The list ends with a write of `0x12345678` to `FINALIZE` (register
   `0x010`), padded to 16 bytes.
2. **The ARM11 runs it** (`GPU_OP_P3D` in `P3d11.c`):
   - Once only: `CFG11_GPUPROT = 0` (`0x10140140`), so the GPU can read FCRAM.
     Then `IRQ_CMP = 0x12345678`, `IRQ_MASK = 0xFFFFFFF0` (low) and
     `0xFFFFFFFF` (high), and `AUTOSTOP = 1`. Last, `START_DRAW_FUNC0 = 1`;
     libn3ds notes the first list hangs the GPU without it.
   - Each frame:
     - clear colour and depth with both fill engines at once;
     - write the list's size and address to `CMDBUF_SIZE0` / `CMDBUF_ADDR0`
       and kick `CMDBUF_JUMP0`;
     - poll bit 31 of `0x10400034` (the finalize interrupt), bounded;
     - acknowledge;
     - display-transfer the tiled RGBA8 render target into a linear RGB8
       framebuffer.
   - Every wait is bounded. A failure is reported as `GPU_P3D_*` and the
     screen falls back to the software renderer.
3. **Presenting** reuses the normal path: the frame lands in the top
   backbuffer (and `0x26DB0000` for the right eye), then a present or
   `GPU_OP_PRESENT_ST` shows it at the vertical blank.

What a frame's list sets, in order, follows citro3d:

| Step | Main writes |
|------|-------------|
| Render target | `0x18100000` RGBA8 colour, `0x18180000` 24-bit depth + 8-bit stencil, 240x400 (the screen on its side), viewport, no scissor |
| Shader | code and operand descriptors uploaded, entry point, output map |
| Inputs | position (float4), texture coordinate (float2) and colour (float4) |
| Fragment stages | no lighting (it is baked into the vertex colours); stage 0 is the vertex colour, or texture 0 x vertex colour |
| Backdrop | depth test off, no texture; citro2d's screen-space projection; 64 triangles of the gradient sent in immediate mode (`FIXEDATTRIB_INDEX = 0xF`) |
| Model | depth test GREATER with the depth buffer cleared to 0; the eye's matrix; one vertex buffer, and per material and primitive the state and `DrawElements` described in [`glb.md`](glb.md) |
| End | flush the tile cache, invalidate, finalize |

The vertex shader (`model.v.pica`) is one 4x4 transform and copies of the
texture coordinate and colour. The Makefile runs picasso
(`$(DEVKITPRO)/tools/bin/picasso`), then `tools/shbin2c.py` turns the binary
into `build/model_shbin.h`: code, operand descriptors, output map, constants
and uniform registers. Picasso ships with devkitPro's 3DS tools.

## What has been checked

On the PC, with no PICA200 to run on:

* **The generated list, decoded** (one eye of the 3D Model screen):
  - it parses exactly to its end and is a multiple of 16 bytes;
  - its setup matches, value for value, a citro3d list that open_agb_firm runs
    on real consoles: framebuffer `0x0118F0F0`, viewport `0x45E000` /
    `0x38111112` / `0x469000` / `0x3747AE14`, depth map `0xBF0000`, colour
    operation `0xE40100`, blend `0x76760000`;
  - attribute formats `0xF7F` / `0x2FF80000` (float4, float2, float4) and
    output map `0x03020100` / `0x1F1F0D0C` / `0x0B0A0908`.
* **The shader binary** (the cube's, built the same way as the model's)
  matches GBATEK's hand-assembled example layout.
* **The GPU matrices against the software renderer:** every cube corner lands
  on the same pixel (0.0000 px apart), for 8 yaws x 5 pitches x 3 eye
  offsets. That uses the screen mapping citro2d's projection defines, which
  round-trips exactly.
* **The software stereo pair:** rendered and viewed as an anaglyph. Points
  beyond the convergence distance separate one way and nearer ones the
  other, as the projection intends.

On a New 3DS (the cube, before models): P3D lists run, the PDC 3D preset
works, and the barrier's fixed pattern gives depth, which also bears out the
expander's byte split. With models, on the same console: vertex buffers,
`DrawElements`, mipmapped textures and the alpha modes. Not checked: the Old
3DS barrier and LED.

## On the console

1. Open **3D Model**. The bottom screen should name the model. If it says
   `Software cube: GPU ...`, the reason after "GPU" is where it stopped
   (clear, command list or copy).
2. Raise the slider. The raw value should rise from near 0, and `3D on`
   should appear just above the bottom. The Old 3DS's 3D LED lights.
3. Look for depth. The nearest part of the model sits at the screen surface
   and the rest goes into the screen. With the slider at the bottom the
   screen goes back to 2D.
4. On a New 3DS, if the picture doubles rather than standing out, try X to
   step through the barrier positions. Note which one works, or whether none
   does: that points at the expander's byte split.

| Symptom | Likely cause |
|---------|--------------|
| Top screen black after raising the slider | 3D timing not accepted; the 2D values are restored when the slider goes down or B is pressed |
| A double image that does not resolve at any viewing angle | barrier off: on a New 3DS the expander; on an Old 3DS the barrier registers |
| Depth inverted (near looks far) | eyes swapped: the sign of the eye offset in `draw_eyes()` |
| Faint flicker on the barrier (New 3DS) | the alternation period; `EXP_PERIOD` in `Stereo11.c` |
| Both screens stop updating on opening 3D Model | the first command list wedged the GPU, so presents stop too; power off, and report the bottom screen's last line |

## Licence notes

`Stereo11.c`, `P3d11.c`, `Stereo9.c`, `P3d9.c` and `tools/shbin2c.py` are
Aurora's own code under GPL-3.0. They reimplement register facts from GBATEK
and 3dbrew; from libn3ds (GPL-3.0, the same licence); and from libctru,
citro3d and citro2d (zlib). No code was copied from any of them. The small
changes in `Gpu11.c` / `Gpu9.c` stay under those files' GPL-2.0.
