/* SPDX-License-Identifier: GPL-2.0 */
/* PICA200 GPU, ARM9 side: posts operations to Gpu11.c and blocks, bounded,
 * until the ARM11 bumps the sequence counter.
 *
 * LICENSE: GPL-2.0, not GPL-3.0 like the rest of AuroraOS. See docs/gpu.md. */
#include "aurora.h"
#include "audio.h"
#include "gpu.h"

extern void os_cache_sync(void);
/* Clean without invalidating: the GPU needs the pixels written back, and
 * emptying the I-cache on every operation leaves the drawing code running cold. */
extern void os_dcache_clean(void);
extern void os_dcache_clean_range(const void *addr, u32 len);

/* One line, so a poll sees the ARM11's latest write without flushing the whole
 * cache. */
static inline void inval_line(const volatile void *p) {
  __asm__ volatile("mcr p15, 0, %0, c7, c6, 1" ::"r"(p) : "memory");
}

static AudioCtrl *const ctrl = (AudioCtrl *)AUDIO_CTRL_ADDR;
static GpuShared *const gs = (GpuShared *)GPU_SHARED_ADDR;

int gpu_alive(void) {
  inval_line(&gs->magic);
  return gs->magic == GPU_MAGIC;
}

void gpu_get(GpuShared *out) {
  os_cache_sync();
  volatile unsigned char *s = (volatile unsigned char *)GPU_SHARED_ADDR;
  unsigned char *d = (unsigned char *)out;
  for (unsigned i = 0; i < sizeof(GpuShared); i++)
    d[i] = s[i];
}

/* Sequence number of the operation posted but not yet waited on, and whether
 * one is outstanding at all. */
static u32 pending_seq;
static int pending;

/* The parameters must reach RAM before the counter, or the ARM11 can act on a
 * stale op. A whole-cache clean writes lines in index order, so the two are
 * cleaned separately. */
static void gpu_post(void) {
  os_dcache_clean();                            /* pixels and gs parameters */
  /* seq is written by the ARM11; without invalidating its line, a stale value
   * passes the first completion check. */
  inval_line(&gs->seq);
  pending_seq = gs->seq;

  ctrl->cmd = AUDIO_CMD_GPU;
  ctrl->cmd_seq = ctrl->cmd_seq + 1;
  os_dcache_clean_range(ctrl, sizeof(*ctrl));   /* then the command itself */
  pending = 1;
}

/* Spins rather than sleeps: the engines finish in tens of microseconds. The
 * bound only stops a dead core from hanging the OS. */
static int gpu_collect(void) {
  if (!pending)
    return 1;
  for (u32 spin = 0; spin < 4000000u; spin++) {
    inval_line(&gs->seq);
    if (gs->seq != pending_seq) {
      pending = 0;
      return gs->err == GPU_ERR_NONE;
    }
  }
  pending = 0;
  return 0;
}

void gpu_wait_idle(void) { (void)gpu_collect(); }

static int gpu_run(void) {
  gpu_collect(); /* only one operation fits in the shared block */
  gpu_post();
  return gpu_collect();
}

int gpu_init(void) {
  os_cache_sync();
  /* FCRAM holds garbage on a cold boot and survives a warm one, so the whole
   * block is cleared, including `ready`, which gates every other op. */
  volatile unsigned char *d = (volatile unsigned char *)GPU_SHARED_ADDR;
  for (unsigned i = 0; i < sizeof(GpuShared); i++)
    d[i] = 0;
  gs->op = GPU_OP_INIT;
  return gpu_run();
}

int gpu_fill(u32 addr, u32 end, u32 value, u32 width) {
  os_cache_sync();
  gs->op = GPU_OP_FILL;
  gs->fill_addr = addr;
  gs->fill_end = end;
  gs->fill_value = value;
  gs->fill_width = width;
  return gpu_run();
}

/* The framebuffers are 24-bit RGB8, so a fill pattern is the colour repeated at
 * the 24-bit fill width. `rgb24` is packed the way the framebuffer stores it. */
int gpu_clear_fb(u32 fb_addr, u32 fb_size, u32 rgb24) {
  return gpu_fill(fb_addr, fb_addr + fb_size, rgb24 & 0x00FFFFFFu, GPU_FILL_24);
}

int gpu_texcopy(u32 src, u32 dst, u32 len) {
  gs->op = GPU_OP_TEXCOPY;
  gs->xf_src = src;
  gs->xf_dst = dst;
  gs->xf_len = len;
  return gpu_run();
}

/* Returns without waiting. Draw into the source buffer again only after
 * gpu_wait_idle(). */
int gpu_texcopy_async(u32 src, u32 dst, u32 len) {
  if (!gpu_alive())
    return 0;
  gpu_collect(); /* the shared block holds one operation at a time */
  gs->op = GPU_OP_TEXCOPY;
  gs->xf_src = src;
  gs->xf_dst = dst;
  gs->xf_len = len;
  gpu_post();
  return 1;
}

int gpu_transfer(u32 src, u32 dst, u32 src_w, u32 src_h, u32 dst_w, u32 dst_h,
                 u32 src_fmt, u32 dst_fmt, u32 flags) {
  os_cache_sync();
  gs->op = GPU_OP_TRANSFER;
  gs->xf_src = src;
  gs->xf_dst = dst;
  gs->xf_src_w = src_w;
  gs->xf_src_h = src_h;
  gs->xf_dst_w = dst_w;
  gs->xf_dst_h = dst_h;
  gs->xf_src_fmt = src_fmt;
  gs->xf_dst_fmt = dst_fmt;
  gs->xf_flags = flags;
  return gpu_run();
}
