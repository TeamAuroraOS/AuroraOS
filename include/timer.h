#ifndef AURORA_TIMER_H
#define AURORA_TIMER_H

#include <stdint.h>

/* Microsecond timing from an ARM9 hardware timer, calibrated against the MCU
 * real-time clock so the readings do not depend on an assumed clock rate.
 * Call timer_ready() once before using the rest; it returns 0 if the MCU did
 * not answer, in which case the readings are meaningless. */
int timer_ready(void);
void timer_start(void);
uint32_t timer_ticks(void);
uint32_t timer_hz(void);
uint32_t timer_us_since(uint32_t t0);

#endif /* AURORA_TIMER_H */
