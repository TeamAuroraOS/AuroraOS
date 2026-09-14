/* New 3DS clock mode (AUDIO_CMD_N3DS). A write to CFG11_MPCORE_CLKCNT is only a
 * request: it applies once every ARM11 core waits for an interrupt, and the
 * hardware then sets bit 15 and raises interrupt 88.
 *
 * The sequence follows fastboot3DS (source/arm11/hardware/cpu.c) and libn3ds
 * (source/arm11/drivers/pdn.c), both by derrek and profi200 under GPL-3.0.
 *
 * A WFI that never returns would hang this core, so the timer interrupt is
 * first checked to reach the CPU interface, the timer stays armed as a second
 * wake source and the wait gives up after about a second (the other core may
 * not be in WFI), a request that did not apply is withdrawn, and every GIC and
 * timer register touched is restored. */
#include "core11.h"

#define CFG11_GPU_N3DS_CNT  (IO_BASE + 0x40400u) /* u8                     */
#define CFG11_MPCORE_CLKCNT (IO_BASE + 0x41300u) /* written as u16         */
#define CFG11_MPCORE_CNT    (IO_BASE + 0x41304u) /* u16                    */

#define CLKCNT_WANT(v) ((v) & 7u)          /* bits 0-2: requested mode      */
#define CLKCNT_FLAG    (1u << 15)          /* set when a change has applied */
#define CLKCNT_CUR(v)  (((v) >> 16) & 7u)  /* bits 16-18: mode in effect    */

#define MPCORE_CNT_QTM (1u << 0) /* extra memory at 0x1F000000               */
#define MPCORE_CNT_L2C (1u << 8) /* powers the L2C block; LGR2 only          */
#define GPU_N3DS_BITS  0x3u      /* memory extensions, texture fix          */

#define SOCINFO_LGR1 (1u << 1)
#define SOCINFO_LGR2 (1u << 2)

/* MPCore private region: CPU interface, core 0's timer, distributor. */
#define MPCORE        0x17E00000u
#define GICC_CTRL     (MPCORE + 0x100u)
#define GICC_PRIMASK  (MPCORE + 0x104u)
#define GICC_IAR      (MPCORE + 0x10Cu)
#define GICC_EOI      (MPCORE + 0x110u)
#define GICC_HIGHPEND (MPCORE + 0x118u)
#define TIMER_LOAD    (MPCORE + 0x600u)
#define TIMER_CNT     (MPCORE + 0x608u)
#define TIMER_STAT    (MPCORE + 0x60Cu)
#define GICD_CTRL     (MPCORE + 0x1000u)
#define GICD_ENA_SET  (MPCORE + 0x1100u) /* one bit per id, 32 to a word */
#define GICD_ENA_CLR  (MPCORE + 0x1180u)
#define GICD_PEN_CLR  (MPCORE + 0x1280u)
#define GICD_PRI      (MPCORE + 0x1400u) /* one byte per id              */
#define GICD_TARGET   (MPCORE + 0x1800u)

#define BIT_REG(base, id) ((base) + 4u * ((id) / 32u))
#define BIT_OF(id)        (1u << ((id) % 32u))
#define BYTE_REG(base, id) ((base) + 4u * ((id) / 4u))

#define IRQ_TIMER 29u   /* core 0's private timer */
#define IRQ_CLOCK 88u   /* clock mode change      */
#define IRQ_NONE  1023u /* nothing pending         */

#define TIMER_EN     (1u << 0)
#define TIMER_RELOAD (1u << 1)
#define TIMER_IRQ    (1u << 2)
/* With no prescaler the private timer counts at half the 268 MHz clock. */
#define TIMER_HZ     (268111856u / 2u)

#define SLICE_TICKS (TIMER_HZ / 20u) /* a timer wake every 50 ms         */
#define SLICE_LIMIT 20u              /* so about a second in all          */
#define WAKE_LIMIT  1000u            /* any wake at all, timer or not     */
#define PEND_GUARD  2000000u         /* polls for the timer check         */

typedef struct {
  uint32_t gicc_ctrl, gicc_primask, gicd_ctrl;
  uint32_t ena[4];
  uint32_t pri_timer, pri_clock, target_clock;
  uint32_t timer_load, timer_cnt;
} GicSave;

