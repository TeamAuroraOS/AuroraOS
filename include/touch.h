/*
 * AuroraOS touchscreen (ARM9 <-> ARM11).
 *
 * The 3DS touchscreen is an ADC inside the CTR audio codec, read over the same
 * SPI bus the ARM11 audio core already drives. So the ARM11 core reads raw touch
 * samples and publishes them here in shared FCRAM; the ARM9 converts the raw ADC
 * values to screen pixels (touch_read).
 *
 * The codec touch init + raw-data layout are reimplemented from GodMode9's codec
 * driver (arm11/source/hw/codec.c) -- see the attribution in src/os/audio11.c.
 */
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

#endif /* AURORA_TOUCH_H */
