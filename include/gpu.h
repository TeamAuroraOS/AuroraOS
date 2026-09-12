/* SPDX-License-Identifier: GPL-2.0 */
/*
 * AuroraOS PICA200 GPU driver (ARM9 <-> ARM11 contract).
 *
 * The 3DS GPU (PICA200) lives at 0x10400000, which is ARM11 I/O; the ARM9 that
 * runs the OS cannot reach it. So the GPU is driven from the ARM11 core
 * (src/os/Gpu11.c), which the ARM9 already starts for audio; GPU work is posted
 * through the same command block and answered in the shared block below.
 *
 * Two engines are implemented, both of which are pure memory movers and need no
 * shader/vertex setup:
 *   PSC (0x10400010..0x1040002C): memory fill, used for hardware screen clears.
 *   PPF (0x10400C00..0x10400C18): "display transfer": copies a rectangle with
 *                                   optional pixel-format conversion and v-flip.
 * The 3D pipeline (P3D command lists) is not implemented; see docs/gpu.md.
 *
 * LICENSE: This GPU code is licensed GPL-2.0 (not GPL-3.0 like the rest of
 * AuroraOS). Its register map and the init / fill / transfer sequences derive
 * from the Linux Nintendo 3DS PICA200 driver (ctr_pica.c, GPL-2.0). Because
 * GPL-2.0 is copyleft and incompatible with GPL-3.0, these files keep the
 * original licence. See docs/gpu.md "License and credits".
 */
#ifndef AURORA_GPU_H
#define AURORA_GPU_H

#include <stdint.h>

#define GPU_SHARED_ADDR 0x233C0000u
#define GPU_MAGIC       0x55504733u /* "3GPU": the ARM11 GPU code has run */

/* PICA200 register block (ARM11 I/O). The LCD sub-block at +0x400/+0x500 is the
 * one aurora.h already uses, so this range is known-live on hardware. */
#define GPU_REG_BASE 0x10400000u

/* Pixel formats, as encoded in the PPF flags field (bits 8-11 in, 12-15 out). */
enum {
  GPU_FMT_RGBA8  = 0, /* 32-bit */
  GPU_FMT_RGB8   = 1, /* 24-bit, AuroraOS framebuffer format */
  GPU_FMT_RGB565 = 2,
  GPU_FMT_RGB5A1 = 3,
  GPU_FMT_RGBA4  = 4,
};

/* PPF transfer flags (AuroraOS-level; mapped onto the hardware flag bits). */
#define GPU_XF_FLIP_VERT 0x0001u /* hardware flags bit0  */
#define GPU_XF_BLOCK32   0x0002u /* hardware flags bit16 */

/* PSC fill width: how many bits the fill value covers per store. */
enum {
  GPU_FILL_16 = 0,
  GPU_FILL_24 = 1, /* matches the 24-bit RGB8 framebuffer */
  GPU_FILL_32 = 2,
};

/* Operation the ARM9 wants, written to GpuShared.op before posting AUDIO_CMD_GPU. */
enum {
  GPU_OP_NONE     = 0,
  GPU_OP_INIT     = 1, /* enable clocks, quiesce the engines, read the hardware ID */
  GPU_OP_FILL     = 2, /* PSC memory fill  */
  GPU_OP_TRANSFER = 3, /* PPF display transfer (de-tiles + converts format) */
  GPU_OP_TEXCOPY  = 4, /* PPF texture copy (raw linear -> linear blit)      */
};

/* How far the last operation got, so a hang localises to the last step set. */
enum {
  GPU_STEP_NONE = 0,
  GPU_STEP_CLOCK,    /* wrote the clock-enable register     */
  GPU_STEP_QUIESCE,  /* cleared inherited PSC/PPF state     */
  GPU_STEP_IDLE,     /* P3D reported idle                   */
  GPU_STEP_ID,       /* read the hardware ID                */
  GPU_STEP_ARMED,    /* engine registers programmed         */
  GPU_STEP_STARTED,  /* trigger written                     */
  GPU_STEP_DONE,     /* engine reported completion          */
};

