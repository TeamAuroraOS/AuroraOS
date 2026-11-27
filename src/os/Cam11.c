// SPDX-License-Identifier: GPL-2.0
/*
 * ARM11 3DS Camera capture driver for AuroraOS.
 * Ported from drivers/platform/nintendo3ds/ctr_cam.c and
 * camera/libcameraservice/CameraHardwareN3ds.cpp in
 * https://github.com/Octoblimp/Android-Eclair-3DS (N3DS_CAM_CAPTURE).
 *
 * Streams 320x240 YUYV/UYVY asynchronously in cam11_tick(), auto-detects
 * luma byte order (even vs odd), and converts completed frames on ARM11
 * directly into double-buffered 3DS column-major BGR24 (CAM_BGR0/1_ADDR)
 * so ARM9 stays 100% responsive with zero capture lag.
 */
#include "camera.h"
#include "core11.h"

/* ---- Hardware registers ------------------------------------------------- */
#define PDN_CAM_CNT           0x10141224u
#define PDN_CAM_CLOCK_ENABLE  (1u << 0)

#define CAM0_BASE             0x10120000u
#define CAM0_FIFO             0x10320000u
#define CAM_FIFO_WINDOW       0x80u

#define CAM_CNT               (CAM0_BASE + 0x000u)
#define CAM_CNT_MUST_BE_1     (1u << 4)
#define CAM_CNT_XFER          (1u << 15)
#define CAM_STAT              (CAM0_BASE + 0x004u)
#define CAM_STAT_FILL         0x07FFu
#define CAM_STAT_OVERRUN      (1u << 15)
#define CAM_CTL2              (CAM0_BASE + 0x006u)
#define CAM_CTL2_CLR_OVERRUN  (1u << 15)
#define CAM_TRIM_X0           (CAM0_BASE + 0x010u)
#define CAM_TRIM_Y0           (CAM0_BASE + 0x012u)
#define CAM_TRIM_X1           (CAM0_BASE + 0x014u)
#define CAM_TRIM_Y1           (CAM0_BASE + 0x016u)

/* MT9V113 registers & MCU variables */
#define MT9V113_CHIP_VERSION        0x2280u
#define MT9V113_REG_CHIP_VERSION    0x0000u
#define MT9V113_REG_STANDBY_CONTROL 0x0018u
#define MT9V113_STANDBY             (1u << 0)
#define MT9V113_STANDBY_DONE        (1u << 14)
#define MT9V113_REG_RESET_MISC      0x001Au
#define MT9V113_PARALLEL_ENABLE     (1u << 9)
#define MT9V113_REG_CORE_301A       0x301Au
#define MT9V113_CORE_AWAKE          (1u << 2)
#define MT9V113_REG_MCU_ADDRESS     0x098Cu
#define MT9V113_REG_MCU_DATA_0      0x0990u
#define MT9V113_VAR_SEQ_CMD         0xA103u
#define MT9V113_VAR_SEQ_STATE       0xA104u
#define MT9V113_SEQ_CMD_REFRESH     5u
#define MT9V113_SEQ_CMD_REFRESH_MOD 6u
#define MT9V113_VAR_AE_MAX_INDEX    0xA20Cu

/* Use the exact same MT9V113 read mode & ISP color registers on both sensors
 * (0x0025) so the Front sensor's Bayer CFA phase matches the Back sensor's
 * working color pipeline; handle horizontal orientation in software via flip_h. */
#define CAM_READ_MODE_COMMON  0x0025u
#define CAM_AE_MAX_INDEX      12u
#define CAM_P1_MAX            7u
#define CAM_OVERRUN_SLOWDOWN  3u
#define CAM_GAP_US            3000u   /* >= 3 ms empty FIFO = VBlank gap */
#define CAM_SENSOR_WAIT_US    250000u /* 250 ms max for sensor state poll */

