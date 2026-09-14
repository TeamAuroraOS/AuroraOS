/* SPDX-License-Identifier: GPL-2.0 */
/* PICA200 GPU driver contract. The GPU is ARM11 I/O, so work is posted to the
 * ARM11 core (src/os/Gpu11.c) and answered in GpuShared. Only the PSC fill and
 * PPF transfer engines are implemented; see docs/gpu.md.
 *
 * LICENSE: This GPU code is licensed GPL-2.0 (not GPL-3.0 like the rest of
 * AuroraOS). Its register map and the init / fill / transfer sequences derive
 * from the Linux Nintendo 3DS PICA200 driver (ctr_pica.c, GPL-2.0). Because
 * GPL-2.0 is copyleft and incompatible with GPL-3.0, these files keep the
 * original licence. See docs/gpu.md "License and credits". */
#ifndef AURORA_GPU_H
#define AURORA_GPU_H

#include <stdint.h>

#define GPU_SHARED_ADDR 0x233C0000u
#define GPU_MAGIC       0x55504733u /* "3GPU": the ARM11 GPU code has run */

/* PICA200 register block (ARM11 I/O). */
#define GPU_REG_BASE 0x10400000u

/* Pixel formats, as encoded in the PPF flags field (bits 8-11 in, 12-15 out). */
enum {
  GPU_FMT_RGBA8  = 0,
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

/* Operation the ARM9 wants, written to GpuShared.op before posting
 * AUDIO_CMD_GPU. */
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
  GPU_STEP_CLOCK,
  GPU_STEP_QUIESCE,  /* cleared inherited PSC/PPF state     */
  GPU_STEP_IDLE,     /* P3D reported idle                   */
  GPU_STEP_ID,
  GPU_STEP_ARMED,
  GPU_STEP_STARTED,
  GPU_STEP_DONE,
};

enum {
  GPU_ERR_NONE = 0,
  GPU_ERR_NOINIT,   /* an op was posted before a successful GPU_OP_INIT */
  GPU_ERR_BADARG,
  GPU_ERR_TIMEOUT,
  GPU_ERR_BUSY,
};

typedef struct {
  volatile uint32_t magic;
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

  /* Parameters, set by the ARM9 before posting AUDIO_CMD_GPU. `op` shares a
   * cache line with ARM11-written counters, so an ARM9 clean can write back
   * stale ctl_after/waits/ops; seq and err are in a line the ARM9 never writes. */
  volatile uint32_t op;         /* GPU_OP_*                                  */
  volatile uint32_t fill_addr;  /* fill start (byte address, 8-byte aligned)  */
  volatile uint32_t fill_end;   /* fill end, exclusive                        */
  volatile uint32_t fill_value;
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

/* ARM9 API (src/os/Gpu9.c). Each call blocks (bounded) until the ARM11 answers
 * and returns 1 on success. Needs the ARM11 core running (audio_boot()). */

/* Required before any other op. */
int gpu_init(void);

/* 1 if the ARM11 GPU code has run at least once. */
int gpu_alive(void);

/* PSC hardware fill of [addr, end) with `value` at the given GPU_FILL_* width. */
int gpu_fill(uint32_t addr, uint32_t end, uint32_t value, uint32_t width);

int gpu_clear_fb(uint32_t fb_addr, uint32_t fb_size, uint32_t rgb24);

/* PPF display transfer: copy src -> dst, converting format and optionally
 * flipping vertically. Dimensions are in pixels; dst must not exceed src.
 * The engine treats the source as tiled GPU render output, so this is the right
 * call for presenting 3D output, use gpu_texcopy() to blit linear buffers. */
int gpu_transfer(uint32_t src, uint32_t dst, uint32_t src_w, uint32_t src_h,
                 uint32_t dst_w, uint32_t dst_h, uint32_t src_fmt,
                 uint32_t dst_fmt, uint32_t flags);

/* PPF texture copy: a raw linear -> linear blit of `len` bytes (multiple of
 * 16), with no tiling or format conversion. This is the framebuffer blit
 * primitive. */
int gpu_texcopy(uint32_t src, uint32_t dst, uint32_t len);

/* Post a blit without waiting for it. The copy overlaps whatever the CPU does
 * next; call gpu_wait_idle() before drawing into the source buffer again. */
int gpu_texcopy_async(uint32_t src, uint32_t dst, uint32_t len);
void gpu_wait_idle(void);

/* Read the shared block back (invalidates the ARM9 cache first). */
void gpu_get(GpuShared *out);

#endif
