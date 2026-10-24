#ifndef AURORA_LID_H
#define AURORA_LID_H

#include "aurora.h"

/* GPIO1 bit 2 is high while the 3DS shell is closed. */
#define AURORA_GPIO1_DATA (*(volatile u8 *)0x10147000u)
#define AURORA_LID_CLOSED (1u << 2)

static inline int aurora_lid_is_closed(void) {
  return (AURORA_GPIO1_DATA & AURORA_LID_CLOSED) != 0;
}

/* Runtime code is cooperative, so pause at its normal input/wait checkpoints.
 * Return whether we waited, allowing the caller to reset input/timing state. */
static inline int aurora_lid_wait_open(void) {
  int waited = 0;
  while (aurora_lid_is_closed()) {
    waited = 1;
    for (volatile u32 i = 0; i < 60000u; i++)
      __asm__ volatile("nop");
  }
  return waited;
}

#endif