/* I2C bus 0 (0x10161000) for 16-bit big-endian MT9V113 access */
#define I2C1_BASE             0x10161000u
#define I2C1_DATA             MMIO8(I2C1_BASE + 0x00u)
#define I2C1_CNT              MMIO8(I2C1_BASE + 0x01u)
#define I2C1_CNTEX            MMIO16(I2C1_BASE + 0x02u)
#define I2C1_SCL              MMIO16(I2C1_BASE + 0x04u)
#define I2C_STOP              (1u << 0)
#define I2C_START             (1u << 1)
#define I2C_ERR               (1u << 2)
#define I2C_ACK               (1u << 4)
#define I2C_READ              (1u << 5)
#define I2C_IRQ               (1u << 6)
#define I2C_EN                (1u << 7)

static uint32_t g_win;
static uint8_t  g_p1;
static uint8_t  g_p1_learned;
static uint32_t g_active_cam;
static int      g_streaming;

/* Async framing state (owned by cam11_tick) */
static uint32_t g_pos;
static uint32_t g_since_gap;
static uint32_t g_last_data_us;
static uint32_t g_consec_overruns;
static int      g_need_gap;
static int      g_gapless;

/* Byte-order auto-detection (from CameraHardwareN3ds.cpp) */
static int      g_luma_odd;

/* ---- I2C bus 0 helpers (time-bounded via core11_now_us) ----------------- */

static int i2c1_wait(void) {
  uint32_t t0 = core11_now_us();
  while (I2C1_CNT & I2C_EN) {
    if (core11_now_us() - t0 > 20000u)
      return 0;
  }
  return 1;
}

static void i2c1_init(void) {
  if (!i2c1_wait())
    return;
  I2C1_CNTEX = 2u;
  I2C1_SCL = 1280u;
}

static int i2c1_byte_w(uint8_t val, uint8_t flags) {
  I2C1_DATA = val;
  I2C1_CNT = (uint8_t)(I2C_EN | I2C_IRQ | flags);
  if (!i2c1_wait())
    return 0;
  return (I2C1_CNT & I2C_ACK) != 0;
}

static int mt9_write16(uint8_t dev, uint16_t reg, uint16_t val) {
  for (int tries = 0; tries < 4; tries++) {
    if (!i2c1_wait())
      return 0;
    if (i2c1_byte_w(dev, I2C_START) &&
        i2c1_byte_w((uint8_t)(reg >> 8), 0) &&
        i2c1_byte_w((uint8_t)(reg & 0xFFu), 0) &&
        i2c1_byte_w((uint8_t)(val >> 8), 0) &&
        i2c1_byte_w((uint8_t)(val & 0xFFu), I2C_STOP))
      return 1;
    I2C1_CNT = I2C_EN | I2C_IRQ | I2C_ERR | I2C_STOP;
    i2c1_wait();
  }
  return 0;
}

static int mt9_read16(uint8_t dev, uint16_t reg, uint16_t *out) {
  for (int tries = 0; tries < 4; tries++) {
    if (!i2c1_wait())
      return 0;
    if (!i2c1_byte_w(dev, I2C_START) ||
        !i2c1_byte_w((uint8_t)(reg >> 8), 0) ||
        !i2c1_byte_w((uint8_t)(reg & 0xFFu), 0) ||
        !i2c1_byte_w((uint8_t)(dev | 1u), I2C_START)) {
      I2C1_CNT = I2C_EN | I2C_IRQ | I2C_ERR | I2C_STOP;
      i2c1_wait();
      continue;
    }
    I2C1_CNT = I2C_EN | I2C_IRQ | I2C_READ | I2C_ACK;
    if (!i2c1_wait())
      return 0;
    uint8_t hi = I2C1_DATA;
    I2C1_CNT = I2C_EN | I2C_IRQ | I2C_READ | I2C_STOP;
    if (!i2c1_wait())
      return 0;
    uint8_t lo = I2C1_DATA;
    *out = (uint16_t)(((uint16_t)hi << 8) | lo);
    return 1;
  }
  return 0;
}

/* ---- Aptina MT9V113 register/MCU helpers (from ctr_cam.c) --------------- */

typedef struct {
  uint8_t dev;
  int     err;
} AptCtx;

static uint16_t apt_rd(AptCtx *a, uint16_t reg) {
  uint16_t val = 0;
  if (a->err)
    return 0;
  if (!mt9_read16(a->dev, reg, &val))
    a->err = 1;
  return val;
}

