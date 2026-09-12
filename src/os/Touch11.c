/*
 * Touchscreen, read through the CTR codec.
 *
 * Results are published to the ARM9 in the shared block at TOUCH_SHARED_ADDR
 * (include/touch.h); the ARM9 side is Touch9.c.
 */
#include "core11.h"
#include "touch.h"

/* The touchscreen + circle-pad ADC live inside the same CTR codec. The register
 * init sequence (page 0x67) and the raw-data layout (page 0xFB, byte offsets)
 * below are REIMPLEMENTED from the hardware facts in GodMode9's codec driver:
 *   arm11/source/hw/codec.c
 *   Copyright (C) 2017 Sergi Granell, Paul LaMendola
 *   Copyright (C) 2019 Wolfvak, licensed GPL v2-or-later.
 * Only the register addresses / init sequence / data layout are used (hardware
 * facts); the code here is Aurora's own, built on its existing SPI helpers. */

#define CDC(page, off) (((uint16_t)(page) << 8) | (uint16_t)(off))

void touch11_init(void) {
  cdc_write(CDC(0x67, 0x24), 0x98);
  cdc_write(CDC(0x67, 0x26), 0x00);
  cdc_write(CDC(0x67, 0x25), 0x43);
  cdc_write(CDC(0x67, 0x24), 0x18);
  cdc_write(CDC(0x67, 0x17), 0x43);
  cdc_write(CDC(0x67, 0x19), 0x69);
  cdc_write(CDC(0x67, 0x1B), 0x80);
  cdc_write(CDC(0x67, 0x27), 0x11);
  cdc_write(CDC(0x67, 0x26), 0xEC);
  cdc_write(CDC(0x67, 0x24), 0x18);
  cdc_write(CDC(0x67, 0x25), 0x53);
  cdc_mask(CDC(0x67, 0x26), 0x80, 0x80);
  cdc_mask(CDC(0x67, 0x24), 0x00, 0x80);
  cdc_mask(CDC(0x67, 0x25), 0x10, 0x3C);
}

/* Read one raw sample block from the codec and publish touch state. */
static uint32_t g_touch_seq = 0;
void touch11_poll(void) {
  uint8_t buf[52] __attribute__((aligned(4)));
  cdc_read_buf(CDC(0xFB, 1), buf, 52);

  TouchShared *ts = (TouchShared *)TOUCH_SHARED_ADDR;
  ts->d_b0 = buf[0]; /* diagnostics: raw sample bytes, always */
  ts->d_b1 = buf[1];
  ts->d_b10 = buf[10];
  ts->d_b11 = buf[11];
  int pressed = !(buf[0] & 0x10); /* byte0 bit4 low => pen down */
  if (pressed) {
    ts->raw_x = (uint32_t)(((buf[0] << 8) | buf[1]) & 0xFFF);
    ts->raw_y = (uint32_t)(((buf[10] << 8) | buf[11]) & 0xFFF);
    ts->pressed = 1;
  } else {
    ts->pressed = 0;
  }
  ts->seq = ++g_touch_seq;
  dcache_clean(); /* publish to the ARM9 */
}