static void set_byte(uint32_t reg, uint32_t id, uint32_t v) {
  uint32_t s = 8u * (id % 4u);
  MMIO32(reg) = (MMIO32(reg) & ~(0xFFu << s)) | ((v & 0xFFu) << s);
}

/* Acknowledge whatever the CPU interface holds for this core; returns its id. */
static uint32_t ack_one(void) {
  uint32_t iar = MMIO32(GICC_IAR);
  if ((iar & 0x3FFu) != IRQ_NONE)
    MMIO32(GICC_EOI) = iar;
  return iar & 0x3FFu;
}

static void ack_all(void) {
  for (int i = 0; i < 16; i++)
    if (ack_one() == IRQ_NONE)
      break;
}

static void timer_stop(void) {
  MMIO32(TIMER_CNT) = 0;
  MMIO32(TIMER_STAT) = 1u;
}

/* Save the interrupt state, then leave only the timer and the clock change
 * enabled, both at top priority and aimed at core 0. */
static void gic_take(GicSave *s) {
  s->gicc_ctrl = MMIO32(GICC_CTRL);
  s->gicc_primask = MMIO32(GICC_PRIMASK);
  s->gicd_ctrl = MMIO32(GICD_CTRL);
  for (uint32_t i = 0; i < 4u; i++)
    s->ena[i] = MMIO32(GICD_ENA_SET + 4u * i);
  s->pri_timer = MMIO32(BYTE_REG(GICD_PRI, IRQ_TIMER));
  s->pri_clock = MMIO32(BYTE_REG(GICD_PRI, IRQ_CLOCK));
  s->target_clock = MMIO32(BYTE_REG(GICD_TARGET, IRQ_CLOCK));
  s->timer_load = MMIO32(TIMER_LOAD);
  s->timer_cnt = MMIO32(TIMER_CNT);

  for (uint32_t i = 0; i < 4u; i++)
    MMIO32(GICD_ENA_CLR + 4u * i) = 0xFFFFFFFFu;
  timer_stop();
  MMIO32(BIT_REG(GICD_PEN_CLR, IRQ_TIMER)) = BIT_OF(IRQ_TIMER);
  MMIO32(BIT_REG(GICD_PEN_CLR, IRQ_CLOCK)) = BIT_OF(IRQ_CLOCK);
  set_byte(BYTE_REG(GICD_PRI, IRQ_TIMER), IRQ_TIMER, 0);
  set_byte(BYTE_REG(GICD_PRI, IRQ_CLOCK), IRQ_CLOCK, 0);
  set_byte(BYTE_REG(GICD_TARGET, IRQ_CLOCK), IRQ_CLOCK, 1u);

  MMIO32(GICD_CTRL) = 1u;
  MMIO32(GICC_PRIMASK) = 0xF0u;
  MMIO32(GICC_CTRL) = 1u;
  ack_all(); /* anything stale, before the real sources are armed */

  MMIO32(BIT_REG(GICD_ENA_SET, IRQ_TIMER)) = BIT_OF(IRQ_TIMER);
  MMIO32(BIT_REG(GICD_ENA_SET, IRQ_CLOCK)) = BIT_OF(IRQ_CLOCK);
}

static void gic_give(const GicSave *s) {
  timer_stop();
  MMIO32(BIT_REG(GICD_ENA_CLR, IRQ_TIMER)) = BIT_OF(IRQ_TIMER);
  MMIO32(BIT_REG(GICD_ENA_CLR, IRQ_CLOCK)) = BIT_OF(IRQ_CLOCK);
  ack_all();
  MMIO32(BIT_REG(GICD_PEN_CLR, IRQ_TIMER)) = BIT_OF(IRQ_TIMER);
  MMIO32(BIT_REG(GICD_PEN_CLR, IRQ_CLOCK)) = BIT_OF(IRQ_CLOCK);

  MMIO32(TIMER_LOAD) = s->timer_load;
  MMIO32(TIMER_CNT) = s->timer_cnt;
  MMIO32(BYTE_REG(GICD_PRI, IRQ_TIMER)) = s->pri_timer;
  MMIO32(BYTE_REG(GICD_PRI, IRQ_CLOCK)) = s->pri_clock;
  MMIO32(BYTE_REG(GICD_TARGET, IRQ_CLOCK)) = s->target_clock;
  MMIO32(GICC_PRIMASK) = s->gicc_primask;
  MMIO32(GICC_CTRL) = s->gicc_ctrl;
  MMIO32(GICD_CTRL) = s->gicd_ctrl;
  for (uint32_t i = 0; i < 4u; i++)
    MMIO32(GICD_ENA_SET + 4u * i) = s->ena[i];
}

