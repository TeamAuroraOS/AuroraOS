#ifndef AURORA_TOUCH_H
#define AURORA_TOUCH_H

#include <stdint.h>

#define TOUCH_SHARED_ADDR 0x233A0000u

typedef struct {
  volatile uint32_t seq;     /* bumped on each ARM11 update (heartbeat) */
  volatile uint32_t pressed;
  volatile uint32_t raw_x;   /* raw ADC X (12-bit)                      */
  volatile uint32_t raw_y;   /* raw ADC Y (12-bit)                      */
  /* Raw codec sample bytes, updated even when not pressed. */
  volatile uint32_t d_b0;  /* buf[0]  (pen-down flag byte + X high) */
  volatile uint32_t d_b1;  /* buf[1]  (X low)  */
  volatile uint32_t d_b10; /* buf[10] (Y high) */
  volatile uint32_t d_b11; /* buf[11] (Y low)  */
  /* Circle pad ADC (12-bit, about 2048 at rest), sampled whether or not the
   * screen is touched. From core AUDIO_CPAD_VERSION. */
  volatile uint32_t cpad_x;
  volatile uint32_t cpad_y;
} TouchShared;

/* The raw ADC readings at the screen's edges: x_min at pixel column 0, x_max at
 * column 320, and y_min and y_max at rows 0 and 240. A reversed pair means the
 * axis runs the other way. */
typedef struct {
  int16_t x_min, x_max, y_min, y_max;
} TouchCal;

void touch_cal_default(TouchCal *cal);
void touch_cal_get(TouchCal *cal);

/* Returns 0 and changes nothing when the values cannot be a calibration. */
int touch_cal_set(const TouchCal *cal);

int touch_cal_is_default(void);

/* Returns 1 while touched, with the position in screen pixels. Raw ADC values
 * go through rawx/rawy when non-NULL. */
int touch_read(int *sx, int *sy, int *rawx, int *rawy);

/* Returns 1 once, when a new touch begins. Call once per input-loop pass, like
 * get_keys_down(). */
int touch_tap(int *x, int *y);

/* The circle pad with its rest point at 0, +x right and +y up, in raw ADC
 * units. Returns 0 with both at 0 when the running core does not publish it.
 * Raw readings go through rawx/rawy when non-NULL. */
int cpad_read(int *x, int *y, int *rawx, int *rawy);

static inline int touch_in(int tx, int ty, int x, int y, int w, int h) {
  return tx >= x && tx < x + w && ty >= y && ty < y + h;
}

#endif
