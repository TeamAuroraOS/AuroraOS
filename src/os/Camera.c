/* Built-in AuroraOS Camera app — styled to match the Home Menu UI.
 * Streams live 320x240 BGR24 frames asynchronously from ARM11 (Cam11.c)
 * with zero input lag, smooth GPU transitions, shutter flash fade, and
 * a built-in Photo Gallery for SD:/Aurora/Photos/. */
#include "camera.h"
#include "anim.h"
#include "audio.h"
#include "ff.h"
#include "fileview.h"
#include "gpu.h"
#include "i2c.h"
#include "icons.h"
#include "image.h"
#include "power.h"
#include "statusbar.h"
#include "theme.h"
#include "touch.h"
#include "ui.h"
#include "uisound.h"

#define TW  TOP_SCREEN_WIDTH
#define TH  TOP_SCREEN_HEIGHT
#define BW  BOT_SCREEN_WIDTH
#define BH  BOT_SCREEN_HEIGHT

#define MAX_PHOTOS     64
#define PHOTO_NAME_MAX 32

extern void os_cache_sync(void);
extern void os_dcache_clean_range(const void *addr, u32 len);

static char g_photos[MAX_PHOTOS][PHOTO_NAME_MAX];
static int  g_photo_count;
static int  g_cam = CAM_BACK;
static int  g_mode = 0; /* 0 = Capture, 1 = Gallery */
static int  g_gal_idx = 0;
static int  g_flash_step = 0; /* 6..1 smooth white flash fade */
static u32  g_last_seq = 0;
static int  g_was_live = 0;
static int  g_gal_xfade = 0;

/* The camera LED (MCU register 0x2B: 2 on, 0 off) and the notification LED in
 * white, which is the one a user can see on every model. */
static void cam_set_led(int on) {
  u8 v = on ? 0xFFu : 0u;
  I2C_writeReg(I2C_DEV_MCU, 0x2Bu, on ? 2u : 0u);
  power_set_led(v, v, v);
}

/* Send a camera command to ARM11 (Cam11.c) via AudioCtrl mailbox.
 * Always drains any async GPU blit first so AudioCtrl is never contended. */
static void cam_cmd(u32 op, u32 arg1) {
  if (op == CAM_OP_START)
    cam_set_led(1);
  else if (op == CAM_OP_STOP)
    cam_set_led(0);
  AudioCtrl *ct = (AudioCtrl *)AUDIO_CTRL_ADDR;
  if (!audio_ready())
    return;
  gpu_wait_idle();
  ct->cmd = AUDIO_CMD_CAM;
  ct->arg0 = op;
  ct->arg1 = arg1;
  os_dcache_clean_range((const void *)&ct->cmd, 3u * sizeof(u32));
  ct->cmd_seq = ct->cmd_seq + 1u;
  os_dcache_clean_range((const void *)&ct->cmd_seq, sizeof(u32));
  for (u32 w = 0; w < 2000000u; w++) {
    os_cache_sync();
    if (ct->ack_seq == ct->cmd_seq)
      return;
  }
}

/* An app that crashed with the camera on left it streaming and the light on.
 * CamShared is only written by a core that ran the camera, so it holds
 * garbage until it is cleared here when a core starts fresh. */
void camera_boot_reset(int core_was_running) {
  volatile CamShared *cs = cam_shared();
  os_cache_sync();
  if (!core_was_running) {
    cs->status = CAM_ST_OFF;
    os_dcache_clean_range((const void *)&cs->status, sizeof(u32));
  } else if (cs->status != CAM_ST_OFF) {
    cam_cmd(CAM_OP_STOP, 0);
  }
  I2C_writeReg(I2C_DEV_MCU, 0x2Bu, 0u);
}

static Color darken(Color c, int num, int den) {
  Color o;
  o.r = (u8)((int)c.r * num / den);
  o.g = (u8)((int)c.g * num / den);
  o.b = (u8)((int)c.b * num / den);
  return o;
}

static void str_copy(char *d, const char *s, int max) {
  int i = 0;
  while (s[i] && i + 1 < max) { d[i] = s[i]; i++; }
  d[i] = '\0';
}