static void apt_wr(AptCtx *a, uint16_t reg, uint16_t val) {
  if (a->err)
    return;
  if (!mt9_write16(a->dev, reg, val))
    a->err = 1;
}

static void apt_set(AptCtx *a, uint16_t reg, uint16_t bits) {
  uint16_t val = apt_rd(a, reg);
  apt_wr(a, reg, (uint16_t)(val | bits));
}

static void apt_clr(AptCtx *a, uint16_t reg, uint16_t bits) {
  uint16_t val = apt_rd(a, reg);
  apt_wr(a, reg, (uint16_t)(val & (uint16_t)~bits));
}

static void apt_wait(AptCtx *a, uint16_t reg, uint16_t mask, int expect_set) {
  uint32_t t0 = core11_now_us();
  for (;;) {
    uint16_t val = apt_rd(a, reg);
    if (a->err || (((val & mask) != 0) == (expect_set != 0)))
      return;
    if (core11_now_us() - t0 > CAM_SENSOR_WAIT_US)
      return;
    sleep_ms(1);
  }
}

static void apt_mcu_wr(AptCtx *a, uint16_t var, uint16_t val) {
  apt_wr(a, MT9V113_REG_MCU_ADDRESS, var);
  apt_wr(a, MT9V113_REG_MCU_DATA_0, val);
}

static uint16_t apt_mcu_rd(AptCtx *a, uint16_t var) {
  apt_wr(a, MT9V113_REG_MCU_ADDRESS, var);
  return apt_rd(a, MT9V113_REG_MCU_DATA_0);
}

static void apt_mcu_set(AptCtx *a, uint16_t var, uint16_t bits) {
  uint16_t val = apt_mcu_rd(a, var);
  apt_mcu_wr(a, var, (uint16_t)(val | bits));
}

static void apt_seq(AptCtx *a, uint16_t cmd) {
  uint32_t t0 = core11_now_us();
  apt_mcu_wr(a, MT9V113_VAR_SEQ_CMD, cmd);
  for (;;) {
    uint16_t val = apt_mcu_rd(a, MT9V113_VAR_SEQ_CMD);
    if (a->err || !(val & 0x0Fu))
      return;
    if (core11_now_us() - t0 > CAM_SENSOR_WAIT_US)
      return;
    sleep_ms(1);
  }
}

static void cam_sensor_park(uint8_t dev) {
  AptCtx a = { dev, 0 };
  apt_clr(&a, MT9V113_REG_RESET_MISC, MT9V113_PARALLEL_ENABLE);
  apt_set(&a, MT9V113_REG_STANDBY_CONTROL, MT9V113_STANDBY);
  apt_wait(&a, MT9V113_REG_STANDBY_CONTROL, MT9V113_STANDBY_DONE, 1);
  apt_wait(&a, MT9V113_REG_CORE_301A, MT9V113_CORE_AWAKE, 0);
}

