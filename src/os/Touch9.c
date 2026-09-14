/* Turns the ARM11's raw touch ADC values into screen pixels. */
#include "aurora.h"
#include "touch.h"

/* Raw 12-bit ADC -> screen-pixel calibration. A first guess, tuned by eye; swap
 * min and max to invert an axis. */
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

/* Fires once when a new touch begins; a finger still down on a new screen must
 * lift first. */
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
