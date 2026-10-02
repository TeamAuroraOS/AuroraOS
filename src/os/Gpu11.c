/* SPDX-License-Identifier: GPL-2.0 */
/* PICA200 GPU, ARM11 side, dispatched from Core11.c on AUDIO_CMD_GPU. PSC and
 * PPF are plain DMA engines that need only a clock enable. Every wait is
 * bounded, so a wedged engine stalls one operation rather than the core.
 *
 * LICENSE: GPL-2.0, not GPL-3.0 like the rest of AuroraOS. Derived from the
 * Linux Nintendo 3DS PICA200 driver (ctr_pica.c). See docs/gpu.md. */
#include "gpu.h"

void sleep_ms(uint32_t ms);

typedef volatile uint32_t vu32;
#define GREG(off) (*(vu32 *)(GPU_REG_BASE + (off)))

/* PICA200 registers, as offsets from GPU_REG_BASE. */
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

/* Bound on every completion poll, counted in passes and sized for the 804 MHz
 * clock, where a pass is a third as long. A full-screen fill takes well under a
 * millisecond. */
#define GPU_POLL_MAX 600000u

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

/* Enables the engine clocks, clears state inherited from the firm and reads the
 * hardware ID, which proves the block is reachable. */
static void gpu_op_init(GpuShared *g) {
  g->busy_before = GREG(R_BUSY);

  GREG(R_CLOCK) = CLOCK_ALL;
  dsb();
  g->step = GPU_STEP_CLOCK;
  sleep_ms(10); /* for the clocks to come up */

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
  /* If inherited P3D work never drained, `waits` saturates and busy_before
   * keeps bit31, both visible on the test screen. It is not treated as an init
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

/* PPF display transfer. The destination rectangle may be smaller than the
 * source (the engine downscales in that case) but never larger. */
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
static void ppf_copy(GpuShared *g, uint32_t src, uint32_t dst, uint32_t len) {
  if ((src & 7u) || (dst & 7u) || len == 0u || (len & 15u)) {
    g->err = GPU_ERR_BADARG;
    return;
  }

  g->busy_before = GREG(R_BUSY);
  GREG(R_PPF_CNT) = GREG(R_PPF_CNT) & ~0x0000FF00u; /* clear a stale done bit */
  GREG(R_PPF_INPUT) = src >> 3;
  GREG(R_PPF_OUTPUT) = dst >> 3;
  GREG(R_PPF_DST_DIM) = 0;
  GREG(R_PPF_SRC_DIM) = 0;
  GREG(R_PPF_FLAGS) = PPF_FLAG_RAW_COPY;
  GREG(R_PPF_LEN) = len;
  dsb();
  g->step = GPU_STEP_ARMED;

  gpu_ppf_start_wait(g);
}

static void gpu_op_texcopy(GpuShared *g) {
  ppf_copy(g, g->xf_src, g->xf_dst, g->xf_len);
}

/* Vertical sync. Each panel's LCD controller (PDC) raises an interrupt at
 * every vertical blank, seen either as a flag in its swap register or as
 * pending in the ARM11's interrupt distributor. Which of these a console
 * answers is measured at boot: the ARM9 times a few blanks with each
 * (GPU_OP_VSYNC_TEST) and keeps one that runs at the display's rate
 * (GPU_OP_VSYNC_SET).
 *
 * A panel with a method is triple buffered. A present copies the frame into
 * the framebuffer neither shown nor shown before it, waits for a blank and
 * then points the controller at it: both address slots, so the slot select
 * does not matter, and the select toggled, in case the controller only reloads
 * an address when it changes. Whether the controller takes a new address at
 * once, at the next frame or a blank later, the framebuffer being written is
 * never one it can still be scanning. A panel without a method gets each
 * frame copied straight into A.
 *
 * Register layout as Luma3DS and libn3ds program it. */
#define PDC_TOP    0x10400400u
#define PDC_BOT    0x10400500u
#define PDC_FB_A   0x68u /* address slot 0; the top screen's left eye */
#define PDC_FB_B   0x6Cu /* address slot 1 */
#define PDC_CNT    0x74u
#define PDC_FB_A_R 0x94u /* the top screen's right eye */
#define PDC_FB_B_R 0x98u
#define PDC_SWAP   0x78u
#define SWAP_SEL   (1u << 0)  /* the address slot to scan */
#define SWAP_VBLK  (1u << 17) /* vblank interrupt flag, write 1 to clear */
#define SWAP_ACK   (7u << 16) /* clears all three interrupt flags */

#define GICD_CTRL    0x17E01000u
#define GICD_SETPEND 0x17E01200u
#define GICD_CLRPEND 0x17E01280u
#define IRQ_PDC0     42u /* PDC1, the bottom panel, is 43 */

/* Far more than a frame of polling at any clock. A wait only runs out when a
 * method does not work. */
#define BLANK_POLLS 2000000u

typedef struct {
  uint32_t pdc, len;
  uint32_t fb[3]; /* A, B, C */
} Panel;

static const Panel panels[2] = {
    {PDC_TOP, 400u * 240u * 3u, {GPU_FB_TOP_A, GPU_FB_TOP_B, GPU_FB_TOP_C}},
    {PDC_BOT, 320u * 240u * 3u, {GPU_FB_BOT_A, GPU_FB_BOT_B, GPU_FB_BOT_C}},
};

static uint32_t mode[2];              /* GPU_VSYNC_* */
static int shown[2], before[2];       /* framebuffers, 0 A to 2 C */
static uint32_t presents[2], missed[2]; /* for the GPU Test screen */

static vu32 *pdc_reg(const Panel *p, uint32_t off) {
  return (vu32 *)(p->pdc + off);
}

static uint32_t gic_bit(int i) { return 1u << ((IRQ_PDC0 + (uint32_t)i) % 32u); }
static uint32_t gic_word(int i) { return 4u * ((IRQ_PDC0 + (uint32_t)i) / 32u); }

/* Clears whatever says a blank has passed, so the next one can be seen. */
static void blank_arm(int i, uint32_t m) {
  vu32 *swap = pdc_reg(&panels[i], PDC_SWAP);
  *swap = (*swap & SWAP_SEL) | SWAP_ACK;
  if (m == GPU_VSYNC_GIC)
    *(vu32 *)(GICD_CLRPEND + gic_word(i)) = gic_bit(i);
  dsb();
}

static int blank_seen(int i, uint32_t m) {
  if (m == GPU_VSYNC_GIC)
    return (*(vu32 *)(GICD_SETPEND + gic_word(i)) & gic_bit(i)) != 0;
  return (*pdc_reg(&panels[i], PDC_SWAP) & SWAP_VBLK) != 0;
}

static int blank_wait(int i, uint32_t m) {
  blank_arm(i, m);
  for (uint32_t w = 0; w < BLANK_POLLS; w++)
    if (blank_seen(i, m))
      return 1;
  return 0;
}

static void set_front(GpuShared *g, int i) {
  g->front[i] = panels[i].fb[shown[i]];
  g->vsync = (g->vsync & ~(3u << (2 * i))) | (mode[i] << (2 * i));
  g->vs_presents[i] = presents[i];
  g->vs_missed[i] = missed[i];
}

static void show(int i, int f) {
  const Panel *p = &panels[i];
  uint32_t addr = p->fb[f];
  vu32 *swap = pdc_reg(p, PDC_SWAP);
  *pdc_reg(p, PDC_FB_A) = addr;
  *pdc_reg(p, PDC_FB_B) = addr;
  if (p->pdc == PDC_TOP) {
    *pdc_reg(p, PDC_FB_A_R) = addr;
    *pdc_reg(p, PDC_FB_B_R) = addr;
  }
  *swap = ((*swap & SWAP_SEL) ^ SWAP_SEL) | SWAP_ACK;
  dsb();
  if (f != shown[i])
    before[i] = shown[i];
  shown[i] = f;
}

/* The framebuffer a present may write: neither the one shown nor the one shown
 * before it. */
static int next_fb(int i) {
  if (shown[i] == before[i])
    return (shown[i] + 1) % 3;
  return 3 - shown[i] - before[i];
}

/* Moves the newest frame into A and shows it, for anything that draws to the
 * panel directly: an app, the crash screen, another core. The method stays. A
 * switch the controller takes a blank late could leave A scanned for a frame,
 * hence the wait. */
static void panel_to_a(GpuShared *g, int i) {
  const Panel *p = &panels[i];
  if (shown[i]) {
    if (mode[i] != GPU_VSYNC_OFF)
      blank_wait(i, mode[i]);
    ppf_copy(g, p->fb[shown[i]], p->fb[0], p->len);
  }
  show(i, 0);
  set_front(g, i);
}

static void panels_to_a(GpuShared *g) {
  panel_to_a(g, 0);
  panel_to_a(g, 1);
}

/* xf_flags: the panel; xf_src_w: the method; xf_len: how many blanks. */
static void gpu_op_vsync_test(GpuShared *g) {
  int i = (int)(g->xf_flags & 1u);
  uint32_t m = g->xf_src_w, n = g->xf_len;
  const Panel *p = &panels[i];

  panel_to_a(g, i);
  g->vs_swap[i] = *pdc_reg(p, PDC_SWAP);
  g->vs_cnt[i] = *pdc_reg(p, PDC_CNT);
  if (m == GPU_VSYNC_IRQ || m == GPU_VSYNC_GIC) {
    if (m == GPU_VSYNC_GIC)
      *(vu32 *)GICD_CTRL |= 1u; /* IRQs stay masked in the CPU */
    for (; n; n--)
      if (!blank_wait(i, m))
        break;
  }
  if (n)
    g->err = GPU_ERR_TIMEOUT;
}

/* xf_flags: the panel; xf_src_w: the method to use from now on. */
static void gpu_op_vsync_set(GpuShared *g) {
  int i = (int)(g->xf_flags & 1u);
  uint32_t m = g->xf_src_w;

  panel_to_a(g, i);
  mode[i] = (m == GPU_VSYNC_IRQ || m == GPU_VSYNC_GIC) ? m : GPU_VSYNC_OFF;
  presents[i] = missed[i] = 0;
  set_front(g, i);
}

static void present_one(GpuShared *g, int i, uint32_t src, uint32_t len) {
  const Panel *p = &panels[i];
  if (mode[i] == GPU_VSYNC_OFF) {
    ppf_copy(g, src, p->fb[0], len);
    return;
  }
  int f = next_fb(i);
  ppf_copy(g, src, p->fb[f], len);
  if (g->err)
    return;
  if (!blank_wait(i, mode[i]))
    missed[i]++;
  show(i, f);
  presents[i]++;
  set_front(g, i);
}

static void gpu_op_present(GpuShared *g) {
  int i = g->xf_dst == GPU_FB_TOP_A ? 0 : (g->xf_dst == GPU_FB_BOT_A ? 1 : -1);
  if (i < 0)
    ppf_copy(g, g->xf_src, g->xf_dst, g->xf_len);
  else
    present_one(g, i, g->xf_src, g->xf_len);
}

/* Both panels, xf_src the top's frame and xf_src2 the bottom's. Both frames
 * are copied first and the blanks watched together, so two panels cost one
 * frame rather than two. */
static void gpu_op_present2(GpuShared *g) {
  const uint32_t src[2] = {g->xf_src, g->xf_src2};
  int f[2] = {0, 0}, left = 0;

  for (int i = 0; i < 2; i++) {
    if (mode[i] == GPU_VSYNC_OFF) {
      ppf_copy(g, src[i], panels[i].fb[0], panels[i].len);
      continue;
    }
    f[i] = next_fb(i);
    ppf_copy(g, src[i], panels[i].fb[f[i]], panels[i].len);
    if (g->err)
      return;
    left |= 1 << i;
  }
  for (int i = 0; i < 2; i++)
    if (left & (1 << i))
      blank_arm(i, mode[i]);
  for (uint32_t w = 0; left && w < BLANK_POLLS; w++)
    for (int i = 0; i < 2; i++)
      if ((left & (1 << i)) && blank_seen(i, mode[i])) {
        show(i, f[i]);
        presents[i]++;
        set_front(g, i);
        left &= ~(1 << i);
      }
  for (int i = 0; i < 2; i++) /* a panel that stopped answering */
    if (left & (1 << i)) {
      show(i, f[i]);
      presents[i]++;
      missed[i]++;
      set_front(g, i);
    }
}

void gpu11_show_a(void) {
  GpuShared *g = (GpuShared *)GPU_SHARED_ADDR;
  if (g->ready) {
    panels_to_a(g);
    dcache_clean();
  }
}

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
    panels_to_a(g);
  } else if (op == GPU_OP_FILL) {
    gpu_op_fill(g);
  } else if (op == GPU_OP_TRANSFER) {
    gpu_op_transfer(g);
  } else if (op == GPU_OP_TEXCOPY) {
    gpu_op_texcopy(g);
  } else if (op == GPU_OP_PRESENT) {
    gpu_op_present(g);
  } else if (op == GPU_OP_PRESENT2) {
    gpu_op_present2(g);
  } else if (op == GPU_OP_SHOW_A) {
    panels_to_a(g);
  } else if (op == GPU_OP_VSYNC_TEST) {
    gpu_op_vsync_test(g);
  } else if (op == GPU_OP_VSYNC_SET) {
    gpu_op_vsync_set(g);
  } else {
    g->err = GPU_ERR_BADARG;
  }

  if (!g->err)
    g->ops++;
  g->seq++;
  dcache_clean();
}