static int cam_sensor_start(uint8_t dev, int inner, uint32_t width,
                            uint32_t height, uint8_t p1) {
  AptCtx a = { dev, 0 };
  (void)inner;
  uint16_t read_mode = CAM_READ_MODE_COMMON;

  /* reset */
  apt_wr(&a, 0x001Au, 0x0003u);
  apt_wr(&a, 0x001Au, 0x0000u);
  sleep_ms(2);

  /* wakeup */
  apt_wr(&a, 0x0018u, 0x4028u);
  apt_wr(&a, 0x001Eu, 0x0201u);
  apt_wr(&a, 0x0016u, 0x42DFu);
  apt_wait(&a, MT9V113_REG_STANDBY_CONTROL, MT9V113_STANDBY_DONE, 0);
  apt_wait(&a, MT9V113_REG_CORE_301A, MT9V113_CORE_AWAKE, 1);

  apt_mcu_wr(&a, 0x02F0u, 0x0000u);
  apt_mcu_wr(&a, 0x02F2u, 0x0210u);
  apt_mcu_wr(&a, 0x02F4u, 0x001Au);
  apt_mcu_wr(&a, 0x2145u, 0x02F4u);
  apt_mcu_wr(&a, 0xA134u, 0x0001u);
  apt_mcu_set(&a, 0xA115u, 0x0002u);

  /* YUV422, luma first: Y0 Cb Y1 Cr */
  apt_mcu_wr(&a, 0x2755u, 0x0002u);
  apt_mcu_wr(&a, 0x2757u, 0x0002u);

  /* PLL */
  apt_wr(&a, 0x0014u, 0x2145u);
  apt_wr(&a, 0x0010u, 0x0111u);
  apt_wr(&a, 0x0012u, (uint16_t)(p1 & 0x0Fu));
  apt_wr(&a, 0x0014u, 0x244Bu);
  apt_wr(&a, 0x0014u, 0x304Bu);
  apt_wait(&a, 0x0014u, (1u << 15), 1);
  apt_clr(&a, 0x0014u, 0x0001u);

  /* Context A = requested size; Context B = 640x480 */
  apt_mcu_wr(&a, 0x2703u, (uint16_t)width);
  apt_mcu_wr(&a, 0x2705u, (uint16_t)height);
  apt_mcu_wr(&a, 0x2707u, 0x0280u);
  apt_mcu_wr(&a, 0x2709u, 0x01E0u);

  apt_mcu_wr(&a, 0x2715u, 0x0001u);
  apt_mcu_wr(&a, 0x2719u, 0x001Au);
  apt_mcu_wr(&a, 0x271Bu, 0x006Bu);
  apt_mcu_wr(&a, 0x271Du, 0x006Bu);
  apt_mcu_wr(&a, 0x271Fu, 0x02C0u);
  apt_mcu_wr(&a, 0x2721u, 0x034Bu);
  apt_mcu_wr(&a, MT9V113_VAR_AE_MAX_INDEX, CAM_AE_MAX_INDEX);
  apt_mcu_wr(&a, 0x272Bu, 0x0001u);
  apt_mcu_wr(&a, 0x272Fu, 0x001Au);
  apt_mcu_wr(&a, 0x2731u, 0x006Bu);
  apt_mcu_wr(&a, 0x2733u, 0x006Bu);
  apt_mcu_wr(&a, 0x2735u, 0x02C0u);
  apt_mcu_wr(&a, 0x2737u, 0x034Bu);

  apt_set(&a, 0x3210u, 0x0008u);
  apt_mcu_wr(&a, 0xA208u, 0x0000u);
  apt_mcu_wr(&a, 0xA24Cu, 0x0020u);
  apt_mcu_wr(&a, 0xA24Fu, 0x0070u);
  apt_mcu_wr(&a, 0x2717u, read_mode);
  apt_mcu_wr(&a, 0x272Du, read_mode);
  apt_mcu_wr(&a, 0xA202u, 0x0000u);
  apt_mcu_wr(&a, 0xA203u, 0x00FFu);
  apt_set(&a, 0x0016u, 0x0020u);
  apt_mcu_wr(&a, 0xA115u, 0x0072u);
  apt_mcu_wr(&a, 0xA11Fu, 0x0001u);
  apt_wr(&a, 0x326Cu, 0x1000u);
  apt_mcu_wr(&a, 0xAB22u, 0x0002u);
  apt_seq(&a, MT9V113_SEQ_CMD_REFRESH_MOD);
  apt_seq(&a, MT9V113_SEQ_CMD_REFRESH);

  /* Activate & enable parallel pixel bus */
  apt_clr(&a, MT9V113_REG_STANDBY_CONTROL, MT9V113_STANDBY);
  apt_wait(&a, MT9V113_REG_STANDBY_CONTROL, MT9V113_STANDBY_DONE, 0);
  apt_wait(&a, MT9V113_REG_CORE_301A, MT9V113_CORE_AWAKE, 1);
  apt_set(&a, MT9V113_REG_RESET_MISC, MT9V113_PARALLEL_ENABLE);

  return !a.err;
}

/* ---- CAM0 receiver helpers ---------------------------------------------- */

