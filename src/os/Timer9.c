/* ARM9 hardware timer for real microseconds: timer 0 divides the 67.027964 MHz
 * bus clock by 1024 and timer 1 counts its overflows. */
#include "aurora.h"
#include "timer.h"

#define TMR_BASE  0x10003000u
#define TMR0_VAL  (*(volatile u16 *)(TMR_BASE + 0x00))
#define TMR0_CNT  (*(volatile u16 *)(TMR_BASE + 0x02))
#define TMR1_VAL  (*(volatile u16 *)(TMR_BASE + 0x04))
#define TMR1_CNT  (*(volatile u16 *)(TMR_BASE + 0x06))

#define TIMER_HZ 65457u /* 67027964 / 1024 */

static u32 ticks_per_s = 0;
static int started;

void timer_start(void) {
  started = 1;
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

int timer_ready(void) {
  if (!started)
    timer_start();
  ticks_per_s = TIMER_HZ;
  return 1;
}

int timer_calibrated(void) { return ticks_per_s != 0; }

u32 timer_hz(void) { return ticks_per_s; }

u32 timer_us_since(u32 t0) {
  if (!ticks_per_s)
    return 0;
  return (u32)(((u64)(timer_ticks() - t0) * 1000000u) / ticks_per_s);
}
