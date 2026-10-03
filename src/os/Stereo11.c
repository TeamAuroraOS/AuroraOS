/* The top screen's stereoscopic mode and parallax barrier, on the ARM11.
 *
 * Hardware facts, reimplemented here:
 *   - display controller timings for 2D and 3D: libn3ds pdc_presets.h and
 *     gfx.c (profi200, derrek; GPL-3.0), taken from Nintendo's GSP module;
 *   - barrier registers and values: GBATEK "3DS Video LCD Registers" and
 *     libn3ds lcd.c LCD_setParallaxBarrier();
 *   - the New 3DS barrier mask on a TI TCA6416A expander, its enable pin, its
 *     patterns and its polarity alternation: libctru qtm.h / qtmc.h (TuxSH).
 * See docs/stereo3d.md. */
#include "core11.h"
#include "gpu.h"

#define PDC0 0x10400400u
#define PDC_FB_FMT 0x70u

/* Bits 4-6 of the format: 0x20 interleaves buffer A (left eye) with B (right
 * eye) column by column; 0x40 doubles each line, the 2D mode. */
#define FMT_OUT_MASK 0x70u
#define FMT_OUT_AB   0x20u

#define LCD_PARALLAX_CNT 0x10202000u
#define LCD_PARALLAX_PWM 0x10202004u
#define LCD_TOP_FILL     0x10202204u
#define FILL_BLACK       0x01000000u /* bit 24 shows the colour, black */
#define PARALLAX_ON      0x00010001u /* both barrier outputs, PWM driven  */
#define PARALLAX_TIMING  0x0A390A39u /* 2.5 ms on, 2.5 ms off            */

/* The 3D preset: the controller fetches 800 lines a frame, left and right
 * eye alternating, at twice the clock of the line-doubled 2D preset, so a
 * frame still lasts 16.7 ms. Offsets from the controller's base. */
static const struct {
  uint16_t off;
  uint32_t val;
} preset3d[] = {
    {0x00, 450},        {0x04, 209},        {0x08, 449},
    {0x0C, 449},        {0x10, 0},          {0x14, 207},
    {0x18, 209},        {0x1C, 0x01C501C1}, {0x20, 0x00010000},
    {0x24, 827},        {0x28, 2},          {0x2C, 802},
    {0x30, 802},        {0x34, 802},        {0x38, 1},
    {0x3C, 2},          {0x40, 0x03260322}, {0x44, 0},
    {0x48, 0},          {0x4C, 0xFF000000}, {0x5C, 0x019000F0},
    {0x60, 0x01C100D1}, {0x64, 0x03220002}, {0x9C, 0},
};
#define PRESET_N (sizeof(preset3d) / sizeof(preset3d[0]))

/* What the controller ran before 3D, put back for 2D. */
static uint32_t saved[PRESET_N], saved_fmt, saved_fill;
static int in3d;

/* New 3DS mask expander: I2C bus at 0x10161000, device 0x40, reset released
 * by GPIO3 bit 11. Ports 0 and 1 drive the twelve mask units and the
 * common electrode (bit 12). */
#define GPIO3_DAT 0x10147020u
#define GPIO3_DIR 0x10147022u
#define EXP_NRST  (1u << 11)
#define I2C0      0x10161000u
#define I2C_DATA  (I2C0 + 0)
#define I2C_CNT   (I2C0 + 1)
#define EXP_ADDR  0x40u
#define EXP_OUT0  0x02u
#define EXP_DIR0  0x06u

#define I2C_STOP   (1u << 0)
#define I2C_START  (1u << 1)
#define I2C_ERROR  (1u << 2)
#define I2C_ACK    (1u << 4)
#define I2C_READ   (1u << 5)
#define I2C_IRQ    (1u << 6)
#define I2C_ENABLE (1u << 7)
#define I2C_POLLS  200000u

/* The mask is liquid crystal: held at one polarity it would carry a DC
 * voltage, so the pattern and its inverse alternate, as QTM does, about a
 * hundred times a second. Timed by this core's private watchdog, run as a
 * free counter (PERIPHCLK, half the CPU clock): 10 ms at 268 MHz. */
#define WDT_LOAD    0x17E00620u
#define WDT_COUNT   0x17E00624u
#define WDT_CTRL    0x17E00628u
#define WDT_DISABLE 0x17E00634u
#define EXP_PERIOD  1340000u

static int exp_on, exp_phase;
static uint32_t exp_pattern, exp_last;

static int i2c_wait(void) {
  for (uint32_t i = 0; i < I2C_POLLS; i++)
    if (!(MMIO8(I2C_CNT) & I2C_ENABLE))
      return 1;
  return 0;
}

static int i2c_byte(uint8_t data, uint8_t flags) {
  MMIO8(I2C_DATA) = data;
  MMIO8(I2C_CNT) = I2C_ENABLE | I2C_IRQ | flags;
  if (!i2c_wait())
    return 0;
  if (MMIO8(I2C_CNT) & I2C_ACK)
    return 1;
  MMIO8(I2C_CNT) = I2C_ENABLE | I2C_IRQ | I2C_ERROR | I2C_STOP;
  i2c_wait();
  return 0;
}

/* Two bytes from `reg` on: the expander steps through each register pair. */
static int exp_write2(uint8_t reg, uint8_t a, uint8_t b) {
  if (!i2c_wait())
    return 0;
  return i2c_byte(EXP_ADDR, I2C_START) && i2c_byte(reg, 0) &&
         i2c_byte(a, 0) && i2c_byte(b, I2C_STOP);
}

