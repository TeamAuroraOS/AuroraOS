/* Camera API for AuroraOS C SDK apps (sdk/include/aurora_app.h).
 * Enforces Settings > Camera permission (default OFF): if an app tries to
 * start the camera while it is off in Settings, shows the "Camera Off"
 * pop-up dialog and returns false without activating the sensor or LED. */
#include "app_internal.h"
#include "audio.h"
#include "camera.h"
#include "ff.h"
#include "i2c.h"
#include "user.h"

extern void os_cache_sync(void);
extern void os_dcache_clean_range(const void *addr, u32 len);

static bool g_cam_active;

static bool cam_perm_enabled(void) {
  if (app_core()) {
    os_cache_sync();
    if (cam_shared()->app_allowed)
      return true;
  }
  const u8 *ud = app_user_dat();
  if (ud && (ud[USER_DAT_CAMERA] & 1u))
    return true;
  return false;
}

bool cam_allowed(void) {
  return cam_perm_enabled();
}

void cam_set_led(bool on) {
  if (on && !cam_perm_enabled())
    return;
  I2C_writeReg(I2C_DEV_MCU, 0x2Bu, on ? 2u : 0u);
}

/* Modal pop-up shown on the bottom screen when an app tries to use the camera
 * while Settings > Camera is turned off. */
static void cam_popup_center(int y, GfxFont font, u32 color, const char *s) {
  int w = gfx_text_width(font, s);
  gfx_print(SCREEN_BOTTOM, (SCREEN_BOTTOM_WIDTH - w) / 2, y, font, color, "%s", s);
}

static void cam_show_off_popup(void) {
  const int bx = 22, by = 44, bw = 276, bh = 152;
  const int ok_x = 104, ok_y = 154, ok_w = 112, ok_h = 30;
  const u32 accent = sys_accent();
  const u32 bg_dim = 0x0C1018u;
  const u32 panel  = 0x1A2230u;
  const u32 border = 0x3A4A62u;

  /* Wait until any held buttons/touch are released so the dialog isn't
   * dismissed accidentally by the same press that triggered cam_start(). */
  while (app_loop()) {
    if (!hid_keys_held() && !hid_touch_held())
      break;
  }

  while (app_loop()) {
    gfx_clear(SCREEN_BOTTOM, bg_dim);
    gfx_round_rect(SCREEN_BOTTOM, bx - 1, by - 1, bw + 2, bh + 2, 13, border);
    gfx_round_rect(SCREEN_BOTTOM, bx, by, bw, bh, 12, panel);

    cam_popup_center(by + 16, FONT_BOLD, accent, "Camera Off");
    cam_popup_center(by + 48, FONT_REGULAR, 0xE6ECF4u,
                     "The camera is turned off,");
    cam_popup_center(by + 66, FONT_REGULAR, 0xE6ECF4u,
                     "please turn it on in settings");
    cam_popup_center(by + 84, FONT_REGULAR, 0xE6ECF4u,
                     "to use it in this app");

    gfx_round_rect(SCREEN_BOTTOM, ok_x, ok_y, ok_w, ok_h, 8, accent);
    cam_popup_center(ok_y + 8, FONT_BOLD, 0xFFFFFFu, "OK (A)");

    u32 down = hid_keys_down();
    if (down & (KEY_A | KEY_B | KEY_START))
      break;
    if (hid_touch_down() && hid_touch_in(ok_x, ok_y, ok_w, ok_h))
      break;
  }
}

bool cam_start(int which) {
  if (!cam_perm_enabled()) {
    cam_show_off_popup();
    return false;
  }
  if (!app_core())
    return false;

  u32 sel = (which == CAMERA_FRONT) ? (u32)CAM_FRONT : (u32)CAM_BACK;
  I2C_writeReg(I2C_DEV_MCU, 0x2Bu, 2u); /* Camera LED on */
  app_core_post(AUDIO_CMD_CAM, CAM_OP_START, sel, 0, 0);
  g_cam_active = true;
  return true;
}

