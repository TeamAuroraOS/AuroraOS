#ifndef AURORA_VIDEO11_ENGINE_H
#define AURORA_VIDEO11_ENGINE_H

#include <stdint.h>

struct AVFrame;

/* ARMv6 has these as coprocessor operations, not instructions. */
static inline void cpu_dmb(void) {
  __asm__ volatile("mcr p15, 0, %0, c7, c10, 5" ::"r"(0) : "memory");
}
static inline void cpu_dsb(void) {
  __asm__ volatile("mcr p15, 0, %0, c7, c10, 4" ::"r"(0) : "memory");
}
static inline void cpu_sev(void) {
  cpu_dsb();
  __asm__ volatile("sev");
}

void video11_main(void);

/* convert11.c: a YUV 4:2:0 frame as BGR in the top framebuffer's layout, scaled to
 * fit 400x240, black bars where the shapes differ. */
void video11_convert(const struct AVFrame *f, uint8_t *fb);

/* threads11.c: runs the job's other cores, and the pthreads FFmpeg uses on them. */
void threads11_core_loop(uint32_t core);
void threads11_setup(uint32_t cores_mask);
void threads11_stop(void);
/* The core has told the job to end (SMP_OP_END). */
int smp_stop(void);

#endif
