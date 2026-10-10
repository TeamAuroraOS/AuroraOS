/* The ARM11 job's entry point, first in the image (job11.ld). The core calls
 * it with the MMU, the L1 caches and the VFP on, interrupts off. */
#include "job11.h"

/* Flush-to-zero and default NaN, so the VFP never hands an operation to
 * support code, which AuroraOS's core does not have. Horizon runs the same
 * workloads with the same setting (../../horizon/source/main.c). */
#define FPSCR_RUNFAST 0x03000000u

__attribute__((section(".text.entry"))) uint32_t job11_entry(Job11 *j) {
  __asm__ volatile("vmsr fpscr, %0" ::"r"(FPSCR_RUNFAST));
  j->sum = work_run(&j->ctx, j->kind, j->reps);
  j->ran = 1;
  return j->sum;
}
