#ifndef AURORA_TOUCH_H
#define AURORA_TOUCH_H

#include <stdint.h>

#define TOUCH_SHARED_ADDR 0x233A0000u

typedef struct {
  volatile uint32_t seq;     /* bumped on each ARM11 update (heartbeat) */
  volatile uint32_t pressed; /* 1 while the screen is being touched     */
  volatile uint32_t raw_x;   /* raw ADC X (12-bit)                      */
  volatile uint32_t raw_y;   /* raw ADC Y (12-bit)                      */
  /* Raw codec sample bytes, updated even when not pressed. */
  volatile uint32_t d_b0;  /* buf[0]  (pen-down flag byte + X high) */
  volatile uint32_t d_b1;  /* buf[1]  (X low)  */
  volatile uint32_t d_b10; /* buf[10] (Y high) */
  volatile uint32_t d_b11; /* buf[11] (Y low)  */
} TouchShared;

/* Returns 1 while touched, with the position in screen pixels. Raw ADC values
 * go through rawx/rawy when non-NULL. */
int touch_read(int *sx, int *sy, int *rawx, int *rawy);

/* Press-edge "tap": returns 1 once at the moment a new touch begins, filling
 * the tap position. Call once per input-loop iteration (like get_keys_down). */
int touch_tap(int *x, int *y);

static inline int touch_in(int tx, int ty, int x, int y, int w, int h) {
  return tx >= x && tx < x + w && ty >= y && ty < y + h;
}

#endif