enum {
  GPU_ERR_NONE = 0,
  GPU_ERR_NOINIT,   /* an op was posted before a successful GPU_OP_INIT */
  GPU_ERR_BADARG,   /* parameters failed validation                     */
  GPU_ERR_TIMEOUT,  /* the engine never reported completion             */
  GPU_ERR_BUSY,     /* the engine was still busy when the op started    */
};

typedef struct {
  volatile uint32_t magic; /* GPU_MAGIC once the ARM11 GPU code has run */
  volatile uint32_t seq;   /* bumped after every completed operation    */
  volatile uint32_t step;  /* GPU_STEP_* reached by the last operation  */
  volatile uint32_t err;   /* GPU_ERR_* of the last operation           */
  volatile uint32_t ready; /* 1 once GPU_OP_INIT has succeeded          */

  volatile uint32_t hw_id;      /* PICA hardware ID (reg 0x0000) */
  volatile uint32_t busy_before;/* BUSY (0x0034) sampled before the op */
  volatile uint32_t busy_after; /* BUSY (0x0034) sampled after the op  */
  volatile uint32_t ctl_after;  /* engine control register after the op */
  volatile uint32_t waits;      /* poll iterations the last op consumed */
  volatile uint32_t ops;        /* successful operations since boot     */

  /* Parameters: the ARM9 fills these in before posting AUDIO_CMD_GPU. */
  volatile uint32_t op;         /* GPU_OP_*                                  */
  volatile uint32_t fill_addr;  /* fill start (byte address, 8-byte aligned)  */
  volatile uint32_t fill_end;   /* fill end, exclusive                        */
  volatile uint32_t fill_value; /* pattern                                    */
  volatile uint32_t fill_width; /* GPU_FILL_*                                 */
  volatile uint32_t xf_src;     /* transfer source (8-byte aligned)           */
  volatile uint32_t xf_dst;     /* transfer destination (8-byte aligned)      */
  volatile uint32_t xf_src_w;   /* source width in pixels                     */
  volatile uint32_t xf_src_h;   /* source height in pixels                    */
  volatile uint32_t xf_dst_w;   /* destination width (<= source width)        */
  volatile uint32_t xf_dst_h;   /* destination height (<= source height)      */
  volatile uint32_t xf_src_fmt; /* GPU_FMT_*                                  */
  volatile uint32_t xf_dst_fmt; /* GPU_FMT_*                                  */
  volatile uint32_t xf_flags;   /* GPU_XF_*                                   */
  volatile uint32_t xf_len;     /* texture-copy length in bytes (16-aligned)  */
} GpuShared;

/* ARM9-side API (src/os/Gpu9.c). Each call posts the operation to the ARM11 and
 * blocks (bounded) until it completes, then returns 1 on success, 0 on failure.
 * The audio core must already be running (audio_boot()). */

/* Enable the GPU clocks and read the hardware ID. Required before any other op. */
int gpu_init(void);

/* 1 if the ARM11 GPU code has run at least once. */
int gpu_alive(void);

/* PSC hardware fill of [addr, end) with `value` at the given GPU_FILL_* width. */
int gpu_fill(uint32_t addr, uint32_t end, uint32_t value, uint32_t width);

/* Convenience: clear a framebuffer to a 24-bit colour using the PSC engine. */
int gpu_clear_fb(uint32_t fb_addr, uint32_t fb_size, uint32_t rgb24);

/* PPF display transfer: copy src -> dst, converting format and optionally
 * flipping vertically. Dimensions are in pixels; dst must not exceed src.
 * The engine treats the source as tiled GPU render output, so this is the right
 * call for presenting 3D output, use gpu_texcopy() to blit linear buffers. */
int gpu_transfer(uint32_t src, uint32_t dst, uint32_t src_w, uint32_t src_h,
                 uint32_t dst_w, uint32_t dst_h, uint32_t src_fmt,
                 uint32_t dst_fmt, uint32_t flags);

/* PPF texture copy: a raw linear -> linear blit of `len` bytes (multiple of 16),
 * with no tiling or format conversion. This is the framebuffer blit primitive. */
int gpu_texcopy(uint32_t src, uint32_t dst, uint32_t len);

/* Read the shared block back (invalidates the ARM9 cache first). */
void gpu_get(GpuShared *out);

#endif /* AURORA_GPU_H */
