#ifndef AURORA_TOUCH_H
#define AURORA_TOUCH_H

#include <stdint.h>

#define TOUCH_SHARED_ADDR 0x233A0000u

typedef struct {
  volatile uint32_t seq;     /* bumped on each ARM11 update (heartbeat) */
  volatile uint32_t pressed; /* 1 while the screen is being touched     */
  volatile uint32_t raw_x;   /* raw ADC X (12-bit)                      */
  volatile uint32_t raw_y;   /* raw ADC Y (12-bit)                      */
  /* Diagnostics, always updated (even when not pressed): the raw codec sample
   * bytes the touch state is derived from. */
  volatile uint32_t d_b0;  /* buf[0]  (pen-down flag byte + X high) */
  volatile uint32_t d_b1;  /* buf[1]  (X low)  */
  volatile uint32_t d_b10; /* buf[10] (Y high) */
  volatile uint32_t d_b11; /* buf[11] (Y low)  */
} TouchShared;

/* ARM9 API (src/os/os_main.c). Returns 1 if the screen is being touched and
 * fills the screen-pixel position (0..319, 0..239); 0 otherwise. The raw ADC
 * values are returned through rawx/rawy when non-NULL (handy for calibration). */
int touch_read(int *sx, int *sy, int *rawx, int *rawy);

/* Press-edge "tap": returns 1 once at the moment a new touch begins, filling the
 * tap position. Call once per input-loop iteration (like get_keys_down). */
int touch_tap(int *x, int *y);

/* True if point (tx,ty) is inside the rect (x,y,w,h). */
static inline int touch_in(int tx, int ty, int x, int y, int w, int h) {
  return tx >= x && tx < x + w && ty >= y && ty < y + h;
}

#endif /* AURORA_TOUCH_H */
