/* Model, 3D slider and 3D LED from the MCU; the ARM11 does the display side
 * (Stereo11.c). MCU registers from GBATEK "3DS I2C MCU Registers" and libn3ds
 * mcu_regmap.h; the New 3DS barrier patterns from libctru qtmc.h. See
 * docs/stereo3d.md. */
#include "stereo.h"
#include "gpu.h"
#include "i2c.h"

#define MCU_SLIDER_3D 0x08u
#define MCU_LED_3D    0x2Cu
#define MCU_RAW_STATE 0x7Fu /* 19 bytes; byte 9 is the system model */

/* The factory calibration (HWCAL "SVR2" bounds) puts the bottom of the slider
 * at or below 0x07..0x1B and the top at or above 0xF2..0xFD; without it these
 * bounds cover every console in that range. */
#define SLIDER_OFF  0x20
#define SLIDER_FULL 0xF0

static int model = -1;
static int on;

StereoModel stereo_model(void) {
  if (model < 0) {
    u8 raw[19];
    model = I2C_readRegBuf(I2C_DEV_MCU, MCU_RAW_STATE, raw, sizeof(raw)) &&
                    raw[9] <= STEREO_MODEL_N2DS_XL
                ? raw[9]
                : STEREO_MODEL_UNKNOWN;
  }
  return (StereoModel)model;
}

const char *stereo_model_name(StereoModel m) {
  static const char *const names[] = {"3DS",    "3DS XL",    "New 3DS",
                                      "2DS",    "New 3DS XL", "New 2DS XL"};
  return m <= STEREO_MODEL_N2DS_XL ? names[m] : "unknown model";
}

static int is_new(StereoModel m) {
  return m == STEREO_MODEL_N3DS || m == STEREO_MODEL_N3DS_XL;
}

int stereo_capable(void) {
  StereoModel m = stereo_model();
  return m == STEREO_MODEL_3DS || m == STEREO_MODEL_3DS_XL || is_new(m);
}

u8 stereo_slider_raw(void) {
  u8 v = 0;
  if (!I2C_readRegBuf(I2C_DEV_MCU, MCU_SLIDER_3D, &v, 1))
    return 0;
  return v;
}

int stereo_slider(u8 raw) {
  if (raw <= SLIDER_OFF)
    return 0;
  if (raw >= SLIDER_FULL)
    return 256;
  return ((int)raw - SLIDER_OFF) * 256 / (SLIDER_FULL - SLIDER_OFF);
}

/* Twelve mask units, the leftmost in bit 11; 1 is opaque. */
static u32 pattern(int pos) {
  static const u16 ss3d[STEREO_POSITIONS] = {
      0x0FC, 0x07E, 0x03F, 0x81F, 0xC0F, 0xE07,
      0xF03, 0xF81, 0xFC0, 0x7E0, 0x3F0, 0x1F8,
  };
  if (pos < 0 || pos >= STEREO_POSITIONS)
    return 0xF07; /* 111100000111: slits one unit narrower than the above */
  return ss3d[pos];
}

int stereo_enable(int want, int pos) {
  StereoModel m = stereo_model();
  u32 flags;

  if (want && !stereo_capable())
    return 0;
  flags = want ? (GPU_STEREO_ON | GPU_STEREO_BARRIER |
                  (is_new(m) ? GPU_STEREO_EXPANDER : 0))
               : 0;
  if (!gpu_stereo(flags, pattern(pos)))
    return 0;
  /* Only the Old 3DS and 3DS XL have the LED. */
  if (m == STEREO_MODEL_3DS || m == STEREO_MODEL_3DS_XL)
    I2C_writeReg(I2C_DEV_MCU, MCU_LED_3D, want ? 1 : 0);
  on = want;
  return 1;
}

int stereo_is_on(void) { return on; }
