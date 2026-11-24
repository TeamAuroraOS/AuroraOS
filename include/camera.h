#ifndef AURORA_CAMERA_H
#define AURORA_CAMERA_H

#include "aurora.h"

/* Shared ARM9/ARM11 camera control block (clear of SystemMenu at 0x233A8000
 * and WifiShared at 0x233B0000). */
#define CAM_SHARED_ADDR 0x233AC000u

/* Scratch YUYV frame (320 x 240 x 2 = 153,600 bytes) + double-buffered
 * 3DS column-major BGR24 output (320 x 240 x 3 = 230,400 bytes each),
 * all inside the idle 0x26E40000..0x26F00000 window below 0x27000000. */
#define CAM_YUYV_ADDR   0x26E40000u
#define CAM_BGR0_ADDR   0x26E80000u
#define CAM_BGR1_ADDR   0x26EC0000u
#define CAM_WIDTH       320u
#define CAM_HEIGHT      240u
#define CAM_FRAME_BYTES (CAM_WIDTH * CAM_HEIGHT * 2u)
#define CAM_FRAME_WORDS (CAM_FRAME_BYTES / 4u)
#define CAM_BGR_BYTES   (CAM_WIDTH * CAM_HEIGHT * 3u)

static inline uint32_t cam_bgr_addr(uint32_t idx) {
  return (idx & 1u) ? CAM_BGR1_ADDR : CAM_BGR0_ADDR;
}

#define CAM_PHOTOS_DIR  "Aurora/Photos"

enum {
  CAM_FRONT = 0, /* Inner (selfie) camera, I2C bus 0 addr 0x7A (7-bit 0x3D) */
  CAM_BACK  = 1, /* Outer (rear right) camera, I2C bus 0 addr 0x78 (7-bit 0x3C) */
};

enum {
  CAM_OP_STOP    = 0,
  CAM_OP_START   = 1,
  CAM_OP_CAPTURE = 2,
};

enum {
  CAM_ST_OFF       = 0,
  CAM_ST_READY     = 1,
  CAM_ST_LIVE      = 2,
  CAM_ST_NO_SIGNAL = 3,
};

typedef struct {
  volatile uint32_t status;      /* CAM_ST_* */
  volatile uint32_t active_cam;  /* CAM_FRONT or CAM_BACK */
  volatile uint32_t chip_id;     /* MT9V113 chip ID (0x2280 if detected) */
  volatile uint32_t frame_seq;   /* incremented on each ready BGR24 frame */
  volatile uint32_t ready_buf;   /* 0 = CAM_BGR0_ADDR, 1 = CAM_BGR1_ADDR */
  volatile uint32_t luma_odd;    /* 0 = YUYV, 1 = UYVY (auto-detected) */
  volatile uint32_t words_read;  /* 32-bit FIFO words read in last frame */
  volatile uint32_t width;
  volatile uint32_t height;
  volatile uint32_t overruns;
  volatile uint32_t p1;          /* current MT9V113 PLL post-divider */
  volatile uint32_t flip_v;      /* 1 = flip vertically */
  volatile uint32_t flip_h;      /* 1 = mirror horizontally */
  volatile uint32_t swap_uv;     /* 1 = swap Cb/Cr (U/V) */
  volatile uint32_t yc_mode;     /* 0 = YUYV, 1 = UYVY, 2 = auto */
  volatile uint32_t app_allowed; /* 1 = C SDK apps allowed to use camera */
} CamShared;

static inline volatile CamShared *cam_shared(void) {
  return (volatile CamShared *)CAM_SHARED_ADDR;
}

/* Built-in Home Menu Camera app (Capture + Gallery). */
void camera_app_run(void);

#endif
