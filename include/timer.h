#ifndef AURORA_TIMER_H
#define AURORA_TIMER_H

#include <stdint.h>

/* Starts the timer if timer_start() has not and sets its rate. A running timer
 * is not restarted, so earlier ticks stay valid. */
int timer_ready(void);

/* 1 once timer_ready() has run. */
int timer_calibrated(void);
void timer_start(void);
uint32_t timer_ticks(void);
uint32_t timer_hz(void);
uint32_t timer_us_since(uint32_t t0);

#endif
