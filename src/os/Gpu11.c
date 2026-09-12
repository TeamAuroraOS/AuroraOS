/* SPDX-License-Identifier: GPL-2.0 */
/*
 * PICA200 GPU, ARM11 side. Core11.c dispatches here on AUDIO_CMD_GPU.
 * See docs/gpu.md.
 *
 * PSC fills memory and PPF copies it. Both are plain DMA engines, so unlike
 * P3D they need no command list or shader state, only a clock enable. Every
 * wait is bounded: a wedged engine must stall one operation rather than the
 * core, which still has a touchscreen to poll.
 *
 * LICENSE: GPL-2.0, not GPL-3.0 like the rest of AuroraOS. Derived from the
 * Linux Nintendo 3DS PICA200 driver (ctr_pica.c). See docs/gpu.md.
 */
#include "gpu.h"

typedef volatile uint32_t vu32;
#define GREG(off) (*(vu32 *)(GPU_REG_BASE + (off)))

/* --- PICA200 registers (offsets from GPU_REG_BASE) --- */
#define R_HW_ID        0x0000
#define R_CLOCK        0x0004
#define R_PSC0_START   0x0010
#define R_PSC0_END     0x0014
#define R_PSC0_VALUE   0x0018
#define R_PSC0_CNT     0x001C
#define R_PSC1_CNT     0x002C
#define R_BUSY         0x0034
#define R_PPF_INPUT    0x0C00
#define R_PPF_OUTPUT   0x0C04
#define R_PPF_DST_DIM  0x0C08
#define R_PPF_SRC_DIM  0x0C0C
#define R_PPF_FLAGS    0x0C10
#define R_PPF_UNK      0x0C14
#define R_PPF_CNT      0x0C18
#define R_PPF_LEN      0x0C20 /* texture-copy byte count */
#define R_IRQ_ACK      0x1000

#define CLOCK_ALL   0x00070100u /* enables the PSC/PPF/P3D sub-block clocks */
#define BUSY_P3D    (1u << 31)
#define PSC_CNT_GO  (1u << 0)   /* write to start; reads back as "busy"     */
#define PSC_CNT_DONE (1u << 1)  /* set by hardware on completion            */
#define PPF_CNT_GO   (1u << 0)
#define PPF_CNT_DONE (1u << 8)
#define PPF_FLAG_RAW_COPY (1u << 3) /* linear blit: no de-tiling, no conversion */

/* Bound on every completion poll. A full-screen fill takes well under a
 * millisecond, so this is a generous ceiling, not a working budget. */
#define GPU_POLL_MAX 200000u

static inline void dsb(void) {
  __asm__ volatile("mcr p15, 0, %0, c7, c10, 4" ::"r"(0) : "memory");
}
static inline void dcache_clean(void) {
  __asm__ volatile("mcr p15, 0, %0, c7, c10, 0" ::"r"(0) : "memory");
  dsb();
}
static inline void dcache_clean_inval(void) {
  __asm__ volatile("mcr p15, 0, %0, c7, c14, 0" ::"r"(0) : "memory");
  dsb();
}

static void gpu_spin(uint32_t n) {
  while (n--)
    __asm__ volatile("nop");
}

static uint32_t gpu_bpp(uint32_t fmt) {
  switch (fmt) {
    case GPU_FMT_RGBA8: return 4;
    case GPU_FMT_RGB8:  return 3;
    default:            return 2;
  }
}

/* P3D completion is acknowledged by writing zero to the IRQ-ack register; the
 * posted write is flushed with a status read so the level-high line deasserts. */
static void gpu_ack_p3d(void) {
  GREG(R_IRQ_ACK) = 0;
  dsb();
  (void)GREG(R_BUSY);
}

/* Enable the engine clocks, drop any state inherited from the firm, and read the
 * hardware ID. The ID read is the proof that the block is actually reachable. */
