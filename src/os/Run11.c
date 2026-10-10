/* AUDIO_CMD_RUN11: runs ARM11 code an app put in FCRAM, as a function of one
 * argument, the way Horizon runs an app's code: with the MMU on, the L1
 * caches and branch prediction on, and the VFP enabled. This core itself runs
 * with all of that off (docs/audio.md, "A core's start-up"), so everything is
 * put back before the ack. The L2 cache of the New 3DS stays off.
 *
 * The map is flat: every 1 MB section at its own address, strongly ordered,
 * except FCRAM's first 128 MB (every model has them), which is normal
 * write-back memory. Domain 0 is a manager domain, so nothing is checked. */
#include "core11.h"

#define TT_SECTION (2u << 0)
#define TT_B       (1u << 2)
#define TT_C       (1u << 3)
#define TT_AP_RW   (3u << 10)
#define TT_TEX(x)  ((x) << 12)
/* Write-back, write-allocate, inner and outer; not shared. */
#define TT_NORMAL  (TT_TEX(1u) | TT_C | TT_B)

#define FCRAM_FIRST_MB 0x200u
#define FCRAM_END_MB   0x280u

#define SCTLR_M  (1u << 0)
#define SCTLR_C  (1u << 2)
#define SCTLR_Z  (1u << 11)
#define SCTLR_I  (1u << 12)
#define SCTLR_XP (1u << 23) /* the ARMv6 descriptor format */

#define CPACR_VFP (0xFu << 20) /* cp10 and cp11, every mode */
#define FPEXC_EN  (1u << 30)

static uint32_t tt[4096] __attribute__((aligned(16384)));
static int tt_ready;

static void tt_build(void) {
  for (uint32_t mb = 0; mb < 4096u; mb++)
    tt[mb] = (mb << 20) | TT_SECTION | TT_AP_RW |
             (mb >= FCRAM_FIRST_MB && mb < FCRAM_END_MB ? TT_NORMAL : 0u);
}

static inline void isb(void) {
  __asm__ volatile("mcr p15, 0, %0, c7, c5, 4" ::"r"(0) : "memory");
}

/* Nothing cached may outlive a change of the map or of the caches. */
static void caches_flush(void) {
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

static void vfp_enable(void) {
  uint32_t cpacr;
  __asm__ volatile("mrc p15, 0, %0, c1, c0, 2" : "=r"(cpacr));
  __asm__ volatile("mcr p15, 0, %0, c1, c0, 2" ::"r"(cpacr | CPACR_VFP));
  isb();
  /* FMXR FPEXC: MCR p10, 7, Rt, c8, c0, 0. */
  __asm__ volatile("mcr p10, 7, %0, c8, c0, 0" ::"r"(FPEXC_EN));
  isb();
}

void run11(AudioCtrl *ct, uint32_t entry, uint32_t arg) {
  uint32_t sctlr, ttbr0, ttbcr, dacr;
  (void)ct;
  if (entry < (FCRAM_FIRST_MB << 20) || entry >= (FCRAM_END_MB << 20) ||
      (entry & 3u))
    return;
  if (!tt_ready) {
    tt_build();
    tt_ready = 1;
  }
  vfp_enable();

  __asm__ volatile("mrc p15, 0, %0, c1, c0, 0" : "=r"(sctlr));
  __asm__ volatile("mrc p15, 0, %0, c2, c0, 0" : "=r"(ttbr0));
  __asm__ volatile("mrc p15, 0, %0, c2, c0, 2" : "=r"(ttbcr));
  __asm__ volatile("mrc p15, 0, %0, c3, c0, 0" : "=r"(dacr));
  /* Someone else's map: leave it, and run as things are. */
  if (sctlr & SCTLR_M) {
    ((void (*)(uint32_t))entry)(arg);
    return;
  }

  caches_flush();
  __asm__ volatile("mcr p15, 0, %0, c3, c0, 0" ::"r"(0xFFFFFFFFu));
  __asm__ volatile("mcr p15, 0, %0, c2, c0, 2" ::"r"(0));
  __asm__ volatile("mcr p15, 0, %0, c2, c0, 0" ::"r"((uint32_t)tt));
  isb();
  __asm__ volatile("mcr p15, 0, %0, c1, c0, 0" ::"r"(
      sctlr | SCTLR_M | SCTLR_C | SCTLR_I | SCTLR_Z | SCTLR_XP));
  isb();

  ((void (*)(uint32_t))entry)(arg);

  /* The app's results reach memory before the caches go off. */
  dsb();
  __asm__ volatile("mcr p15, 0, %0, c7, c14, 0" ::"r"(0) : "memory");
  dsb();
  __asm__ volatile("mcr p15, 0, %0, c1, c0, 0" ::"r"(sctlr));
  isb();
  __asm__ volatile("mcr p15, 0, %0, c2, c0, 0" ::"r"(ttbr0));
  __asm__ volatile("mcr p15, 0, %0, c2, c0, 2" ::"r"(ttbcr));
  __asm__ volatile("mcr p15, 0, %0, c3, c0, 0" ::"r"(dacr));
  caches_flush();
}