static char *put_str(char *p, const char *s) {
  while (*s) *p++ = *s++;
  *p = '\0';
  return p;
}

static char *put_int(char *p, int v) {
  char t[12];
  int n = 0;
  if (v < 0) { *p++ = '-'; v = -v; }
  do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
  while (n) *p++ = t[--n];
  *p = '\0';
  return p;
}

static int is_photo_ext(const char *s) {
  int n = 0;
  while (s[n]) n++;
  if (n < 5 || s[n - 4] != '.') return 0;
  char a = s[n - 3], b = s[n - 2], c = s[n - 1];
  if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
  if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
  if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
  return (a == 'b' && b == 'm' && c == 'p') ||
         (a == 'p' && b == 'n' && c == 'g') ||
         (a == 'j' && b == 'p' && c == 'g');
}

static void photo_path(char *out, const char *name) {
  char *p = put_str(out, CAM_PHOTOS_DIR "/");
  put_str(p, name);
}

static void scan_photos(void) {
  static FATFS fs;
  static DIR dir;
  static FILINFO info;
  g_photo_count = 0;
  if (f_mount(&fs, "", 1) != FR_OK)
    return;
  f_mkdir("Aurora");
  f_mkdir(CAM_PHOTOS_DIR);
  if (f_opendir(&dir, CAM_PHOTOS_DIR) == FR_OK) {
    while (g_photo_count < MAX_PHOTOS &&
           f_readdir(&dir, &info) == FR_OK && info.fname[0]) {
      if ((info.fattrib & AM_DIR) || !is_photo_ext(info.fname))
        continue;
      str_copy(g_photos[g_photo_count], info.fname, PHOTO_NAME_MAX);
      g_photo_count++;
    }
    f_closedir(&dir);
  }
  f_mount(NULL, "", 0);
  if (g_gal_idx >= g_photo_count)
    g_gal_idx = g_photo_count > 0 ? g_photo_count - 1 : 0;
}

/* Save the raw camera image directly from ARM11's 320x240 BGR24 buffer
 * (no status bar, pillars, or UI overlays) to SD:/Aurora/Photos/IMG_XXXX.bmp. */