static void gpu_op_init(GpuShared *g) {
  g->busy_before = GREG(R_BUSY);

  GREG(R_CLOCK) = CLOCK_ALL;
  dsb();
  g->step = GPU_STEP_CLOCK;
  gpu_spin(300000); /* ~10 ms for the clocks to come up */

  /* Clear inherited completion/status bits so the first real op starts clean. */
  GREG(R_PPF_CNT) = GREG(R_PPF_CNT) & ~0x0000FF00u;
  GREG(R_PSC0_CNT) = GREG(R_PSC0_CNT) & ~0x000000FFu;
  GREG(R_PSC1_CNT) = GREG(R_PSC1_CNT) & ~0x000000FFu;
  dsb();
  g->step = GPU_STEP_QUIESCE;

  gpu_ack_p3d();
  uint32_t w = 0;
  while ((GREG(R_BUSY) & BUSY_P3D) && ++w < GPU_POLL_MAX)
    ;
  /* If inherited P3D work never drained, `waits` saturates and busy_before keeps
   * bit31, both visible on the test screen. It is not treated as an init
   * failure: PSC and PPF are independent engines and work regardless. */
  g->waits = w;
  g->step = GPU_STEP_IDLE;

  g->hw_id = GREG(R_HW_ID);
  g->busy_after = GREG(R_BUSY);
  g->step = GPU_STEP_ID;
  g->ready = 1;
}

/* PSC0 memory fill of [start, end). Addresses are programmed as physical >> 3,
 * so both ends must be 8-byte aligned. */
static void gpu_op_fill(GpuShared *g) {
  uint32_t start = g->fill_addr, end = g->fill_end;

  if ((start & 7u) || (end & 7u) || end <= start ||
      g->fill_width > GPU_FILL_32) {
    g->err = GPU_ERR_BADARG;
    return;
  }

  g->busy_before = GREG(R_BUSY);
  GREG(R_PSC0_CNT) = GREG(R_PSC0_CNT) & ~0x000000FFu; /* clear a stale done bit */
  GREG(R_PSC0_START) = start >> 3;
  GREG(R_PSC0_END) = end >> 3;
  GREG(R_PSC0_VALUE) = g->fill_value;
  dsb();
  g->step = GPU_STEP_ARMED;

  GREG(R_PSC0_CNT) = PSC_CNT_GO | (g->fill_width << 8);
  dsb();
  g->step = GPU_STEP_STARTED;

  uint32_t w = 0, cnt = 0;
  for (;;) {
    cnt = GREG(R_PSC0_CNT);
    if (cnt & PSC_CNT_DONE)
      break;
    if (!(cnt & PSC_CNT_GO)) /* some revisions just drop the go bit */
      break;
    if (++w >= GPU_POLL_MAX) {
      g->err = GPU_ERR_TIMEOUT;
      break;
    }
  }
  g->waits = w;
  g->ctl_after = cnt;
  GREG(R_PSC0_CNT) = cnt & ~0x000000FFu; /* acknowledge */
  dsb();
  g->busy_after = GREG(R_BUSY);
  if (!g->err)
    g->step = GPU_STEP_DONE;
}

/* Trigger a PPF operation whose registers are already programmed, then wait
 * (bounded) for completion and acknowledge it. Shared by both PPF modes. */
static void gpu_ppf_start_wait(GpuShared *g) {
  GREG(R_PPF_CNT) = PPF_CNT_GO;
  dsb();
  g->step = GPU_STEP_STARTED;

  uint32_t w = 0, cnt = 0;
  for (;;) {
    cnt = GREG(R_PPF_CNT);
    if (cnt & PPF_CNT_DONE)
      break;
    if (!(cnt & PPF_CNT_GO)) /* some revisions just drop the go bit */
      break;
    if (++w >= GPU_POLL_MAX) {
      g->err = GPU_ERR_TIMEOUT;
      break;
    }
  }
  g->waits = w;
  g->ctl_after = cnt;
  GREG(R_PPF_CNT) = cnt & ~PPF_CNT_DONE; /* acknowledge */
  dsb();
  g->busy_after = GREG(R_BUSY);
  if (!g->err)
    g->step = GPU_STEP_DONE;
}

/* PPF display transfer. The destination rectangle may be smaller than the source
 * (the engine downscales in that case) but never larger. */