static int exp_read2(uint8_t reg, uint8_t *a, uint8_t *b) {
  if (!i2c_wait() || !i2c_byte(EXP_ADDR, I2C_START) || !i2c_byte(reg, 0) ||
      !i2c_byte(EXP_ADDR | 1u, I2C_START))
    return 0;
  MMIO8(I2C_CNT) = I2C_ENABLE | I2C_IRQ | I2C_READ | I2C_ACK;
  if (!i2c_wait())
    return 0;
  *a = MMIO8(I2C_DATA);
  MMIO8(I2C_CNT) = I2C_ENABLE | I2C_IRQ | I2C_READ | I2C_STOP;
  if (!i2c_wait())
    return 0;
  *b = MMIO8(I2C_DATA);
  return 1;
}

static void exp_put(GpuShared *g, uint32_t bits) {
  if (exp_write2(EXP_OUT0, (uint8_t)bits, (uint8_t)(bits >> 8)))
    g->exp_writes++;
}

/* Releases the expander from reset and makes every pin an output, all low:
 * every unit transparent. 1 if it then reads its directions back. */
static int exp_start(void) {
  uint8_t a = 0xFF, b = 0xFF;

  MMIO16(GPIO3_DIR) |= EXP_NRST;
  MMIO16(GPIO3_DAT) |= EXP_NRST;
  sleep_ms(1);
  if (!exp_write2(EXP_DIR0, 0, 0) || !exp_write2(EXP_OUT0, 0, 0))
    return 0;
  return exp_read2(EXP_DIR0, &a, &b) && a == 0 && b == 0;
}

static void wdt_start(void) {
  MMIO32(WDT_DISABLE) = 0x12345678u; /* out of watchdog mode, if it was in it */
  MMIO32(WDT_DISABLE) = 0x87654321u;
  MMIO32(WDT_CTRL) = 0;
  MMIO32(WDT_LOAD) = 0xFFFFFFFFu;
  MMIO32(WDT_CTRL) = 3u; /* enabled, reloading, timer mode, no prescale */
  exp_last = MMIO32(WDT_COUNT);
}

void stereo11_tick(void) {
  GpuShared *g = (GpuShared *)GPU_SHARED_ADDR;
  uint32_t now;

  if (!exp_on)
    return;
  now = MMIO32(WDT_COUNT);
  if (exp_last - now < EXP_PERIOD) /* it counts down */
    return;
  exp_last = now;
  exp_phase ^= 1;
  exp_put(g, exp_phase ? (exp_pattern ^ 0x1FFFu) : exp_pattern);
}

static void set_preset(int on) {
  vu32 *pdc = (vu32 *)PDC0;
  for (unsigned i = 0; i < PRESET_N; i++)
    pdc[preset3d[i].off / 4u] = on ? preset3d[i].val : saved[i];
  pdc[PDC_FB_FMT / 4u] = on ? ((saved_fmt & ~FMT_OUT_MASK) | FMT_OUT_AB)
                            : saved_fmt;
  dsb();
}

/* Switches the top panel. The caller blanks the panel (stereo11_blank) and
 * waits out a frame or two around this, as GSP does, so the change of timing
 * never reaches the screen half applied. */
void stereo11_mode(int on) {
  vu32 *pdc = (vu32 *)PDC0;
  if (on == in3d)
    return;
  if (on) {
    for (unsigned i = 0; i < PRESET_N; i++)
      saved[i] = pdc[preset3d[i].off / 4u];
    saved_fmt = pdc[PDC_FB_FMT / 4u];
  }
  set_preset(on);
  in3d = on;
}

int stereo11_is3d(void) { return in3d; }

void stereo11_blank(int on) {
  if (on) {
    saved_fill = MMIO32(LCD_TOP_FILL);
    MMIO32(LCD_TOP_FILL) = FILL_BLACK;
  } else {
    MMIO32(LCD_TOP_FILL) = saved_fill & ~FILL_BLACK;
  }
  dsb();
}

/* The barrier after the mode, and off before it, so it never darkens a 2D
 * picture. */
void stereo11_barrier(GpuShared *g, uint32_t flags, uint32_t pattern) {
  int on = (flags & GPU_STEREO_ON) != 0;

  if (on && (flags & GPU_STEREO_BARRIER)) {
    MMIO32(LCD_PARALLAX_PWM) = PARALLAX_TIMING;
    MMIO32(LCD_PARALLAX_CNT) = PARALLAX_ON;
  } else {
    MMIO32(LCD_PARALLAX_CNT) = 0;
  }

  if (on && (flags & GPU_STEREO_EXPANDER)) {
    if (!exp_on) {
      if (exp_start())
        g->stereo |= 0x100u;
      else
        g->stereo &= ~0x100u;
      wdt_start();
    }
    exp_pattern = pattern & 0xFFFu;
    exp_phase = 0;
    exp_on = 1;
    exp_put(g, exp_pattern);
  } else if (exp_on) {
    exp_on = 0;
    exp_put(g, 0); /* every unit transparent, no voltage across any */
  }
  dsb();
}

/* For a core about to be parked: 2D, barrier off, mask clear. */
void stereo11_off(void) {
  GpuShared *g = (GpuShared *)GPU_SHARED_ADDR;
  stereo11_barrier(g, 0, 0);
  if (in3d) {
    stereo11_blank(1);
    stereo11_mode(0);
    sleep_ms(20);
    stereo11_blank(0);
  }
  g->stereo = 0;
}