static int save_current_photo(char *saved_name) {
  static FATFS fs;
  static FIL f;
  static u8 row[CAM_WIDTH * 3];
  char path[96];
  UINT bw = 0;

  os_cache_sync();
  volatile CamShared *cs = cam_shared();
  if (cs->status != CAM_ST_LIVE)
    return 0;
  const u8 *bgr = (const u8 *)cam_bgr_addr(cs->ready_buf);

  if (f_mount(&fs, "", 1) != FR_OK)
    return 0;
  f_mkdir("Aurora");
  f_mkdir(CAM_PHOTOS_DIR);

  int num = g_photo_count + 1;
  for (int tries = 0; tries < 9999; tries++, num++) {
    if (num > 9999) num = 1;
    saved_name[0] = 'I'; saved_name[1] = 'M'; saved_name[2] = 'G'; saved_name[3] = '_';
    saved_name[4] = (char)('0' + (num / 1000) % 10);
    saved_name[5] = (char)('0' + (num / 100) % 10);
    saved_name[6] = (char)('0' + (num / 10) % 10);
    saved_name[7] = (char)('0' + num % 10);
    saved_name[8] = '.'; saved_name[9] = 'b'; saved_name[10] = 'm';
    saved_name[11] = 'p'; saved_name[12] = '\0';
    photo_path(path, saved_name);
    FILINFO fno;
    if (f_stat(path, &fno) != FR_OK)
      break;
  }

  if (f_open(&f, path, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) {
    f_mount(NULL, "", 0);
    return 0;
  }

  u32 img_size = CAM_WIDTH * CAM_HEIGHT * 3u;
  u32 file_size = 54u + img_size;
  u8 hdr[54];
  for (int i = 0; i < 54; i++) hdr[i] = 0;
  hdr[0] = 'B'; hdr[1] = 'M';
  hdr[2] = (u8)(file_size); hdr[3] = (u8)(file_size >> 8);
  hdr[4] = (u8)(file_size >> 16); hdr[5] = (u8)(file_size >> 24);
  hdr[10] = 54;
  hdr[14] = 40;
  hdr[18] = (u8)(CAM_WIDTH); hdr[19] = (u8)(CAM_WIDTH >> 8);
  hdr[22] = (u8)(CAM_HEIGHT); hdr[23] = (u8)(CAM_HEIGHT >> 8);
  hdr[26] = 1;
  hdr[28] = 24;
  hdr[34] = (u8)(img_size); hdr[35] = (u8)(img_size >> 8);
  hdr[36] = (u8)(img_size >> 16); hdr[37] = (u8)(img_size >> 24);

  int ok = (f_write(&f, hdr, 54, &bw) == FR_OK && bw == 54);
  for (int y = (int)CAM_HEIGHT - 1; ok && y >= 0; y--) {
    for (int x = 0; x < (int)CAM_WIDTH; x++) {
      u32 off = ((u32)x * CAM_HEIGHT + (u32)(CAM_HEIGHT - 1u - (u32)y)) * 3u;
      row[x * 3 + 0] = bgr[off + 0];
      row[x * 3 + 1] = bgr[off + 1];
      row[x * 3 + 2] = bgr[off + 2];
    }
    ok = (f_write(&f, row, sizeof(row), &bw) == FR_OK && bw == sizeof(row));
  }
  f_close(&f);
  f_mount(NULL, "", 0);
  if (ok)
    scan_photos();
  return ok;
}

static int draw_bmp_top(const char *path) {
  static FATFS fs;
  static FIL f;
  static u8 row[TW * 3];
  u8 hdr[54];
  UINT br = 0;
  if (f_mount(&fs, "", 1) != FR_OK)
    return 0;
  if (f_open(&f, path, FA_READ) != FR_OK) {
    f_mount(NULL, "", 0);
    return 0;
  }
  if (f_read(&f, hdr, 54, &br) != FR_OK || br != 54 ||
      hdr[0] != 'B' || hdr[1] != 'M') {
    f_close(&f);
    Image img;
    int ok = (image_load(path, &img) == IMG_OK);
    f_mount(NULL, "", 0);
    if (ok) {
      ui_wallpaper(VRAM_TOP_LA, TW, TH, TH);
      image_draw_fit(VRAM_TOP_LA, 0, 0, TW, TH, TH, &img);
    }
    return ok;
  }
  u32 data_off = (u32)hdr[10] | ((u32)hdr[11] << 8) |
                 ((u32)hdr[12] << 16) | ((u32)hdr[13] << 24);
  int w = (int)((u32)hdr[18] | ((u32)hdr[19] << 8));
  int h = (int)((u32)hdr[22] | ((u32)hdr[23] << 8));
  int bpp = (int)((u32)hdr[28] | ((u32)hdr[29] << 8));
  if ((w != (int)CAM_WIDTH && w != TW) || h != TH || bpp != 24 ||
      f_lseek(&f, data_off) != FR_OK) {
    f_close(&f);
    f_mount(NULL, "", 0);
    return 0;
  }

  volatile u8 *fb = VRAM_TOP_LA;
  screen_touch(fb);
  int x_off = (TW - w) / 2;
  if (x_off > 0) {
    draw_filled_rect(fb, 0, 0, x_off, TH, TH, g_theme.bg_bot);
    draw_filled_rect(fb, x_off + w, 0, TW - (x_off + w), TH, TH, g_theme.bg_bot);
  }
  u32 row_bytes = (u32)w * 3u;
  for (int y = TH - 1; y >= 0; y--) {
    if (f_read(&f, row, row_bytes, &br) != FR_OK || br != row_bytes)
      break;
    for (int x = 0; x < w; x++) {
      u32 off = ((u32)(x_off + x) * TH + (u32)(TH - 1 - y)) * 3u;
      fb[off + 0] = row[x * 3 + 0];
      fb[off + 1] = row[x * 3 + 1];
      fb[off + 2] = row[x * 3 + 2];
    }
  }
  f_close(&f);
  f_mount(NULL, "", 0);
  return 1;
}

static void delete_current_photo(void) {
  static FATFS fs;
  char path[96];
  if (g_photo_count <= 0) return;
  photo_path(path, g_photos[g_gal_idx]);
  if (f_mount(&fs, "", 1) == FR_OK) {
    f_unlink(path);
    f_mount(NULL, "", 0);
  }
  scan_photos();
}

/* Fast 32-bit word copy of ARM11's pre-converted column-major 320x240 BGR24
 * frame directly into the centered 320x240 region of VRAM_TOP_LA. */
static void draw_top_capture(void) {
  volatile CamShared *cs = cam_shared();
  volatile u8 *fb = VRAM_TOP_LA;
  screen_touch(fb);
  os_cache_sync();

  if (cs->status == CAM_ST_LIVE) {
    const u32 x_off = (TW - CAM_WIDTH) / 2u; /* 40 px matte pillars */
    draw_filled_rect(fb, 0, 0, (int)x_off, TH, TH, g_theme.bg_bot);
    draw_filled_rect(fb, (int)(x_off + CAM_WIDTH), 0, (int)x_off, TH, TH,
                     g_theme.bg_bot);

    const u32 *src = (const u32 *)cam_bgr_addr(cs->ready_buf);
    u32 *dst = (u32 *)(fb + x_off * TH * 3u);
    u32 words = CAM_BGR_BYTES / 4u;
    for (u32 i = 0; i < words; i++)
      dst[i] = src[i];
  } else {
    ui_wallpaper(fb, TW, TH, TH);
    draw_gradient_round_rect(fb, 152, 56, 96, 96, 20, TH,
                             g_theme.panel_top, g_theme.panel_bot);
    ui_icon(fb, 168, 72, 64, TH, ASSET_APP_CAMERA_64, icon_camera_bits, g_accent);
    ui_text_mid(fb, TW / 2, 166, TH,
                g_cam == CAM_FRONT ? "Starting Front Camera..." : "Starting Back Camera...",
                g_theme.text, g_theme.bg_bot, &ui_bold);
    ui_text_mid(fb, TW / 2, 186, TH,
                "Press R or tap Shutter to capture",
                g_theme.text_dim, g_theme.bg_bot, &ui_small);
  }

  /* Smooth white shutter flash fade-out over the viewfinder */
  if (g_flash_step > 0) {
    static const int flash_alpha[7] = { 0, 28, 65, 110, 155, 200, 235 };
    int a = flash_alpha[g_flash_step > 6 ? 6 : g_flash_step];
    draw_filled_rect_alpha(fb, 0, 0, TW, TH, TH, COLOR_WHITE, a);
    g_flash_step--;
  }

  status_bar_draw();
  screen_present_top();
}

static void draw_top_gallery(void) {
  volatile u8 *fb = VRAM_TOP_LA;
  if (g_photo_count <= 0) {
    ui_wallpaper(fb, TW, TH, TH);
    status_bar_draw();
    ui_text_mid(fb, TW / 2, 108, TH, "No photos yet",
                g_theme.text, g_theme.bg_bot, &ui_title);
    ui_text_mid(fb, TW / 2, 134, TH, "Take a picture in Camera mode",
                g_theme.text_dim, g_theme.bg_bot, &ui_small);
    screen_present_top();
    return;
  }

  char path[96];
  photo_path(path, g_photos[g_gal_idx]);
  if (!draw_bmp_top(path)) {
    ui_wallpaper(fb, TW, TH, TH);
    ui_text_mid(fb, TW / 2, 110, TH, "Could not load photo",
                COLOR_RED, g_theme.bg_bot, &ui_font);
  }
  draw_filled_rect_alpha(fb, 0, TH - 24, TW, 24, TH, COLOR_BLACK, 175);
  char cap[64];
  char *p = put_str(cap, g_photos[g_gal_idx]);
  p = put_str(p, "   (");
  p = put_int(p, g_gal_idx + 1);
  p = put_str(p, " / ");
  p = put_int(p, g_photo_count);
  put_str(p, ")");
  ui_text_mid(fb, TW / 2, TH - 19, TH, cap, COLOR_WHITE, COLOR_BLACK, &ui_small);
  screen_present_top();
}

/* Bottom screen styled cleanly to match AuroraOS's Home Menu with a normal
 * rounded Shutter button carrying the camera icon. */
static void draw_bottom_capture(const char *toast) {
  volatile u8 *fb = VRAM_BOT_A;
  volatile CamShared *cs = cam_shared();
  os_cache_sync();

  ui_wallpaper(fb, BW, BH, BH);

  /* Header card: camera mode title only (or temporary toast when saving) */
  draw_gradient_round_rect(fb, 14, 16, BW - 28, 54, 12, BH,
                           g_theme.panel_top, g_theme.panel_bot);
  draw_round_ring(fb, 14, 16, BW - 28, 54, 12, 1, BH, g_theme.slot_top);
  ui_icon(fb, 26, 27, 32, BH, ASSET_APP_CAMERA_32, icon_camera_bits, g_accent);
  if (toast && toast[0]) {
    ui_text(fb, 68, 24, BH,
            g_cam == CAM_FRONT ? "Front Camera" : "Back Camera",
            g_theme.text, g_theme.panel_top, &ui_bold);
    ui_text(fb, 68, 45, BH, toast,
            g_theme.text_dim, g_theme.panel_bot, &ui_small);
  } else {
    ui_text(fb, 68, 35, BH,
            g_cam == CAM_FRONT ? "Front Camera" : "Back Camera",
            g_theme.text, g_theme.panel_top, &ui_bold);
  }
  Color dot_c = (cs->status == CAM_ST_LIVE) ? (Color){0x48, 0xE5, 0x8A}
                                            : (Color){0xF0, 0xB8, 0x40};
  draw_filled_round_rect(fb, BW - 34, 38, 10, 10, 5, BH, dot_c);

  /* Main Shutter button: normal rounded button with camera icon (R) */
  draw_gradient_round_rect(fb, 14, 88, BW - 28, 60, 14, BH,
                           g_accent, darken(g_accent, 7, 10));
  draw_round_ring(fb, 14, 88, BW - 28, 60, 14, 1, BH, COLOR_WHITE);
  ui_icon(fb, BW / 2 - 16, 102, 32, BH, ASSET_APP_CAMERA_32, icon_camera_bits,
          COLOR_WHITE);

  /* Secondary action buttons: Flip (X) & Gallery (Y) */
  draw_gradient_round_rect(fb, 14, 164, 140, 54, 12, BH,
                           g_theme.slot_top, g_theme.slot_bot);
  draw_round_ring(fb, 14, 164, 140, 54, 12, 1, BH, g_theme.panel_top);
  ui_text_mid(fb, 84, 178, BH, g_cam == CAM_FRONT ? "Back" : "Front",
              g_theme.text, g_theme.slot_top, &ui_bold);
  ui_text_mid(fb, 84, 198, BH, "Flip (X)",
              g_theme.text_dim, g_theme.slot_bot, &ui_small);

  draw_gradient_round_rect(fb, 166, 164, 140, 54, 12, BH,
                           g_theme.slot_top, g_theme.slot_bot);
  draw_round_ring(fb, 166, 164, 140, 54, 12, 1, BH, g_theme.panel_top);
  ui_text_mid(fb, 236, 178, BH, "Gallery",
              g_theme.text, g_theme.slot_top, &ui_bold);
  char gcnt[20];
  char *p = put_int(gcnt, g_photo_count);
  put_str(p, " (Y)");
  ui_text_mid(fb, 236, 198, BH, gcnt,
              g_theme.text_dim, g_theme.slot_bot, &ui_small);

  screen_present_bottom();
}

static void draw_bottom_gallery(void) {
  volatile u8 *fb = VRAM_BOT_A;
  char count_s[40];
  if (g_photo_count > 0) {
    char *p = put_int(count_s, g_gal_idx + 1);
    p = put_str(p, " of ");
    put_int(p, g_photo_count);
  } else {
    put_str(count_s, "0 photos");
  }

  ui_wallpaper(fb, BW, BH, BH);

  draw_gradient_round_rect(fb, 14, 14, BW - 28, 56, 11, BH,
                           g_theme.panel_top, g_theme.panel_bot);
  ui_text_mid(fb, BW / 2, 23, BH,
              g_photo_count > 0 ? g_photos[g_gal_idx] : "Photo Gallery",
              g_theme.text, g_theme.panel_top, &ui_bold);
  ui_text_mid(fb, BW / 2, 44, BH, count_s,
              g_theme.text_dim, g_theme.panel_bot, &ui_small);

  draw_gradient_round_rect(fb, 14, 86, 140, 48, 10, BH,
                           g_theme.slot_top, g_theme.slot_bot);
  ui_text_mid(fb, 84, 103, BH, "< Prev",
              g_theme.text, g_theme.slot_top, &ui_bold);

  draw_gradient_round_rect(fb, 166, 86, 140, 48, 10, BH,
                           g_theme.slot_top, g_theme.slot_bot);
  ui_text_mid(fb, 236, 103, BH, "Next >",
              g_theme.text, g_theme.slot_top, &ui_bold);

  draw_gradient_round_rect(fb, 14, 150, 140, 46, 10, BH,
                           COLOR_DARK_RED, darken(COLOR_DARK_RED, 6, 10));
  ui_text_mid(fb, 84, 166, BH, "Delete (X)",
              COLOR_WHITE, COLOR_DARK_RED, &ui_font);

  draw_gradient_round_rect(fb, 166, 150, 140, 46, 10, BH,
                           g_accent, darken(g_accent, 7, 10));
  ui_text_mid(fb, 236, 166, BH, "Camera (B)",
              COLOR_WHITE, g_accent, &ui_font);

  screen_present_bottom();
}

void camera_app_run(void) {
  char toast[40];
  int toast_ticks = 0;
  toast[0] = '\0';
  g_mode = 0;
  g_flash_step = 0;
  g_last_seq = 0;
  g_was_live = 0;
  g_gal_xfade = 0;

  anim_flush();
  gpu_wait_idle();
  scan_photos();
  cam_cmd(CAM_OP_START, (u32)g_cam);

  /* Smooth slide-in transition when opening Camera */
  anim_transition(ANIM_PUSH, ANIM_BOTH);
  draw_top_capture();
  draw_bottom_capture(toast);

  for (;;) {
    u32 k = get_keys_down();
    int tx, ty;
    int tapped = touch_tap(&tx, &ty);

    if (g_mode == 0) {
      int do_capture = 0, do_flip = 0, do_gallery = 0;

      if (k & BUTTON_B) {
        ui_sound_back();
        break;
      }
      if (k & BUTTON_R)
        do_capture = 1;
      if (k & BUTTON_X)
        do_flip = 1;
      if (k & BUTTON_Y)
        do_gallery = 1;

      if (tapped) {
        if (touch_in(tx, ty, 14, 88, BW - 28, 60))
          do_capture = 1;
        else if (touch_in(tx, ty, 14, 164, 140, 54))
          do_flip = 1;
        else if (touch_in(tx, ty, 166, 164, 140, 54))
          do_gallery = 1;
      }

      if (do_flip) {
        ui_sound_select();
        g_cam = (g_cam == CAM_FRONT) ? CAM_BACK : CAM_FRONT;
        g_was_live = 0;
        cam_cmd(CAM_OP_START, (u32)g_cam);
        toast[0] = '\0';
        anim_transition(ANIM_FADE, ANIM_BOTH);
        draw_top_capture();
        draw_bottom_capture(toast);
      } else if (do_capture) {
        char saved[PHOTO_NAME_MAX];
        ui_sound_select();
        /* Draw clean viewfinder without flash before saving BMP */
        g_flash_step = 0;
        draw_top_capture();
        if (save_current_photo(saved)) {
          char *p = put_str(toast, "Saved ");
          put_str(p, saved);
          toast_ticks = 90;
          g_flash_step = 6; /* smooth 6-frame white flash fade */
        } else {
          put_str(toast, "Save failed (check SD)");
          toast_ticks = 90;
        }
        draw_top_capture();
        draw_bottom_capture(toast);
      } else if (do_gallery) {
        ui_sound_select();
        scan_photos();
        g_gal_idx = g_photo_count > 0 ? g_photo_count - 1 : 0;
        g_mode = 1;
        g_gal_xfade = 0;
        anim_transition(ANIM_PUSH, ANIM_BOTH);
        draw_top_gallery();
        draw_bottom_gallery();
        continue;
      }

      /* Non-blocking async viewfinder refresh whenever ARM11 publishes a frame */
      os_cache_sync();
      volatile CamShared *cs = cam_shared();
      u32 seq = cs->frame_seq;
      if (g_flash_step > 0 || seq != g_last_seq) {
        g_last_seq = seq;
        draw_top_capture();
        if (!g_was_live && cs->status == CAM_ST_LIVE) {
          g_was_live = 1;
          draw_bottom_capture(toast);
        }
      }
      if (toast_ticks > 0) {
        toast_ticks--;
        if (toast_ticks == 0) {
          toast[0] = '\0';
          draw_bottom_capture(toast);
        }
      }
    } else {
      /* Gallery mode with smooth horizontal slide/cross-fade browsing */
      int move_dir = 0;
      if (k & BUTTON_B) {
        ui_sound_back();
        g_mode = 0;
        g_gal_xfade = 0;
        anim_transition(ANIM_POP, ANIM_BOTH);
        draw_top_capture();
        draw_bottom_capture(toast);
        continue;
      }
      if ((k & BUTTON_DLEFT) && g_photo_count > 1) {
        g_gal_idx = (g_gal_idx + g_photo_count - 1) % g_photo_count;
        move_dir = -1;
      }
      if ((k & BUTTON_DRIGHT) && g_photo_count > 1) {
        g_gal_idx = (g_gal_idx + 1) % g_photo_count;
        move_dir = 1;
      }
      if ((k & BUTTON_X) && g_photo_count > 0) {
        ui_sound_select();
        if (fv_confirm("Delete this photo?", g_photos[g_gal_idx],
                       "Delete", "Cancel")) {
          delete_current_photo();
        }
        g_gal_xfade = 0;
        anim_transition(ANIM_FADE, ANIM_BOTH);
        draw_top_gallery();
        draw_bottom_gallery();
        continue;
      }
      if (tapped) {
        if (touch_in(tx, ty, 14, 86, 140, 48) && g_photo_count > 1) {
          g_gal_idx = (g_gal_idx + g_photo_count - 1) % g_photo_count;
          move_dir = -1;
        } else if (touch_in(tx, ty, 166, 86, 140, 48) && g_photo_count > 1) {
          g_gal_idx = (g_gal_idx + 1) % g_photo_count;
          move_dir = 1;
        } else if (touch_in(tx, ty, 14, 150, 140, 46) && g_photo_count > 0) {
          ui_sound_select();
          if (fv_confirm("Delete this photo?", g_photos[g_gal_idx],
                         "Delete", "Cancel")) {
            delete_current_photo();
          }
          g_gal_xfade = 0;
          anim_transition(ANIM_FADE, ANIM_BOTH);
          draw_top_gallery();
          draw_bottom_gallery();
          continue;
        } else if (touch_in(tx, ty, 166, 150, 140, 46)) {
          ui_sound_back();
          g_mode = 0;
          g_gal_xfade = 0;
          anim_transition(ANIM_POP, ANIM_BOTH);
          draw_top_capture();
          draw_bottom_capture(toast);
          continue;
        }
      }
      if (move_dir != 0) {
        ui_sound_move();
        anim_xfade_begin(ANIM_TOP);
        draw_top_gallery();
        anim_xfade_area(ANIM_TOP, 0, 0, TW, TH);
        anim_xfade_start(ANIM_TOP, move_dir);
        screen_present_top();
        g_gal_xfade = 1;
        draw_bottom_gallery();
      } else if (g_gal_xfade) {
        if (anim_xfade_frame(ANIM_TOP))
          screen_present_top();
        else
          g_gal_xfade = 0;
      }
    }

    ui_idle();
  }

  cam_cmd(CAM_OP_STOP, 0);
  anim_transition(ANIM_POP, ANIM_BOTH);
}
