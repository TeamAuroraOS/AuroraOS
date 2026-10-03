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

#define GPU_REG_BASE 0x10400000u

/* Each panel's three framebuffers. Everything that draws to a panel directly
 * draws to A; the Home Menu's presents rotate through all three, the LCD
 * controller switching to each at a vertical blank (GPU_OP_PRESENT). */
#define GPU_FB_TOP_A 0x18300000u
#define GPU_FB_TOP_B 0x18400000u
#define GPU_FB_TOP_C 0x18500000u
#define GPU_FB_BOT_A 0x18346500u
#define GPU_FB_BOT_B 0x18446500u
#define GPU_FB_BOT_C 0x18546500u

/* The top panel's right-eye framebuffers, used while it is in stereo mode
 * (GPU_OP_STEREO). Below the left-eye ones, in VRAM nothing else uses. */
#define GPU_FB_TOP_RA 0x18200000u
#define GPU_FB_TOP_RB 0x18250000u
#define GPU_FB_TOP_RC 0x182A0000u /* -> 0x182E6500 */

/* PICA200 render target for GPU_OP_P3D: 240x400 (the top screen on its side),
 * RGBA8 colour and 24-bit depth + 8-bit stencil, 384,000 bytes each. */
#define GPU_P3D_COLOR 0x18100000u
#define GPU_P3D_DEPTH 0x18180000u
#define GPU_P3D_W     240u
#define GPU_P3D_H     400u

/* Pixel formats, as encoded in the PPF flags field (bits 8-11 in, 12-15 out). */
enum {
  GPU_FMT_RGBA8  = 0,
  GPU_FMT_RGB8   = 1, /* 24-bit, AuroraOS framebuffer format */
  GPU_FMT_RGB565 = 2,
  GPU_FMT_RGB5A1 = 3,
  GPU_FMT_RGBA4  = 4,
};

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
  GPU_OP_PRESENT  = 5, /* texture copy to a panel, synced as chosen below  */
  GPU_OP_SHOW_A   = 6, /* newest frame into framebuffer A, and show A       */
  GPU_OP_PRESENT2 = 7, /* both panels at once: xf_src top, xf_src2 bottom   */
  GPU_OP_VSYNC_TEST = 8, /* wait xf_len blanks of panel xf_flags by method  */
  GPU_OP_VSYNC_SET  = 9, /* sync panel xf_flags by method xf_src_w from now */
  GPU_OP_STEREO     = 10, /* top panel 2D or 3D: xf_flags GPU_STEREO_*,    */
                          /* xf_src_w the New 3DS barrier pattern          */
  GPU_OP_PRESENT_ST = 11, /* top left xf_src, top right xf_src2, and the   */
                          /* bottom xf_src3 unless it is 0                 */
  GPU_OP_P3D        = 12, /* clear the render target to fill_value, run the */
                          /* command list xf_src (xf_len bytes), then copy */
                          /* the result to xf_dst as a top-screen frame    */
};

/* GPU_OP_STEREO flags. The barrier is the Old 3DS one (LCD registers) on every
 * model with a 3D screen; the New 3DS adds its I2C-driven mask. */
#define GPU_STEREO_ON       0x01u
#define GPU_STEREO_BARRIER  0x02u /* the LCD parallax registers            */
#define GPU_STEREO_EXPANDER 0x04u /* New 3DS: the mask expander as well    */

/* How a panel's presents are synchronised with its vertical blank; see
 * Gpu11.c. */
enum {
  GPU_VSYNC_OFF = 0, /* copied straight into framebuffer A                */
  GPU_VSYNC_IRQ = 1, /* the controller's vblank interrupt flag             */
  GPU_VSYNC_GIC = 2, /* that interrupt, pending in the ARM11's distributor */
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

  volatile uint32_t xf_src2;    /* GPU_OP_PRESENT2: the bottom panel's frame  */
  volatile uint32_t xf_src3;    /* GPU_OP_PRESENT_ST: the bottom panel's frame */
  volatile uint32_t pad[4];

