#ifndef AURORA_STEREO_H
#define AURORA_STEREO_H

#include "aurora.h"

/* The system model the MCU reports (register 0x7F, byte 9). */
typedef enum {
  STEREO_MODEL_3DS = 0,
  STEREO_MODEL_3DS_XL,
  STEREO_MODEL_N3DS,
  STEREO_MODEL_2DS,
  STEREO_MODEL_N3DS_XL,
  STEREO_MODEL_N2DS_XL,
  STEREO_MODEL_UNKNOWN = 0xFF,
} StereoModel;

/* Read once from the MCU, then remembered. */
StereoModel stereo_model(void);
const char *stereo_model_name(StereoModel m);

/* 1 when the model has a 3D screen: not the 2DS or New 2DS XL. */
int stereo_capable(void);

/* The 3D slider as the MCU's ADC reads it, 0..255, and that reading mapped to
 * 0 (off) .. 256 (top). */
u8 stereo_slider_raw(void);
int stereo_slider(u8 raw);

/* Barrier positions on a New 3DS: STEREO_POS_FIXED is the pattern QTM uses
 * with "super-stable 3D" off; 0..11 are its head-tracking positions. */
#define STEREO_POS_FIXED (-1)
#define STEREO_POSITIONS 12

/* The top screen in 3D (stereo framebuffers, barrier on, the Old 3DS's 3D LED)
 * or back in 2D. Returns 1 if the GPU took the request. */
int stereo_enable(int on, int pos);
int stereo_is_on(void);

#endif