static void gpu_op_transfer(GpuShared *g) {
  uint32_t sw = g->xf_src_w, sh = g->xf_src_h;
  uint32_t dw = g->xf_dst_w, dh = g->xf_dst_h;
  uint32_t sfmt = g->xf_src_fmt, dfmt = g->xf_dst_fmt;
  uint32_t sbpp, dbpp, align_s, align_d;

  if (sfmt > GPU_FMT_RGBA4 || dfmt > GPU_FMT_RGBA4) {
    g->err = GPU_ERR_BADARG;
    return;
  }
  sbpp = gpu_bpp(sfmt);
  dbpp = gpu_bpp(dfmt);
  /* A row must land on the engine's 8-byte burst; 24-bit rows need 16 bytes. */
  align_s = (sfmt == GPU_FMT_RGB8) ? 15u : 7u;
  align_d = (dfmt == GPU_FMT_RGB8) ? 15u : 7u;

  if ((g->xf_src & 7u) || (g->xf_dst & 7u) || sw < 64u || sh < 16u ||
      dw < 64u || dh < 16u || dw > sw || dh > sh ||
      ((sw * sbpp) & align_s) || ((dw * dbpp) & align_d)) {
    g->err = GPU_ERR_BADARG;
    return;
  }
  if ((g->xf_flags & GPU_XF_BLOCK32) && ((sw | sh | dw | dh) & 31u)) {
    g->err = GPU_ERR_BADARG;
    return;
  }

  uint32_t hwflags = (sfmt << 8) | (dfmt << 12);
  if (g->xf_flags & GPU_XF_FLIP_VERT)
    hwflags |= (1u << 0);
  if (g->xf_flags & GPU_XF_BLOCK32)
    hwflags |= (1u << 16);

  g->busy_before = GREG(R_BUSY);
  GREG(R_PPF_CNT) = GREG(R_PPF_CNT) & ~0x0000FF00u; /* clear a stale done bit */
  GREG(R_PPF_INPUT) = g->xf_src >> 3;
  GREG(R_PPF_OUTPUT) = g->xf_dst >> 3;
  GREG(R_PPF_DST_DIM) = dw | (dh << 16);
  GREG(R_PPF_SRC_DIM) = sw | (sh << 16);
  GREG(R_PPF_FLAGS) = hwflags;
  GREG(R_PPF_UNK) = 0;
  dsb();
  g->step = GPU_STEP_ARMED;

  gpu_ppf_start_wait(g);
}

/* PPF texture copy: a raw linear -> linear blit with no tiling or format
 * conversion. Flags bit3 selects this mode; the dimension registers become
 * line-width/gap descriptors, and zero in both means "one contiguous run", so
 * the byte count in R_PPF_LEN is the only size that matters. */
static void gpu_op_texcopy(GpuShared *g) {
  uint32_t len = g->xf_len;

  if ((g->xf_src & 7u) || (g->xf_dst & 7u) || len == 0u || (len & 15u)) {
    g->err = GPU_ERR_BADARG;
    return;
  }

  g->busy_before = GREG(R_BUSY);
  GREG(R_PPF_CNT) = GREG(R_PPF_CNT) & ~0x0000FF00u; /* clear a stale done bit */
  GREG(R_PPF_INPUT) = g->xf_src >> 3;
  GREG(R_PPF_OUTPUT) = g->xf_dst >> 3;
  GREG(R_PPF_DST_DIM) = 0;
  GREG(R_PPF_SRC_DIM) = 0;
  GREG(R_PPF_FLAGS) = PPF_FLAG_RAW_COPY;
  GREG(R_PPF_LEN) = len;
  dsb();
  g->step = GPU_STEP_ARMED;

  gpu_ppf_start_wait(g);
}

/* Entry point: called from the ARM11 command loop for AUDIO_CMD_GPU. */
void gpu11_run(void) {
  GpuShared *g = (GpuShared *)GPU_SHARED_ADDR;

  dcache_clean_inval(); /* pull the ARM9-written parameters from RAM */
  g->magic = GPU_MAGIC;
  g->step = GPU_STEP_NONE;
  g->err = GPU_ERR_NONE;
  g->waits = 0;

  uint32_t op = g->op;
  if (op != GPU_OP_INIT && !g->ready) {
    g->err = GPU_ERR_NOINIT;
  } else if (op == GPU_OP_INIT) {
    gpu_op_init(g);
  } else if (op == GPU_OP_FILL) {
    gpu_op_fill(g);
  } else if (op == GPU_OP_TRANSFER) {
    gpu_op_transfer(g);
  } else if (op == GPU_OP_TEXCOPY) {
    gpu_op_texcopy(g);
  } else {
    g->err = GPU_ERR_BADARG;
  }

  if (!g->err)
    g->ops++;
  g->seq++;
  dcache_clean();
}