static inline uint32_t cam_fifo_word(void) {
  uint32_t w = MMIO32(CAM0_FIFO + g_win);
  g_win = (g_win + 4u) & (CAM_FIFO_WINDOW - 1u);
  return w;
}

static void cam_fifo_flush(void) {
  uint32_t guard = 0x4000u;
  uint16_t n;
  while (guard && (n = (uint16_t)(MMIO16(CAM_STAT) & CAM_STAT_FILL))) {
    while (n-- && guard--)
      (void)cam_fifo_word();
  }
}

static void cam_port_stop(void) {
  MMIO16(CAM_CNT) = (uint16_t)(MMIO16(CAM_CNT) & (uint16_t)~CAM_CNT_XFER);
  dsb();
  cam_fifo_flush();
  MMIO16(CAM_CTL2) = (uint16_t)(MMIO16(CAM_CTL2) | CAM_CTL2_CLR_OVERRUN);
  dsb();
}

static void cam_port_reset(void) {
  MMIO16(CAM_CNT) = 0u;
  dsb();
  sleep_ms(2);
  MMIO16(CAM_CNT) = CAM_CNT_MUST_BE_1;
  dsb();
  sleep_ms(2);
  cam_fifo_flush();
  MMIO16(CAM_CTL2) = (uint16_t)(MMIO16(CAM_CTL2) | CAM_CTL2_CLR_OVERRUN);
  dsb();
}

static void cam_port_start(void) {
  MMIO16(CAM_CNT) = CAM_CNT_MUST_BE_1;
  dsb();
  cam_fifo_flush();
  MMIO16(CAM_CTL2) = (uint16_t)(MMIO16(CAM_CTL2) | CAM_CTL2_CLR_OVERRUN);
  MMIO16(CAM_TRIM_X0) = 0u;
  MMIO16(CAM_TRIM_Y0) = 0u;
  MMIO16(CAM_TRIM_X1) = (uint16_t)((CAM_WIDTH - 2u) & 0x03FEu);
  MMIO16(CAM_TRIM_Y1) = (uint16_t)((CAM_HEIGHT - 1u) & 0x01FFu);
  dsb();
  MMIO16(CAM_CNT) = (uint16_t)(CAM_CNT_MUST_BE_1 | CAM_CNT_XFER);
  dsb();
}

static uint8_t cam_dev_addr(uint32_t which) {
  return (which == CAM_BACK) ? 0x78u : 0x7Au;
}

/* ---- YUYV/UYVY auto-order & ARM11 BGR24 column-major conversion --------- */

/* Ported from CameraHardwareN3ds::decideOrder(): luma swings far from 128
 * while chroma stays close to 128, so even vs odd MSD reliably tells YUYV
 * apart from UYVY. Re-evaluated each frame (~20 us) so dark startup frames
 * can never lock in the wrong byte order. */
static void cam_decide_order(const uint32_t *words) {
  uint32_t se = 0, so = 0, n = 0;
  for (uint32_t i = 0; i < CAM_FRAME_WORDS; i += 37u) {
    uint32_t w = words[i];
    int a = (int)(w & 0xFFu) - 128;
    int b = (int)((w >> 8) & 0xFFu) - 128;
    int c = (int)((w >> 16) & 0xFFu) - 128;
    int d = (int)((w >> 24) & 0xFFu) - 128;
    se += (uint32_t)(a * a + c * c);
    so += (uint32_t)(b * b + d * d);
    n++;
  }
  if (n < 64u)
    return;

  uint32_t floor_v = n * 150u;
  if (se > so + (so >> 2) && se > floor_v)
    g_luma_odd = 0;
  else if (so > se + (se >> 2) && so > floor_v)
    g_luma_odd = 1;
}

static inline uint8_t clamp8(int v) {
  if (v < 0) return 0u;
  if (v > 255) return 255u;
  return (uint8_t)v;
}