  /* Written only by the ARM11, in a cache line of their own: an ARM9 clean of
   * the parameters above must not write stale copies back over them. */
  volatile uint32_t front[2];   /* top, bottom: framebuffer with the newest frame */
  volatile uint32_t vsync;      /* GPU_VSYNC_* per panel, 2 bits, top first       */
  volatile uint32_t vs_swap[2]; /* swap and control registers as the */
  volatile uint32_t vs_cnt[2];  /* last test found them */
  volatile uint32_t vs_presents[2]; /* presents since the method was set, and */
  volatile uint32_t vs_missed[2];   /* how many found no blank to wait for    */
  volatile uint32_t stereo;     /* GPU_STEREO_* in force, bit 8: expander answered */
  volatile uint32_t exp_writes; /* New 3DS barrier pattern writes (it alternates) */
  volatile uint32_t p3d_state;  /* GPU_P3D_* */
  volatile uint32_t p3d_runs;   /* command lists that finished                 */
  volatile uint32_t p3d_waits;  /* polls the last one took                     */
  volatile uint32_t p3d_stat;   /* 0x10400034 when the last one ended          */
} GpuShared;

enum {
  GPU_P3D_NONE = 0, /* never run                                   */
  GPU_P3D_OK,
  GPU_P3D_CLEAR_TIMEOUT,
  GPU_P3D_LIST_TIMEOUT, /* the list never raised its finalize interrupt */
  GPU_P3D_COPY_TIMEOUT,
};

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

/* PPF texture copy: a raw linear -> linear blit of `len` bytes (a multiple of
 * 16), with no tiling or format conversion. */
int gpu_texcopy(uint32_t src, uint32_t dst, uint32_t len);

/* Post a blit without waiting for it. The copy overlaps whatever the CPU does
 * next; call gpu_wait_idle() before drawing into the source buffer again. */
int gpu_texcopy_async(uint32_t src, uint32_t dst, uint32_t len);
void gpu_wait_idle(void);

/* The OS's present: like gpu_texcopy_async to a panel's framebuffer A, but
 * shown at the panel's vertical blank when anim_vsync_setup() found a way to
 * wait for one, so it does not tear. */
int gpu_present_async(uint32_t src, uint32_t dst, uint32_t len);

/* Both panels in one operation, so waiting for two blanks costs one frame. */
int gpu_present2_async(uint32_t top_src, uint32_t bot_src);

/* Waits `blanks` vertical blanks of screen 0 or 1 by a GPU_VSYNC_* method;
 * 0 if one never came. The caller times it to see whether the method is real
 * (anim_vsync_setup). */
int gpu_vsync_test(int screen, uint32_t method, uint32_t blanks);

int gpu_vsync_set(int screen, uint32_t method);

/* GpuShared.vsync, the swap and control registers the last tests read, and each
 * panel's presents and missed blanks. */
void gpu_vsync_info(uint32_t *vsync, uint32_t *swap, uint32_t *cnt,
                    uint32_t *presents, uint32_t *missed);

/* The framebuffer holding the newest frame of screen 0 (top) or 1 (bottom). */
uint32_t gpu_front(int screen);

/* Framebuffer A back on screen with the newest frame, before anything that
 * draws to the panel directly, such as an app. Presents carry on rotating from
 * there. */
int gpu_show_a(void);

/* Bit 0 top, bit 1 bottom: panels whose presents wait for the display. */
uint32_t gpu_vsynced(void);

/* GPU_OP_STEREO: `flags` GPU_STEREO_*, `pattern` the New 3DS barrier mask
 * (bits 0-11, leftmost unit in bit 11). */
int gpu_stereo(uint32_t flags, uint32_t pattern);

/* Both eyes of the top panel and, unless `bot` is 0, the bottom panel, shown
 * together. Returns without waiting, like gpu_present_async(). */
int gpu_present_st_async(uint32_t left, uint32_t right, uint32_t bot);

/* Runs a PICA200 command list on a cleared render target and copies the frame
 * to `dst` (400x240 RGB8, the framebuffer layout). Blocks. */
int gpu_p3d(uint32_t list, uint32_t bytes, uint32_t dst, uint32_t clear_rgba);

/* The ARM11's view of stereo and P3D, for diagnostics. */
void gpu_3d_info(uint32_t *stereo, uint32_t *exp_writes, uint32_t *p3d_state,
                 uint32_t *p3d_runs, uint32_t *p3d_waits, uint32_t *p3d_stat);

/* Why the last operation that failed did: a GPU_ERR_* code, or
 * GPU_FAIL_NO_ANSWER when the core did not answer in time. */
#define GPU_FAIL_NO_ANSWER 0x100u
uint32_t gpu_last_fail(void);

/* Read the shared block back (invalidates the ARM9 cache first). */
void gpu_get(GpuShared *out);

#endif
