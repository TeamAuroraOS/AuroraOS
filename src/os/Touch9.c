/* Turns the ARM11's raw touch ADC values into screen pixels. */
#include "aurora.h"
#include "touch.h"

/* Used until Settings > Touch Calibration saves a measured one. A first guess,
 * tuned by eye. */
#define TS_X_MIN 0x0D0
#define TS_X_MAX 0xF00
#define TS_Y_MIN 0x0F0
#define TS_Y_MAX 0xF00

/* The real spans are about 0xE00. One under a tenth of that comes from taps that
 * were not measured properly, and would divide the screen into a few pixels. */
#define TS_MIN_SPAN 256

static TouchCal cal = {TS_X_MIN, TS_X_MAX, TS_Y_MIN, TS_Y_MAX};

void touch_cal_default(TouchCal *c) {
  c->x_min = TS_X_MIN;
  c->x_max = TS_X_MAX;
  c->y_min = TS_Y_MIN;
  c->y_max = TS_Y_MAX;
}

void touch_cal_get(TouchCal *c) { *c = cal; }

static int span_ok(int a, int b) {
  int d = b - a;
  return d >= TS_MIN_SPAN || d <= -TS_MIN_SPAN;
}

int touch_cal_set(const TouchCal *c) {
  if (!span_ok(c->x_min, c->x_max) || !span_ok(c->y_min, c->y_max))
    return 0;
  cal = *c;
  return 1;
}

int touch_cal_is_default(void) {
  return cal.x_min == TS_X_MIN && cal.x_max == TS_X_MAX &&
         cal.y_min == TS_Y_MIN && cal.y_max == TS_Y_MAX;
}

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

  int x = (rx - cal.x_min) * 320 / (cal.x_max - cal.x_min);
  int y = (ry - cal.y_min) * 240 / (cal.y_max - cal.y_min);
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