/* Fire the timer once and look for it at the CPU interface. WFI wakes on the
 * same signal, so if it shows up here a sleep is safe to start. */
static int wake_source_works(void) {
  int seen = 0;

  MMIO32(TIMER_LOAD) = TIMER_HZ / 1000u; /* 1 ms, single shot */
  MMIO32(TIMER_CNT) = TIMER_IRQ | TIMER_EN;
  for (uint32_t n = 0; n < PEND_GUARD; n++) {
    if ((MMIO32(GICC_HIGHPEND) & 0x3FFu) == IRQ_TIMER) {
      seen = 1;
      break;
    }
  }
  timer_stop();
  ack_all();
  return seen;
}

void clock11_set(AudioCtrl *ct, uint32_t mode) {
  uint32_t info = ct->socinfo;
  uint32_t clk = MMIO32(CFG11_MPCORE_CLKCNT);
  uint32_t cnt, flag, slices = 0, wakes = 0, status;
  GicSave save;

  ct->n3ds_before = clk;
  ct->n3ds_after = clk;

  if (!((mode == 3u && (info & SOCINFO_LGR1)) ||
        ((mode == 1u || mode == 5u) && (info & SOCINFO_LGR2)))) {
    ct->n3ds_status = AUDIO_N3DS_UNSUPPORTED;
    return;
  }
  if (CLKCNT_CUR(clk) == mode) {
    ct->n3ds_status = AUDIO_N3DS_ALREADY;
    return;
  }

  gic_take(&save);
  if (!wake_source_works()) {
    gic_give(&save);
    ct->n3ds_status = AUDIO_N3DS_NO_WAKE;
    return;
  }

  cnt = MMIO16(CFG11_MPCORE_CNT);
  MMIO16(CFG11_MPCORE_CNT) = (info & SOCINFO_LGR2)
                                 ? (MPCORE_CNT_L2C | MPCORE_CNT_QTM)
                                 : MPCORE_CNT_QTM;
  spin(2000); /* both references wait briefly after this write */

  MMIO32(TIMER_LOAD) = SLICE_TICKS;
  MMIO32(TIMER_CNT) = TIMER_IRQ | TIMER_RELOAD | TIMER_EN;

  /* Acknowledge any old flag and make the request in the same write. */
  MMIO16(CFG11_MPCORE_CLKCNT) = CLKCNT_FLAG | CLKCNT_WANT(mode);
  for (;;) {
    __asm__ volatile("wfi");
    if (MMIO32(CFG11_MPCORE_CLKCNT) & CLKCNT_FLAG)
      break;
    if (MMIO32(TIMER_STAT) & 1u) {
      MMIO32(TIMER_STAT) = 1u;
      if (++slices >= SLICE_LIMIT)
        break;
    }
    ack_one();
    if (++wakes >= WAKE_LIMIT)
      break;
  }
  timer_stop();

  flag = MMIO32(CFG11_MPCORE_CLKCNT) & CLKCNT_FLAG;
  if (flag)
    MMIO16(CFG11_MPCORE_CLKCNT) = CLKCNT_FLAG | CLKCNT_WANT(mode);
  clk = MMIO32(CFG11_MPCORE_CLKCNT);

  if (flag || CLKCNT_CUR(clk) == mode) {
    MMIO8(CFG11_GPU_N3DS_CNT) = GPU_N3DS_BITS;
    status = AUDIO_N3DS_APPLIED;
  } else {
    /* Put back the request that was there before, which is the mode in
     * effect, so this one cannot apply later. */
    MMIO16(CFG11_MPCORE_CLKCNT) = CLKCNT_FLAG | CLKCNT_WANT(ct->n3ds_before);
    MMIO16(CFG11_MPCORE_CNT) = (uint16_t)cnt;
    status = AUDIO_N3DS_REFUSED;
  }

  gic_give(&save);
  ct->n3ds_after = MMIO32(CFG11_MPCORE_CLKCNT);
  ct->n3ds_status = status;
}