/* Convert 320x240 YUYV/UYVY into 3DS column-major BGR24 on ARM11. */
static void cam_deliver_frame(void) {
  volatile CamShared *cs = cam_shared();
  const uint32_t *src = (const uint32_t *)CAM_YUYV_ADDR;

  uint32_t yc_mode = cs->yc_mode;
  int odd;
  if (yc_mode == 0u)
    odd = 0;
  else if (yc_mode == 1u)
    odd = 1;
  else {
    cam_decide_order(src);
    odd = g_luma_odd;
  }
  cs->luma_odd = (uint32_t)odd;

  int flip_v  = (cs->flip_v != 0u);
  int flip_h  = (cs->flip_h != 0u);
  int swap_uv = (cs->swap_uv != 0u);

  uint32_t wbuf = (cs->ready_buf ^ 1u) & 1u;
  uint8_t *dst = (uint8_t *)cam_bgr_addr(wbuf);
  const int32_t col_step = flip_h ? -(int32_t)(CAM_HEIGHT * 3u)
                                  :  (int32_t)(CAM_HEIGHT * 3u);

  for (uint32_t y = 0; y < CAM_HEIGHT; y++) {
    uint32_t row_off = (flip_v ? y : (CAM_HEIGHT - 1u - y)) * 3u;
    const uint32_t *srow = src + y * (CAM_WIDTH / 2u);
    uint8_t *col = dst + row_off +
                   (flip_h ? (CAM_WIDTH - 1u) * CAM_HEIGHT * 3u : 0u);

    for (uint32_t x = 0; x < CAM_WIDTH / 2u; x++) {
      uint32_t w = srow[x];
      int y0, y1, u, v;
      if (odd) {
        u  = (int)(w & 0xFFu) - 128;
        y0 = (int)((w >> 8) & 0xFFu);
        v  = (int)((w >> 16) & 0xFFu) - 128;
        y1 = (int)((w >> 24) & 0xFFu);
      } else {
        y0 = (int)(w & 0xFFu);
        u  = (int)((w >> 8) & 0xFFu) - 128;
        y1 = (int)((w >> 16) & 0xFFu);
        v  = (int)((w >> 24) & 0xFFu) - 128;
      }
      if (swap_uv) {
        int tmp = u;
        u = v;
        v = tmp;
      }

      /* JFIF full-range YCbCr -> BGR */
      int rv = (359 * v) >> 8;
      int gv = -((88 * u + 183 * v) >> 8);
      int bv = (454 * u) >> 8;

      col[0] = clamp8(y0 + bv);
      col[1] = clamp8(y0 + gv);
      col[2] = clamp8(y0 + rv);
      col += col_step;

      col[0] = clamp8(y1 + bv);
      col[1] = clamp8(y1 + gv);
      col[2] = clamp8(y1 + rv);
      col += col_step;
    }
  }

  dsb();
  cs->ready_buf = wbuf;
  cs->words_read = CAM_FRAME_WORDS;
  cs->status = CAM_ST_LIVE;
  cs->frame_seq = cs->frame_seq + 1u;
  dsb();
}

static void cam_reset_framing(void) {
  g_pos = 0;
  g_since_gap = 0;
  g_need_gap = 1;
  g_consec_overruns = 0;
  g_last_data_us = core11_now_us();
}

/* ---- Top-level commands & async tick ------------------------------------ */

static void cam_stop_hw(void) {
  volatile CamShared *cs = cam_shared();
  g_streaming = 0;
  cam_port_stop();
  if (cs->chip_id) {
    cam_sensor_park(0x7Au);
    cam_sensor_park(0x78u);
  }
  MMIO8(PDN_CAM_CNT) &= (uint8_t)~PDN_CAM_CLOCK_ENABLE;
  dsb();
  cs->status = CAM_ST_OFF;
}

