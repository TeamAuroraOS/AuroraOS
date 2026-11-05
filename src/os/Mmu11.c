/* This core's own map and caches. Only its image (code, data, stack) is
 * cached. Everything another CPU or a DMA engine can see stays strongly
 * ordered, which is how every access behaves with the MMU off, so what the core
 * shares with the ARM9 and the GPU needs no cache maintenance.
 *
 * Holding SELECT while the core starts leaves all of this off, as it does the
 * ARM9's MPU (os_main.c). */
#include "core11.h"

#define TT_SECTION (2u << 0)
#define TT_B       (1u << 2)
#define TT_C       (1u << 3)
#define TT_AP_RW   (3u << 10)
#define TT_TEX(x)  ((x) << 12)
/* Write-back, write-allocate, inner and outer; not shared. */
#define TT_NORMAL  (TT_TEX(1u) | TT_C | TT_B)

#define IMAGE_FIRST_MB (AUDIO_CORE_ADDR >> 20)
#define IMAGE_END_MB   (IMAGE_FIRST_MB + 2u)

#define SCTLR_M  (1u << 0)
#define SCTLR_C  (1u << 2)
#define SCTLR_Z  (1u << 11)
#define SCTLR_I  (1u << 12)
#define SCTLR_XP (1u << 23) /* the ARMv6 descriptor format */

/* Return stack, dynamic and static branch prediction, instruction folding. */
#define ACTLR_PREDICT 0x0Fu

#define REG_HID_PAD 0x10146000u
#define PAD_SELECT  (1u << 2) /* active low */

#define CFG11_MPCORE_CNT 0x10141304u
#define MPCORE_CNT_L2C   (1u << 8) /* the controller's power; Clock11.c sets it */
#define SOCINFO_LGR2     (1u << 2)

/* The New 3DS's L2C-310 r3p3, 2 MB (3dbrew, Memory layout). */
#define L2C_CTRL     0x17E10100u
#define L2C_INT_CLR  0x17E10220u
#define L2C_SYNC     0x17E10730u
#define L2C_INV_WAY  0x17E1077Cu
#define L2C_ALL_WAYS 0xFFFFu
#define L2C_POLLS    4000000u

/* In audio11_start.s, since nothing may touch memory between its steps. */
void mmu11_stop(uint32_t l2);

static uint32_t tt[4096] __attribute__((aligned(16384)));
static int allowed, mapped, l2_on;

static inline void isb(void) {
  __asm__ volatile("mcr p15, 0, %0, c7, c5, 4" ::"r"(0) : "memory");
}

void mmu11_flush_all(void) {
  dsb();
  __asm__ volatile("mcr p15, 0, %0, c7, c14, 0\n\t" /* clean + invalidate D */
                   "mcr p15, 0, %0, c7, c10, 4\n\t" /* DSB */
                   "mcr p15, 0, %0, c7, c5, 0\n\t"  /* invalidate I */
                   "mcr p15, 0, %0, c7, c5, 6\n\t"  /* branch predictor */
                   "mcr p15, 0, %0, c8, c7, 0\n\t"  /* TLB */
                   "mcr p15, 0, %0, c7, c10, 4" /* DSB */
                   ::"r"(0)
                   : "memory");
  isb();
}

static void publish(void) {
  AudioCtrl *ct = (AudioCtrl *)AUDIO_CTRL_ADDR;
  ct->core_flags = (mapped ? AUDIO_FLAG_CACHES : 0u) |
                   (l2_on ? AUDIO_FLAG_L2 : 0u);
}

static int l2_wait(uint32_t reg, uint32_t mask) {
  for (uint32_t i = 0; i < L2C_POLLS; i++)
    if (!(MMIO32(reg) & mask))
      return 1;
  return 0;
}

/* Only at 804 MHz: that mode is what powers the controller on, and with it off
 * the block should not be touched. The reset state of its tag RAMs is unknown,
 * so every way is invalidated first. Called with the caches off. */
static void l2_start(void) {
  const AudioCtrl *ct = (const AudioCtrl *)AUDIO_CTRL_ADDR;
  if (!(ct->socinfo & SOCINFO_LGR2) || core11_clock_div() != 3u ||
      !(MMIO16(CFG11_MPCORE_CNT) & MPCORE_CNT_L2C))
    return;
  MMIO32(L2C_CTRL) = 0;
  dsb();
  MMIO32(L2C_INT_CLR) = 0x1FFu;
  MMIO32(L2C_INV_WAY) = L2C_ALL_WAYS;
  if (!l2_wait(L2C_INV_WAY, L2C_ALL_WAYS))
    return;
  MMIO32(L2C_SYNC) = 0;
  if (!l2_wait(L2C_SYNC, 1u))
    return;
  MMIO32(L2C_CTRL) = 1u;
  dsb();
  l2_on = 1;
}

void mmu11_on(void) {
  uint32_t actlr, sctlr;
  if (!allowed || mapped)
    return;
  mmu11_flush_all();
  l2_start();
  __asm__ volatile("mrc p15, 0, %0, c1, c0, 1" : "=r"(actlr));
  __asm__ volatile("mcr p15, 0, %0, c1, c0, 1" ::"r"(actlr | ACTLR_PREDICT));
  __asm__ volatile("mcr p15, 0, %0, c3, c0, 0" ::"r"(0xFFFFFFFFu));
  __asm__ volatile("mcr p15, 0, %0, c2, c0, 2" ::"r"(0));
  __asm__ volatile("mcr p15, 0, %0, c2, c0, 0" ::"r"((uint32_t)tt));
  isb();
  __asm__ volatile("mrc p15, 0, %0, c1, c0, 0" : "=r"(sctlr));
  __asm__ volatile("mcr p15, 0, %0, c1, c0, 0" ::"r"(
      sctlr | SCTLR_M | SCTLR_C | SCTLR_I | SCTLR_Z | SCTLR_XP));
  isb();
  mapped = 1;
  publish();
}

void mmu11_off(void) {
  if (!mapped)
    return;
  /* Cached copies of these two are about to be left behind. */
  int was_l2 = l2_on;
  mapped = 0;
  l2_on = 0;
  mmu11_stop((uint32_t)was_l2);
  publish();
}

void mmu11_init(void) {
  for (uint32_t mb = 0; mb < 4096u; mb++)
    tt[mb] = (mb << 20) | TT_SECTION | TT_AP_RW |
             (mb >= IMAGE_FIRST_MB && mb < IMAGE_END_MB ? TT_NORMAL : 0u);
  allowed = (MMIO16(REG_HID_PAD) & PAD_SELECT) != 0;
  publish();
  mmu11_on();
}
