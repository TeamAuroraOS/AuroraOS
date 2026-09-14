#ifndef AURORA_TIMER_H
#define AURORA_TIMER_H

#include <stdint.h>

/* Microsecond timing from an ARM9 hardware timer, calibrated against the MCU
 * RTC. */
/* Calibrates against the real-time clock, which means waiting out up to two
 * RTC second boundaries. Call it once at start-up, never on a hot path. */
int timer_ready(void);

/* Whether that has already happened. Cheap, and safe to call per frame. */
int timer_calibrated(void);
void timer_start(void);
uint32_t timer_ticks(void);
uint32_t timer_hz(void);
uint32_t timer_us_since(uint32_t t0);

#endif