static void cam_start_hw(uint32_t which) {
  volatile CamShared *cs = cam_shared();
  uint8_t dev = cam_dev_addr(which);
  uint8_t other = cam_dev_addr(which == CAM_FRONT ? CAM_BACK : CAM_FRONT);
  uint16_t id = 0;

  g_streaming = 0;
  cs->active_cam = which;
  cs->width = CAM_WIDTH;
  cs->height = CAM_HEIGHT;
  cs->chip_id = 0;
  cs->words_read = 0;
  cs->overruns = 0;
  /* With read mode 0x0025 both sensors send their top row first and Cb before
   * Cr; the back picture comes the right way round and the front one as a
   * mirror would show it, so the conversion changes nothing. */
  cs->flip_v  = 0u;
  cs->flip_h  = 0u;
  cs->swap_uv = 0u;
  cs->yc_mode = 2u;
  g_active_cam = which;
  g_p1 = g_p1_learned;
  cs->p1 = g_p1;
  g_gapless = 0;
  g_luma_odd = 0;

  i2c1_init();

  MMIO8(PDN_CAM_CNT) |= PDN_CAM_CLOCK_ENABLE;
  dsb();
  sleep_ms(2);

  cam_port_stop();
  cam_sensor_park(other);

  if (!mt9_read16(dev, MT9V113_REG_CHIP_VERSION, &id) ||
      id == 0u || id == 0xFFFFu) {
    cs->status = CAM_ST_NO_SIGNAL;
    return;
  }
  cs->chip_id = id;

  cam_port_reset();
  if (!cam_sensor_start(dev, which == CAM_FRONT, CAM_WIDTH, CAM_HEIGHT, g_p1)) {
    cam_sensor_park(dev);
    cs->status = CAM_ST_NO_SIGNAL;
    return;
  }

  cam_reset_framing();
  cam_port_start();
  g_streaming = 1;
  cs->status = CAM_ST_READY;
}

/* Called every pass of Core11's main loop. Drains CAM0 FIFO in tight bursts
 * whenever data is present and publishes completed BGR24 frames async. */
void cam11_tick(void) {
  if (!g_streaming)
    return;

  volatile CamShared *cs = cam_shared();
  uint32_t *yuyv = (uint32_t *)CAM_YUYV_ADDR;
  uint32_t budget_t0 = core11_now_us();

  for (;;) {
    uint16_t stat = MMIO16(CAM_STAT);

    if (stat & CAM_STAT_OVERRUN) {
      cam_port_stop();
      cs->overruns = cs->overruns + 1u;
      if (++g_consec_overruns >= CAM_OVERRUN_SLOWDOWN && g_p1 < CAM_P1_MAX) {
        g_p1++;
        g_p1_learned = g_p1;
        cs->p1 = g_p1;
        cam_port_reset();
        cam_sensor_start(cam_dev_addr(g_active_cam), g_active_cam == CAM_FRONT,
                         CAM_WIDTH, CAM_HEIGHT, g_p1);
        g_consec_overruns = 0;
      }
      cam_reset_framing();
      cam_port_start();
      return;
    }

    uint32_t n = (uint32_t)(stat & CAM_STAT_FILL);
    if (n) {
      g_since_gap += n;
      if (g_need_gap) {
        while (n--)
          (void)cam_fifo_word();
        if (!g_gapless && g_since_gap > 2u * CAM_FRAME_WORDS) {
          g_gapless = 1;
          g_need_gap = 0;
          g_pos = 0;
        }
      } else {
        uint32_t rem = CAM_FRAME_WORDS - g_pos;
        uint32_t take = (n < rem) ? n : rem;
        for (uint32_t i = 0; i < take; i++)
          yuyv[g_pos++] = cam_fifo_word();
        uint32_t extra = n - take;
        while (extra--)
          (void)cam_fifo_word();

        if (g_pos >= CAM_FRAME_WORDS) {
          cam_deliver_frame();
          g_consec_overruns = 0;
          g_pos = 0;
          g_need_gap = !g_gapless;
          g_last_data_us = core11_now_us();
          return;
        }
      }
      g_last_data_us = core11_now_us();
      if (core11_now_us() - budget_t0 > 1500u)
        return;
      continue;
    }

    /* FIFO empty: check VBlank gap */
    if (g_since_gap && core11_now_us() - g_last_data_us >= CAM_GAP_US) {
      g_since_gap = 0;
      g_pos = 0;
      g_need_gap = 0;
    }
    return;
  }
}

void cam11_command(uint32_t op, uint32_t arg1) {
  if (op == CAM_OP_START)
    cam_start_hw(arg1);
  else if (op == CAM_OP_STOP)
    cam_stop_hw();
}
