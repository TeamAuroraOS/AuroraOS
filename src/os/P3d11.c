/* The PICA200's 3D pipeline (P3D), on the ARM11: runs a command list the ARM9
 * built in FCRAM against the render target in VRAM.
 *
 * Hardware facts, reimplemented here: GBATEK "3DS GPU Internal Registers -
 * Command Lists" and "Finalize Interrupt registers"; the start-up values and
 * the configuration-mode note from libn3ds gfx.c hardwareReset() and
 * GX_processCommandList() (profi200, GPL-3.0). See docs/stereo3d.md. */
#include "core11.h"
#include "gpu.h"

void stereo11_tick(void);
int gpu11_display_transfer(GpuShared *g, uint32_t src, uint32_t dst);

#define GX(off)  MMIO32(GPU_REG_BASE + (off))
#define P3D(reg) MMIO32(GPU_REG_BASE + 0x1000u + (reg) * 4u)

#define GX_IRQ_STAT 0x0034u
#define STAT_P3D    (1u << 31) /* the finalize interrupt is raised */
#define PSC0        0x0010u
#define PSC1        0x0020u
#define PSC_GO      (1u << 0)
#define PSC_DONE    (1u << 1)
#define PSC_FILL32  (2u << 8)

#define CFG11_GPUPROT 0x10140140u

#define REG_IRQ_ACK       0x000u
#define REG_IRQ_CMP       0x020u
#define REG_IRQ_MASK_LO   0x030u
#define REG_IRQ_MASK_HI   0x031u
#define REG_IRQ_AUTOSTOP  0x034u
#define REG_CMDBUF_SIZE0  0x238u
#define REG_CMDBUF_ADDR0  0x23Au
#define REG_CMDBUF_JUMP0  0x23Cu
#define REG_START_DRAW    0x245u

/* A list ends with FINALIZE writing this; the compare byte matches raise the
 * interrupt and, with autostop, end the list. */
#define FINALIZE_MAGIC 0x12345678u

#define P3D_POLLS 4000000u
#define RT_BYTES  (GPU_P3D_W * GPU_P3D_H * 4u)

static int p3d_ready;

static void p3d_init(void) {
  /* Lets the GPU reach all of FCRAM: the lists and their data live there. */
  MMIO16(CFG11_GPUPROT) = 0;
  P3D(REG_IRQ_ACK) = 0;
  P3D(REG_IRQ_CMP) = FINALIZE_MAGIC;
  P3D(REG_IRQ_MASK_LO) = 0xFFFFFFF0u; /* only the first four compare bytes */
  P3D(REG_IRQ_MASK_HI) = 0xFFFFFFFFu;
  P3D(REG_IRQ_AUTOSTOP) = 1;
  /* The first list hangs the GPU unless it starts in configuration mode. */
  P3D(REG_START_DRAW) = 1;
  dsb();
  p3d_ready = 1;
}

static int clear_target(uint32_t rgba) {
  GX(PSC0 + 0xC) = 0;
  GX(PSC1 + 0xC) = 0;
  GX(PSC0 + 0x0) = GPU_P3D_COLOR >> 3;
  GX(PSC0 + 0x4) = (GPU_P3D_COLOR + RT_BYTES) >> 3;
  GX(PSC0 + 0x8) = rgba;
  GX(PSC1 + 0x0) = GPU_P3D_DEPTH >> 3;
  GX(PSC1 + 0x4) = (GPU_P3D_DEPTH + RT_BYTES) >> 3;
  GX(PSC1 + 0x8) = 0; /* the farthest depth, as the lists test GREATER */
  dsb();
  GX(PSC0 + 0xC) = PSC_FILL32 | PSC_GO;
  GX(PSC1 + 0xC) = PSC_FILL32 | PSC_GO;
  dsb();
  for (uint32_t w = 0; w < P3D_POLLS; w++) {
    uint32_t a = GX(PSC0 + 0xC), b = GX(PSC1 + 0xC);
    if (((a & PSC_DONE) || !(a & PSC_GO)) && ((b & PSC_DONE) || !(b & PSC_GO))) {
      GX(PSC0 + 0xC) = 0;
      GX(PSC1 + 0xC) = 0;
      return 1;
    }
  }
  return 0;
}

static int run_list(GpuShared *g, uint32_t list, uint32_t bytes) {
  uint32_t w;

  P3D(REG_IRQ_ACK) = 0;
  dsb();
  for (w = 0; (GX(GX_IRQ_STAT) & STAT_P3D) && w < P3D_POLLS; w++)
    ;
  P3D(REG_CMDBUF_SIZE0) = bytes >> 3;
  P3D(REG_CMDBUF_ADDR0) = list >> 3;
  dsb();
  P3D(REG_CMDBUF_JUMP0) = 1;
  dsb();
  for (w = 0; w < P3D_POLLS; w++) {
    if (GX(GX_IRQ_STAT) & STAT_P3D)
      break;
    if (!(w & 0x3FFu))
      stereo11_tick();
  }
  g->p3d_waits = w;
  g->p3d_stat = GX(GX_IRQ_STAT);
  P3D(REG_IRQ_ACK) = 0;
  dsb();
  return w < P3D_POLLS;
}

/* GPU_OP_P3D: xf_src the list, xf_len its size (a multiple of 16), xf_dst the
 * frame's destination, fill_value the clear colour (RGBA8, as 0xRRGGBBAA). */
void p3d11_run(GpuShared *g) {
  if ((g->xf_src & 15u) || !g->xf_len || (g->xf_len & 15u) ||
      (g->xf_dst & 7u)) {
    g->err = GPU_ERR_BADARG;
    return;
  }
  if (!p3d_ready)
    p3d_init();

  g->step = GPU_STEP_ARMED;
  if (!clear_target(g->fill_value)) {
    g->p3d_state = GPU_P3D_CLEAR_TIMEOUT;
    g->err = GPU_ERR_TIMEOUT;
    return;
  }
  g->step = GPU_STEP_STARTED;
  if (!run_list(g, g->xf_src, g->xf_len)) {
    g->p3d_state = GPU_P3D_LIST_TIMEOUT;
    g->err = GPU_ERR_TIMEOUT;
    return;
  }
  g->p3d_runs++;
  if (!gpu11_display_transfer(g, GPU_P3D_COLOR, g->xf_dst)) {
    g->p3d_state = GPU_P3D_COPY_TIMEOUT;
    if (!g->err)
      g->err = GPU_ERR_TIMEOUT;
    return;
  }
  g->p3d_state = GPU_P3D_OK;
  g->step = GPU_STEP_DONE;
}
