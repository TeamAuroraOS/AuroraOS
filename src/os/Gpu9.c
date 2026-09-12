/* SPDX-License-Identifier: GPL-2.0 */
/*
 * PICA200 GPU, ARM9 side. The registers are ARM11 I/O, so every operation is
 * posted to Gpu11.c and answered in GpuShared. Each call blocks, bounded,
 * until the ARM11 bumps the sequence counter.
 *
 * LICENSE: GPL-2.0, not GPL-3.0 like the rest of AuroraOS. See docs/gpu.md.
 */
#include "aurora.h"
#include "audio.h"
#include "gpu.h"

extern void os_cache_sync(void);

/* Invalidate one cache line so a poll sees the ARM11's latest write. A full
 * clean+invalidate would work but costs the whole 4 KB cache, and this runs in
 * the completion spin. */
static inline void inval_line(const volatile void *p) {
  __asm__ volatile("mcr p15, 0, %0, c7, c6, 1" ::"r"(p) : "memory");
}

static AudioCtrl *const ctrl = (AudioCtrl *)AUDIO_CTRL_ADDR;
static GpuShared *const gs = (GpuShared *)GPU_SHARED_ADDR;

int gpu_alive(void) {
  os_cache_sync();
  return gs->magic == GPU_MAGIC;
}

void gpu_get(GpuShared *out) {
  os_cache_sync();
  volatile unsigned char *s = (volatile unsigned char *)GPU_SHARED_ADDR;
  unsigned char *d = (unsigned char *)out;
  for (unsigned i = 0; i < sizeof(GpuShared); i++)
    d[i] = s[i];
}

/* Post the operation already staged in the shared block and wait for the ARM11
 * to finish it. Returns 1 if the operation completed without an error code. */
static int gpu_run(void) {
  os_cache_sync();
  u32 last = gs->seq;

  ctrl->cmd = AUDIO_CMD_GPU;
  ctrl->cmd_seq = ctrl->cmd_seq + 1;
  os_cache_sync(); /* publish the command + parameters to the ARM11 */

  /* Spin on the sequence counter rather than sleeping between checks. The
   * engines finish in tens of microseconds, so a delay here set the floor on
   * what a GPU operation costs and made small ones pointless to offload. The
   * bound only exists so a wedged GPU or a dead ARM11 core cannot hang the OS. */
  for (u32 spin = 0; spin < 4000000u; spin++) {
    inval_line(&gs->seq);
    if (gs->seq != last)
      return gs->err == GPU_ERR_NONE;
  }
  return 0;
}

int gpu_init(void) {
  os_cache_sync();
  /* FCRAM survives a warm reboot and holds garbage on a cold one, so clear the
   * whole block here rather than trusting any field, in particular `ready`,
   * which gates every other operation. gpu_init() is required first, so this is
   * the one place that can safely wipe it. */
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
  os_cache_sync();
  gs->op = GPU_OP_TEXCOPY;
  gs->xf_src = src;
  gs->xf_dst = dst;
  gs->xf_len = len;
  return gpu_run();
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