void cam_stop(void) {
  if (!g_cam_active)
    return;
  g_cam_active = false;
  I2C_writeReg(I2C_DEV_MCU, 0x2Bu, 0u); /* Camera LED off */
  if (app_core())
    app_core_post(AUDIO_CMD_CAM, CAM_OP_STOP, 0, 0, 0);
}

bool cam_ready(void) {
  if (!g_cam_active || !app_core())
    return false;
  os_cache_sync();
  return cam_shared()->status == CAM_ST_LIVE;
}

uint32_t cam_frame_seq(void) {
  if (!g_cam_active || !app_core())
    return 0u;
  os_cache_sync();
  return cam_shared()->frame_seq;
}

const uint8_t *cam_frame_bgr(void) {
  if (!cam_ready())
    return NULL;
  volatile CamShared *cs = cam_shared();
  return (const uint8_t *)cam_bgr_addr(cs->ready_buf);
}

bool cam_draw(Screen screen, int x_off) {
  const uint8_t *src8 = cam_frame_bgr();
  if (!src8)
    return false;
  int max_w = (screen == SCREEN_TOP) ? SCREEN_TOP_WIDTH : SCREEN_BOTTOM_WIDTH;
  if (x_off < 0 || x_off + CAMERA_WIDTH > max_w)
    return false;

  volatile u8 *fb = app_fb(screen);
  const u32 *src = (const u32 *)src8;
  u32 *dst = (u32 *)(fb + (u32)x_off * SCREEN_HEIGHT * 3u);
  u32 words = CAM_BGR_BYTES / 4u;
  for (u32 i = 0; i < words; i++)
    dst[i] = src[i];
  app_dirty(screen);
  return true;
}

bool cam_save_bmp(const char *path) {
  const uint8_t *bgr = cam_frame_bgr();
  char fpath[FS_NAME_MAX + 8];
  static FIL f;
  static u8 row[CAMERA_WIDTH * 3];
  UINT bw = 0;

  if (!bgr || !path || !app_fs_mount() ||
      !app_fs_path(path, fpath, sizeof(fpath)))
    return false;
  if (f_open(&f, fpath, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK)
    return false;

  u32 img_size = CAMERA_WIDTH * CAMERA_HEIGHT * 3u;
  u32 file_size = 54u + img_size;
  u8 hdr[54];
  for (int i = 0; i < 54; i++) hdr[i] = 0;
  hdr[0] = 'B'; hdr[1] = 'M';
  hdr[2] = (u8)(file_size); hdr[3] = (u8)(file_size >> 8);
  hdr[4] = (u8)(file_size >> 16); hdr[5] = (u8)(file_size >> 24);
  hdr[10] = 54;
  hdr[14] = 40;
  hdr[18] = (u8)(CAMERA_WIDTH); hdr[19] = (u8)(CAMERA_WIDTH >> 8);
  hdr[22] = (u8)(CAMERA_HEIGHT); hdr[23] = (u8)(CAMERA_HEIGHT >> 8);
  hdr[26] = 1;
  hdr[28] = 24;
  hdr[34] = (u8)(img_size); hdr[35] = (u8)(img_size >> 8);
  hdr[36] = (u8)(img_size >> 16); hdr[37] = (u8)(img_size >> 24);

  bool ok = (f_write(&f, hdr, 54, &bw) == FR_OK && bw == 54);
  for (int y = CAMERA_HEIGHT - 1; ok && y >= 0; y--) {
    for (int x = 0; x < CAMERA_WIDTH; x++) {
      u32 off = ((u32)x * CAMERA_HEIGHT + (u32)(CAMERA_HEIGHT - 1 - y)) * 3u;
      row[x * 3 + 0] = bgr[off + 0];
      row[x * 3 + 1] = bgr[off + 1];
      row[x * 3 + 2] = bgr[off + 2];
    }
    ok = (f_write(&f, row, sizeof(row), &bw) == FR_OK && bw == sizeof(row));
  }
  f_close(&f);
  return ok;
}

void app_cam_leave(void) {
  cam_stop();
}
