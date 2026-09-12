/*
 * Touchscreen, ARM9 side.
 *
 * The panel is sampled by the ARM11 (Touch11.c), which publishes raw ADC values
 * to the shared block at TOUCH_SHARED_ADDR. This turns those into screen pixels
 * and provides the press-edge helper the UI uses.
 */
#include "aurora.h"
#include "touch.h"

/* Touchscreen raw-ADC -> screen-pixel calibration. Defaults are a first guess;
 * tune them by eye (flip min/max to invert an axis). Raw ADC is 12-bit. */
#define TS_X_MIN 0x0D0
#define TS_X_MAX 0xF00
#define TS_Y_MIN 0x0F0
#define TS_Y_MAX 0xF00

int touch_read(int *sx, int *sy, int *rawx, int *rawy) {
  volatile TouchShared *ts = (volatile TouchShared *)TOUCH_SHARED_ADDR;
  __asm__ volatile("mcr p15, 0, %0, c7, c6, 1" ::"r"(TOUCH_SHARED_ADDR)
                   : "memory");
  if (!ts->pressed)
    return 0;

  int rx = (int)ts->raw_x, ry = (int)ts->raw_y;
  if (rawx)
    *rawx = rx;
  if (rawy)
    *rawy = ry;

  int x = (rx - (TS_X_MIN)) * 320 / ((TS_X_MAX) - (TS_X_MIN));
  int y = (ry - (TS_Y_MIN)) * 240 / ((TS_Y_MAX) - (TS_Y_MIN));
  if (x < 0)
    x = 0;
  if (x > 319)
    x = 319;
  if (y < 0)
    y = 0;
  if (y > 239)
    y = 239;
  if (sx)
    *sx = x;
  if (sy)
    *sy = y;
  return 1;
}

/* Press-edge tap: fires once when a new touch begins. The shared prev-state
 * makes a tap that opens a new screen not immediately re-fire there (the finger
 * must lift and press again), matching the A-button edge behaviour. */
static int touch_prev = 0;
int touch_tap(int *x, int *y) {
  int sx = 0, sy = 0;
  int pressed = touch_read(&sx, &sy, NULL, NULL);
  int tapped = pressed && !touch_prev;
  touch_prev = pressed;
  if (tapped) {
    if (x)
      *x = sx;
    if (y)
      *y = sy;
  }
  return tapped;
}
