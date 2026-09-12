/*
 * Microsecond timing, ARM9 side.
 *
 * An ARM9 hardware timer does the counting, calibrated against one MCU
 * real-time-clock second so the figures are real microseconds and do not
 * depend on any assumed clock constant. Used by the render benchmark.
 */
#include "aurora.h"
#include "power.h"
#include "timer.h"

#define TMR_BASE  0x10003000u
#define TMR0_VAL  (*(volatile u16 *)(TMR_BASE + 0x00))
#define TMR0_CNT  (*(volatile u16 *)(TMR_BASE + 0x02))
#define TMR1_VAL  (*(volatile u16 *)(TMR_BASE + 0x04))
#define TMR1_CNT  (*(volatile u16 *)(TMR_BASE + 0x06))

static u32 ticks_per_s = 0;

void timer_start(void) {
  TMR0_CNT = 0;
  TMR1_CNT = 0;
  TMR0_VAL = 0;
  TMR1_VAL = 0;
  TMR1_CNT = 0x84; /* count this timer up on timer 0 overflow */
  TMR0_CNT = 0x83; /* run, prescaler /1024 */
}

u32 timer_ticks(void) {
  u32 hi = TMR1_VAL, lo = TMR0_VAL, hi2 = TMR1_VAL;
  if (hi != hi2) {
    hi = hi2;
    lo = TMR0_VAL;
  }
  return (hi << 16) | lo;
}

/* Ticks counted across one whole RTC second. */
static u32 calibrate(void) {
  RtcTime t;
  int s0;
  u32 a;
  if (!rtc_read(&t))
    return 0;
  s0 = t.sec;
  while (rtc_read(&t) && t.sec == s0)
    ;
  a = timer_ticks();
  s0 = t.sec;
  while (rtc_read(&t) && t.sec == s0)
    ;
  return timer_ticks() - a;
}

int timer_ready(void) {
  timer_start();
  ticks_per_s = calibrate();
  return ticks_per_s != 0;
}

u32 timer_hz(void) { return ticks_per_s; }

u32 timer_us_since(u32 t0) {
  if (!ticks_per_s)
    return 0;
  return (u32)(((u64)(timer_ticks() - t0) * 1000000u) / ticks_per_s);
}
