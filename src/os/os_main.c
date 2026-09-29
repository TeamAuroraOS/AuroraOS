#include "aurora.h"
#include "assets.h"
#include "ui.h"
#include "files.h"
#include "fileview.h"
#include "model.h"
#include "rendertest.h"
#include "screenshot.h"
#include "sdmmc.h"
#include "statusbar.h"
#include "touchcal.h"
#include "timer.h"
#include "audio.h"
#include "container.h"
#include "crash.h"
#include "font.h"
#include "gpu.h"
#include "i2c.h"
#include "icons.h"
#include "lang.h"
#include "power.h"
#include "touch.h"
#include "user.h"
#include "wifi.h"

void delay(volatile u32 cycles) {
  while (cycles--)
    __asm__ volatile("nop");
}

/* HID pad bits 0..9 are A/B/Select/Start/D-pad/R/L; 10 and 11 are X and Y. */
u32 get_keys(void) { return ~REG_HID_PAD & 0xFFF; }

/* Only directions repeat while held; everything else is edge-triggered, so
 * holding A cannot relaunch an app. */
#define KEY_REPEAT_MASK (BUTTON_DUP | BUTTON_DDOWN | BUTTON_DLEFT | BUTTON_DRIGHT)
#define KEY_REPEAT_DELAY_US 350000u
#define KEY_REPEAT_RATE_US   70000u

static u32 prev_keys = 0;
static u32 repeat_mark;
static u32 repeat_wait;

#define SCREENSHOT_KEYS (BUTTON_L | BUTTON_R)

u32 get_keys_down(void) {
  crash_poll_arm11();
  u32 cur = get_keys();
  u32 down = cur & ~prev_keys;
  u32 held = cur & prev_keys & KEY_REPEAT_MASK;

  /* Every screen polls here, so screenshots are handled once. The shoulder
   * buttons are then swallowed so the screen does not act on them. */
  if ((cur & SCREENSHOT_KEYS) == SCREENSHOT_KEYS && (down & SCREENSHOT_KEYS)) {
    screenshot_take();
    down &= ~(u32)SCREENSHOT_KEYS;
  }

  if (down & KEY_REPEAT_MASK) {
    repeat_mark = timer_ticks();
    repeat_wait = KEY_REPEAT_DELAY_US;
  } else if (held && timer_calibrated() &&
             timer_us_since(repeat_mark) >= repeat_wait) {
    down |= held;
    repeat_mark = timer_ticks();
    repeat_wait = KEY_REPEAT_RATE_US;
  }

  prev_keys = cur;
  return down;
}

static u32 str_len(const char *s) {
  u32 n = 0;
  while (*s++)
    n++;
  return n;
}

static void os_power_off(void) {
  I2C_init();
  I2C_writeReg(I2C_DEV_MCU, 0x22, 1 << 0);
  __asm__ volatile("mcr p15, 0, %0, c7, c10, 4" ::"r"(0) : "memory");
  I2C_writeReg(I2C_DEV_MCU, 0x20, 1 << 0);
  while (1)
    __asm__ volatile("mcr p15, 0, r0, c7, c0, 4");
}

static int g_accent_idx = 0;

/* USER.dat as loaded at boot. */
static UserConfig g_cfg;


typedef enum {
  ACT_NONE = 0,
  ACT_POWER,
  ACT_LAUNCH,
  ACT_MUSIC,
  ACT_FILES
} HomeAction;

typedef struct {
  const char *name; /* NULL => empty placeholder slot */
  const char *dev;
  const unsigned char *icon; /* built-in 32x32 bits, or NULL */
  u32 asset_lg, asset_sm;    /* pack icon at 64 / 32, or UI_NO_ASSET */
  Color tint;
  HomeAction action;
  const char *path;          /* ACT_LAUNCH: container path on the SD card */
} HomeApp;

#define HOME_COLS  5
#define HOME_ROWS  3
#define HOME_COUNT (HOME_COLS * HOME_ROWS)

static HomeApp home_apps[HOME_COUNT];

/* A long name is cut for display, but the path always holds all of it. */
#define APPS_DIR     "Aurora/Apps"
#define MAX_APPS     HOME_COUNT
#define APP_NAME_MAX 64
#define APP_PATH_MAX (sizeof(APPS_DIR) + FF_LFN_BUF + 1)
static char app_name[MAX_APPS][APP_NAME_MAX]; /* file name minus ".bin"  */
static char app_path[MAX_APPS][APP_PATH_MAX]; /* "Aurora/Apps/Name.bin"  */
static unsigned char app_icon[MAX_APPS][ICON_SIZE * ICON_ROW_BYTES];
static int  app_has_icon[MAX_APPS];
static int  app_count;

/* Relocatable app hand-off + return stubs (src/os/os_launch.s), and the end of
 * the OS's loadable image (src/os/os.ld) for the return snapshot. */
extern const unsigned char os_launch_stub[];
extern const unsigned char os_launch_stub_end[];
extern const unsigned char os_return_stub[];
extern const unsigned char os_return_stub_end[];
extern const unsigned char _os_image_end[];
extern void os_cache_sync(void);

/* Does not present. */
static void hm_status_bar(void) { status_bar_draw(); }

static void hm_top_static(void) {
  ui_wallpaper(VRAM_TOP_LA, TOP_SCREEN_WIDTH, TOP_SCREEN_HEIGHT,
               TOP_SCREEN_HEIGHT);
  hm_status_bar();
  screen_present_top();
}

static void hm_top_item(int selected) {
  const HomeApp *app = &home_apps[selected];

  int box = 96, bx = (TOP_SCREEN_WIDTH - box) / 2, by = 34;
  Color tint = app->name ? app->tint : COLOR_HM_EMPTY_TOP;
  Color tint_dark;
  tint_dark.r = (u8)((tint.r * 5) / 9);
  tint_dark.g = (u8)((tint.g * 5) / 9);
  tint_dark.b = (u8)((tint.b * 5) / 9);
  draw_gradient_round_rect(VRAM_TOP_LA, bx, by, box, box, 16,
                           TOP_SCREEN_HEIGHT, tint, tint_dark);
  if (app->icon || app->asset_lg != UI_NO_ASSET)
    ui_icon(VRAM_TOP_LA, bx, by, box, TOP_SCREEN_HEIGHT, app->asset_lg,
            app->icon, COLOR_WHITE);

  int cy = 148, cw = TOP_SCREEN_WIDTH - 80, ch = 62;
  draw_gradient_round_rect(VRAM_TOP_LA, 40, cy, cw, ch, 11, TOP_SCREEN_HEIGHT,
                           COLOR_PANEL_TOP, COLOR_PANEL_BOT);
  const char *name = app->name ? app->name : L(STR_EMPTY_SLOT);
  const char *dev = app->dev ? app->dev : "";
  ui_text_mid_fit(VRAM_TOP_LA, TOP_SCREEN_WIDTH / 2, cy + 7, TOP_SCREEN_HEIGHT,
                  name, cw - 24, COLOR_WHITE, COLOR_PANEL_TOP, &ui_title);
  ui_text_mid(VRAM_TOP_LA, TOP_SCREEN_WIDTH / 2, cy + 31, TOP_SCREEN_HEIGHT,
              dev, COLOR_HM_TEXT2, COLOR_PANEL_BOT, &ui_font);
  screen_present_top();
}

#define SLOT_SIZE 46
#define SLOT_GAP  12
#define SLOT_STEP (SLOT_SIZE + SLOT_GAP)
#define GRID_W    (HOME_COLS * SLOT_SIZE + (HOME_COLS - 1) * SLOT_GAP)
#define GRID_X    ((BOT_SCREEN_WIDTH - GRID_W) / 2)
#define GRID_Y    52

static void hm_bottom_static(void) {
  ui_wallpaper(VRAM_BOT_A, BOT_SCREEN_WIDTH, BOT_SCREEN_HEIGHT,
               BOT_SCREEN_HEIGHT);

  draw_filled_round_rect(VRAM_BOT_A, 8, 8, BOT_SCREEN_WIDTH - 16, 30, 10,
                         BOT_SCREEN_HEIGHT, COLOR_HM_BAR);
  int sx = BOT_SCREEN_WIDTH - 62, sy = 16;
  draw_filled_round_rect(VRAM_BOT_A, sx, sy, 11, 11, 5, BOT_SCREEN_HEIGHT,
                         COLOR_HM_TEXT2);
  draw_filled_round_rect(VRAM_BOT_A, sx + 2, sy + 2, 7, 7, 3, BOT_SCREEN_HEIGHT,
                         COLOR_HM_BAR);
  draw_filled_rect(VRAM_BOT_A, sx + 10, sy + 10, 4, 2, BOT_SCREEN_HEIGHT,
                   COLOR_HM_TEXT2);
  int gx = BOT_SCREEN_WIDTH - 34, gy = 18;
  draw_filled_round_rect(VRAM_BOT_A, gx, gy, 16, 4, 2, BOT_SCREEN_HEIGHT,
                         COLOR_HM_TEXT2);
  draw_filled_round_rect(VRAM_BOT_A, gx + 9, gy - 2, 5, 8, 2, BOT_SCREEN_HEIGHT,
                         COLOR_WHITE);
  draw_filled_round_rect(VRAM_BOT_A, gx, gy + 8, 16, 4, 2, BOT_SCREEN_HEIGHT,
                         COLOR_HM_TEXT2);
  draw_filled_round_rect(VRAM_BOT_A, gx + 2, gy + 6, 5, 8, 2, BOT_SCREEN_HEIGHT,
                         COLOR_WHITE);
}

static void hm_slot(int i, int selected) {
  int col = i % HOME_COLS, row = i / HOME_COLS;
  int x = GRID_X + col * SLOT_STEP, y = GRID_Y + row * SLOT_STEP;
  const HomeApp *app = &home_apps[i];

  if (selected) {
    draw_filled_round_rect(VRAM_BOT_A, x - 3, y - 3, SLOT_SIZE + 6,
                           SLOT_SIZE + 6, 12, BOT_SCREEN_HEIGHT, g_accent);
  } else {
    int t = (y * 255) / BOT_SCREEN_HEIGHT;
    Color bg;
    bg.r = (u8)((COLOR_HM_BG_TOP.r * (255 - t) + COLOR_HM_BG_BOT.r * t) / 255);
    bg.g = (u8)((COLOR_HM_BG_TOP.g * (255 - t) + COLOR_HM_BG_BOT.g * t) / 255);
    bg.b = (u8)((COLOR_HM_BG_TOP.b * (255 - t) + COLOR_HM_BG_BOT.b * t) / 255);
    draw_filled_rect(VRAM_BOT_A, x - 3, y - 3, SLOT_SIZE + 6, SLOT_SIZE + 6,
                     BOT_SCREEN_HEIGHT, bg);
  }
  draw_gradient_round_rect(VRAM_BOT_A, x, y, SLOT_SIZE, SLOT_SIZE, 10,
                           BOT_SCREEN_HEIGHT,
                           app->name ? COLOR_HM_SLOT_TOP : COLOR_HM_EMPTY_TOP,
                           app->name ? COLOR_HM_SLOT_BOT : COLOR_HM_EMPTY_BOT);
  if (app->icon || app->asset_sm != UI_NO_ASSET)
    ui_icon(VRAM_BOT_A, x, y, SLOT_SIZE, BOT_SCREEN_HEIGHT, app->asset_sm,
            app->icon, COLOR_WHITE);
}

static void hm_draw_full(int sel) {
  hm_top_static();
  hm_top_item(sel);
  hm_bottom_static();
  for (int i = 0; i < HOME_COUNT; i++)
    hm_slot(i, i == sel);
  screen_present_bottom();
}

static void hm_update(int old_sel, int new_sel) {
  hm_slot(old_sel, 0);
  hm_slot(new_sel, 1);
  screen_present_bottom();
  hm_top_item(new_sel);
}

/* Shared with os_setup.c, so a saved index means the same everywhere. */
#define accent_presets aurora_accent_presets
#define accent_names   aurora_accent_names
#define ACCENT_COUNT   AURORA_ACCENT_COUNT

static void settings_header(u32 asset, const unsigned char *icon,
                            const char *title, Color icon_color) {
  hm_top_static();
  ui_icon(VRAM_TOP_LA, (TOP_SCREEN_WIDTH - 96) / 2, 44, 96, TOP_SCREEN_HEIGHT,
          asset, icon, icon_color);
  ui_text_mid(VRAM_TOP_LA, TOP_SCREEN_WIDTH / 2, 148, TOP_SCREEN_HEIGHT, title,
              COLOR_WHITE, COLOR_HM_BG_BOT, &ui_title);
  screen_present_top();
}

typedef enum {
  SET_WIFI = 0,
  SET_ACCENT,
  SET_BRIGHTNESS,
  SET_TOUCHCAL,
  SET_WIFITEST,
  SET_GPUTEST,
  SET_ABOUT,
  SET_CRASH,
  SET_COUNT
} SettingId;

#define ROW_X    12
#define ROW_W    (BOT_SCREEN_WIDTH - 24)
#define ROW_H    34
#define ROW_STEP 42
#define ROW_Y0   8
#define SET_VISIBLE 5

/* icon == NULL draws the accent swatch instead. */
static void settings_content(int id, u32 *asset, const unsigned char **icon,
                             const char **name, const char **value) {
  *asset = UI_NO_ASSET;
  switch (id) {
    case SET_WIFI:
      *asset = ASSET_ICON_WIFI_32;
      *icon = icon_wifi_bits; *name = L(STR_WIFI); *value = L(STR_OFF); break;
    case SET_ACCENT:
      *icon = NULL; *name = L(STR_ACCENT_COLOR);
      *value = accent_names[g_accent_idx]; break;
    case SET_BRIGHTNESS:
      *asset = ASSET_ICON_DISPLAY_32;
      *icon = icon_brightness_bits; *name = L(STR_BRIGHTNESS);
      *value = "3 / 5"; break;
    case SET_TOUCHCAL:
      *asset = ASSET_ICON_CONSOLE_32;
      *icon = icon_boot_bits; *name = L(STR_TOUCH_CAL);
      *value = touch_cal_is_default() ? L(STR_DEFAULT) : L(STR_CUSTOM); break;
    case SET_WIFITEST:
      *asset = ASSET_ICON_GLOBE_32;
      *icon = icon_wifi_bits; *name = L(STR_WIFI_TEST); *value = ""; break;
    case SET_GPUTEST:
      *asset = ASSET_ICON_MONITOR_32;
      *icon = icon_boot_bits; *name = L(STR_GPU_TEST);
      *value = gpu_alive() ? "Ready" : "---"; break;
    case SET_ABOUT:
      *asset = ASSET_ICON_INFO_32;
      *icon = icon_settings_bits; *name = L(STR_ABOUT); *value = AURORA_VERSION; break;
    default: /* SET_CRASH */
      *asset = ASSET_ICON_REPORT_32;
      *icon = icon_power_bits; *name = L(STR_DEBUG_CRASH); *value = ""; break;
  }
}

static void settings_row(int drow, int id, int sel) {
  const unsigned char *icon;
  const char *name, *value;
  u32 asset;
  settings_content(id, &asset, &icon, &name, &value);

  int y = ROW_Y0 + drow * ROW_STEP;
  /* Unselected rows repaint the ring area in the background shade, so no halo
   * is left. */
  if (id == sel) {
    draw_filled_round_rect(VRAM_BOT_A, ROW_X - 3, y - 3, ROW_W + 6, ROW_H + 6,
                           10, BOT_SCREEN_HEIGHT, g_accent);
  } else {
    int t = (y * 255) / BOT_SCREEN_HEIGHT;
    Color bg;
    bg.r = (u8)((COLOR_HM_BG_TOP.r * (255 - t) + COLOR_HM_BG_BOT.r * t) / 255);
    bg.g = (u8)((COLOR_HM_BG_TOP.g * (255 - t) + COLOR_HM_BG_BOT.g * t) / 255);
    bg.b = (u8)((COLOR_HM_BG_TOP.b * (255 - t) + COLOR_HM_BG_BOT.b * t) / 255);
    draw_filled_rect(VRAM_BOT_A, ROW_X - 3, y - 3, ROW_W + 6, ROW_H + 6,
                     BOT_SCREEN_HEIGHT, bg);
  }
  draw_gradient_round_rect(VRAM_BOT_A, ROW_X, y, ROW_W, ROW_H, 8,
                           BOT_SCREEN_HEIGHT, COLOR_HM_SLOT_TOP,
                           COLOR_HM_SLOT_BOT);
  if (icon || asset != UI_NO_ASSET)
    ui_icon(VRAM_BOT_A, ROW_X + 6, y, ROW_H, BOT_SCREEN_HEIGHT, asset, icon,
            COLOR_WHITE);
  else
    draw_filled_round_rect(VRAM_BOT_A, ROW_X + 11, y + 9, 16, 16, 4,
                           BOT_SCREEN_HEIGHT, g_accent);
  ui_text(VRAM_BOT_A, ROW_X + 44, y + (ROW_H - ui_th(&ui_font)) / 2,
          BOT_SCREEN_HEIGHT, name, COLOR_WHITE, COLOR_HM_SLOT, &ui_font);
  if (value && value[0])
    ui_text(VRAM_BOT_A, ROW_X + ROW_W - 26 - ui_tw(&ui_font, value),
            y + (ROW_H - ui_th(&ui_font)) / 2, BOT_SCREEN_HEIGHT, value,
            COLOR_HM_TEXT2, COLOR_HM_SLOT, &ui_font);
  ui_text(VRAM_BOT_A, ROW_X + ROW_W - 12, y + (ROW_H - ui_th(&ui_font)) / 2,
          BOT_SCREEN_HEIGHT, ">", COLOR_HM_TEXT2, COLOR_HM_SLOT, &ui_font);
}

static int settings_top(int sel) {
  int top = sel - SET_VISIBLE / 2;
  if (top > SET_COUNT - SET_VISIBLE)
    top = SET_COUNT - SET_VISIBLE;
  if (top < 0)
    top = 0;
  return top;
}

static void settings_draw(int sel) {
  int top = settings_top(sel);

  draw_vgradient(VRAM_BOT_A, 0, 0, BOT_SCREEN_WIDTH, BOT_SCREEN_HEIGHT,
                 BOT_SCREEN_HEIGHT, COLOR_HM_BG_TOP, COLOR_HM_BG_BOT);
  for (int r = 0; r < SET_VISIBLE && top + r < SET_COUNT; r++)
    settings_row(r, top + r, sel);
  screen_present_bottom();
}

static void settings_update(int old_sel, int new_sel) {
  int top = settings_top(new_sel);

  if (settings_top(old_sel) != top) {
    settings_draw(new_sel);
    return;
  }
  settings_row(old_sel - top, old_sel, new_sel);
  settings_row(new_sel - top, new_sel, new_sel);
  screen_present_bottom();
}

#define WIFI_POINTS 5

static void wifi_draw(int sel) {
  clear_screen(VRAM_BOT_A, BOT_FB_SIZE, COLOR_HM_BG);
  draw_string(VRAM_BOT_A, 12, 10, BOT_SCREEN_HEIGHT, "wifi", COLOR_HM_TEXT2,
              COLOR_HM_BG);

  int bw = 170, bh = 34, bx = (BOT_SCREEN_WIDTH - bw) / 2, by = 28;
  if (sel == 0)
    draw_filled_round_rect(VRAM_BOT_A, bx - 3, by - 3, bw + 6, bh + 6, 12,
                           BOT_SCREEN_HEIGHT, COLOR_WHITE);
  draw_filled_round_rect(VRAM_BOT_A, bx, by, bw, bh, 10, BOT_SCREEN_HEIGHT,
                         g_accent);
  const char *bl = "Setup Wifi";
  draw_string(VRAM_BOT_A, bx + (bw - (int)str_len(bl) * FONT_WIDTH) / 2,
              by + (bh - FONT_HEIGHT) / 2, BOT_SCREEN_HEIGHT, bl, COLOR_WHITE,
              g_accent);

  for (int j = 0; j < WIFI_POINTS; j++) {
    int y = 80 + j * 30;
    int s = (sel == 1 + j);
    draw_filled_round_rect(VRAM_BOT_A, 9, y - 3, BOT_SCREEN_WIDTH - 18 + 6, 24 + 6,
                           8, BOT_SCREEN_HEIGHT, s ? g_accent : COLOR_HM_BG);
    draw_filled_round_rect(VRAM_BOT_A, 12, y, BOT_SCREEN_WIDTH - 24, 24, 6,
                           BOT_SCREEN_HEIGHT, COLOR_HM_SLOT);
    draw_string(VRAM_BOT_A, 24, y + (24 - FONT_HEIGHT) / 2, BOT_SCREEN_HEIGHT,
                "Wifi point", COLOR_WHITE, COLOR_HM_SLOT);
  }
  screen_present_bottom();
}

static void wifi_screen(void) {
  settings_header(ASSET_ICON_WIFI_64, icon_wifi_bits, "Wifi Configuration",
                  COLOR_WHITE);
  int sel = 0, n = 1 + WIFI_POINTS;
  wifi_draw(sel);
  while (1) {
    u32 k = get_keys_down();
    int prev = sel;
    if ((k & BUTTON_DUP) && sel > 0)
      sel--;
    if ((k & BUTTON_DDOWN) && sel < n - 1)
      sel++;
    if (sel != prev)
      wifi_draw(sel);
    if (k & BUTTON_B)
      return;
    ui_idle();
  }
}

#define SW_SIZE 40
#define SW_GAP  12
#define SW_COLS 5
#define SW_W    (SW_COLS * SW_SIZE + (SW_COLS - 1) * SW_GAP)
#define SW_X    ((BOT_SCREEN_WIDTH - SW_W) / 2)
#define SW_Y    48

static void accent_draw(int sel) {
  hm_top_static();
  draw_filled_round_rect(VRAM_TOP_LA, (TOP_SCREEN_WIDTH - 96) / 2, 40, 96, 96, 16,
                         TOP_SCREEN_HEIGHT, accent_presets[sel]);
  const char *t = L(STR_ACCENT_COLOR);
  draw_string(VRAM_TOP_LA, (TOP_SCREEN_WIDTH - (int)str_len(t) * FONT_WIDTH) / 2,
              150, TOP_SCREEN_HEIGHT, t, COLOR_WHITE, COLOR_HM_BG);
  draw_string(VRAM_TOP_LA,
              (TOP_SCREEN_WIDTH - (int)str_len(accent_names[sel]) * FONT_WIDTH) / 2,
              170, TOP_SCREEN_HEIGHT, accent_names[sel], COLOR_HM_TEXT2,
              COLOR_HM_BG);
  screen_present_top();

  clear_screen(VRAM_BOT_A, BOT_FB_SIZE, COLOR_HM_BG);
  draw_string(VRAM_BOT_A, 12, 14, BOT_SCREEN_HEIGHT, L(STR_PICK_ACCENT),
              COLOR_HM_TEXT2, COLOR_HM_BG);
  for (int i = 0; i < ACCENT_COUNT; i++) {
    int col = i % SW_COLS, row = i / SW_COLS;
    int x = SW_X + col * (SW_SIZE + SW_GAP), y = SW_Y + row * (SW_SIZE + SW_GAP);
    if (i == sel)
      draw_filled_round_rect(VRAM_BOT_A, x - 3, y - 3, SW_SIZE + 6, SW_SIZE + 6,
                             12, BOT_SCREEN_HEIGHT, COLOR_WHITE);
    draw_filled_round_rect(VRAM_BOT_A, x, y, SW_SIZE, SW_SIZE, 10,
                           BOT_SCREEN_HEIGHT, accent_presets[i]);
    if (i == g_accent_idx)
      draw_filled_round_rect(VRAM_BOT_A, x + SW_SIZE / 2 - 5, y + SW_SIZE / 2 - 5,
                             10, 10, 3, BOT_SCREEN_HEIGHT, COLOR_WHITE);
  }
  draw_string(VRAM_BOT_A, 12, BOT_SCREEN_HEIGHT - 18, BOT_SCREEN_HEIGHT,
              L(STR_A_APPLY_B_BACK), COLOR_HM_TEXT2, COLOR_HM_BG);
  screen_present_bottom();
}

static void accent_screen(void) {
  int sel = g_accent_idx;
  int rows = (ACCENT_COUNT + SW_COLS - 1) / SW_COLS;
  accent_draw(sel);
  while (1) {
    u32 k = get_keys_down();
    int prev = sel;
    int col = sel % SW_COLS, row = sel / SW_COLS;
    if ((k & BUTTON_DLEFT) && col > 0)
      sel--;
    if ((k & BUTTON_DRIGHT) && col < SW_COLS - 1 && sel + 1 < ACCENT_COUNT)
      sel++;
    if ((k & BUTTON_DUP) && row > 0)
      sel -= SW_COLS;
    if ((k & BUTTON_DDOWN) && row < rows - 1 && sel + SW_COLS < ACCENT_COUNT)
      sel += SW_COLS;
    int apply = 0;
    int tx, ty;
    if (touch_tap(&tx, &ty)) {
      for (int i = 0; i < ACCENT_COUNT; i++) {
        int c = i % SW_COLS, r = i / SW_COLS;
        int x = SW_X + c * (SW_SIZE + SW_GAP), y = SW_Y + r * (SW_SIZE + SW_GAP);
        if (touch_in(tx, ty, x, y, SW_SIZE, SW_SIZE)) {
          sel = i;
          apply = 1;
          break;
        }
      }
    }

    if (sel != prev)
      accent_draw(sel);
    if ((k & BUTTON_A) || apply) {
      g_accent = accent_presets[sel];
      g_accent_idx = sel;
      ui_bg_invalidate(); /* the accent tints the wallpaper: recompose it */
      ui_bg_build();
      accent_draw(sel);
    }
    if (k & BUTTON_B)
      return;
    ui_idle();
  }
}

static char *snd_cpy(char *dst, const char *src) {
  while ((*dst = *src)) {
    dst++;
    src++;
  }
  return dst;
}

static void snd_u32(char *out, u32 v) {
  char tmp[12];
  int i = 0;
  if (v == 0)
    tmp[i++] = '0';
  while (v) {
    tmp[i++] = (char)('0' + v % 10);
    v /= 10;
  }
  int p = 0;
  while (i)
    out[p++] = tmp[--i];
  out[p] = '\0';
}

static void snd_hex(char *out, u32 v) {
  static const char d[] = "0123456789ABCDEF";
  out[0] = '0';
  out[1] = 'x';
  for (int i = 0; i < 8; i++)
    out[2 + i] = d[(v >> ((7 - i) * 4)) & 0xF];
  out[10] = '\0';
}


/* Wi-Fi Test. GPL-2.0: part of the Wi-Fi driver (ath6kl-derived; credit
 * Octoblimp). See docs/wifi.md "License and credits". */
static void wifi_hex4(char *out, u32 v) {
  static const char d[] = "0123456789ABCDEF";
  for (int i = 0; i < 4; i++)
    out[i] = d[(v >> ((3 - i) * 4)) & 0xF];
  out[4] = '\0';
}

static void wifitest_draw(const WifiShared *w);

static void wifi_run_probe(WifiShared *w) {
  wifi_get(w);
  u32 last = w->seq;
  wifi_probe();
  for (int t = 0; t < 150; t++) { /* up to ~3 s */
    delay(200000);
    wifi_get(w);
    if (w->seq != last)
      break;
  }
}

static int wifi_load_blob(const char *path, u32 dst, u32 maxlen,
                          volatile u32 *len_out) {
  static FIL f;
  UINT br = 0;
  if (f_open(&f, path, FA_READ) != FR_OK)
    return 0;
  u32 sz = (u32)f_size(&f);
  if (sz == 0 || sz > maxlen) {
    f_close(&f);
    return 0;
  }
  FRESULT fr = f_read(&f, (void *)dst, sz, &br);
  f_close(&f);
  *len_out = br;
  return (fr == FR_OK && br == sz) ? 1 : 0;
}

static int wifi_load_firmware(u32 main_type) {
  static FATFS fwfs;
  if (f_mount(&fwfs, "", 1) != FR_OK)
    return 0;
  WifiFw *fw = (WifiFw *)WIFI_FW_ADDR;
  fw->magic = 0;
  fw->main_len = 0;
  fw->main_type = 0;
  int ok = 1;
  ok &= wifi_load_blob("Aurora/wifi/STUBDATA.BIN", WIFI_FW_STUBDATA, 0x1000,
                       &fw->stubdata_len);
  ok &= wifi_load_blob("Aurora/wifi/STUBCODE.BIN", WIFI_FW_STUBCODE, 0x1000,
                       &fw->stubcode_len);
  ok &= wifi_load_blob("Aurora/wifi/DATABASE.BIN", WIFI_FW_DATABASE, 0x1000,
                       &fw->database_len);
  const char *main_path = main_type == WIFI_FW_TYPE4
                            ? "Aurora/wifi/MAINTYP4.BIN"
                            : "Aurora/wifi/MAINTYP1.BIN";
  ok &= wifi_load_blob(main_path, WIFI_FW_MAIN, 0x60000, &fw->main_len);
  f_mount(NULL, "", 0);
  if (ok) {
    fw->main_type = main_type;
    fw->magic = WIFI_FW_MAGIC;
  }
  return ok;
}

static int wifi_run_boot(WifiShared *w, u32 main_type, u32 opts) {
  if (!wifi_load_firmware(main_type))
    return 0;
  wifi_get(w);
  u32 last = w->seq;
  wifi_boot(opts);
  for (int t = 0; t < 8000; t++) { /* the upload is slow, and may be redone */
    delay(200000);
    wifi_get(w);
    if (w->seq != last)
      break;
    if ((t % 6) == 0)
      wifitest_draw(w);
    if (get_keys_down() & BUTTON_B)
      break;
  }
  return 1;
}

/* The recipe L and R boot with; X cycles it. */
static u32 wifi_opts = WIFI_OPT_RESTORE_SOC | WIFI_OPT_NO_POST_LZ;
static int wifi_preset = 0;

static void wifitest_draw(const WifiShared *w) {
  clear_screen(VRAM_BOT_A, BOT_FB_SIZE, COLOR_HM_BG);
  draw_string(VRAM_BOT_A, 8, 6, BOT_SCREEN_HEIGHT, L(STR_WIFI_TEST),
              COLOR_HM_TEXT2, COLOR_HM_BG);

  char line[48], num[16], *p;
  int alive = audio_alive();

  p = snd_cpy(line, alive ? "core v" : "core? v");
  snd_u32(num, audio_version());
  p = snd_cpy(p, num);
  p = snd_cpy(p, " ph ");
  snd_u32(num, w->phase);
  p = snd_cpy(p, num);
  p = snd_cpy(p, " clk ");
  wifi_hex4(num, w->clk);
  p = snd_cpy(p, num);
  p = snd_cpy(p, " t/o ");
  snd_u32(num, w->timeouts);
  snd_cpy(p, num);
  Color pc = (w->phase == WIFI_PH_DONE) ? COLOR_AURORA : COLOR_ORANGE;
  draw_string(VRAM_BOT_A, 8, 22, BOT_SCREEN_HEIGHT, line, pc, COLOR_HM_BG);

  draw_string(VRAM_BOT_A, 8, 38, BOT_SCREEN_HEIGHT,
              "idx  arg      resp      s0/s1", COLOR_HM_TEXT2, COLOR_HM_BG);

  u32 n = w->nlog;
  u32 nmax = (w->boot_step != WIFI_BOOT_NONE) ? 4u : 8u; /* room for the boot lines */
  if (n > nmax)
    n = nmax;
  for (u32 i = 0; i < n; i++) {
    const WifiCmd *e = &w->log[i];
    p = snd_cpy(line, "C");
    snd_u32(num, (u32)(e->cmd & 0x3F));
    if ((e->cmd & 0x3F) < 10) {
      *p++ = '0';
      *p = '\0';
    }
    p = snd_cpy(p, num);
    *p++ = ' ';
    wifi_hex4(num, (u16)(e->arg >> 16));
    p = snd_cpy(p, num);
    wifi_hex4(num, (u16)e->arg);
    p = snd_cpy(p, num);
    *p++ = ' ';
    snd_hex(num, e->resp);
    p = snd_cpy(p, num);
    *p++ = ' ';
    wifi_hex4(num, e->stat0);
    p = snd_cpy(p, num);
    *p++ = '/';
    wifi_hex4(num, e->stat1);
    snd_cpy(p, num);
    int good = (e->ok == 1) && (e->resp != 0) && (e->resp != 0xFFFFFFFF);
    draw_string(VRAM_BOT_A, 8, 52 + (int)i * 13, BOT_SCREEN_HEIGHT, line,
                good ? COLOR_AURORA : (e->ok == 2 ? COLOR_ORANGE : COLOR_HM_TEXT2),
                COLOR_HM_BG);
  }

  if (w->boot_step != WIFI_BOOT_NONE) {
    p = snd_cpy(line, "a ");
    snd_hex(num, w->htc_regs2);
    p = snd_cpy(p, num);
    p = snd_cpy(p, " fw ");
    snd_hex(num, w->fw_sum);
    p = snd_cpy(p, num);
    p = snd_cpy(p, " ck ");
    snd_hex(num, w->clkcnt);
    snd_cpy(p, num);
    draw_string(VRAM_BOT_A, 8, 122, BOT_SCREEN_HEIGHT, line, COLOR_HM_TEXT2,
               COLOR_HM_BG);

    p = snd_cpy(line, "drop ");
    snd_u32(num, w->htc_drained & 0xFFFFu);
    p = snd_cpy(p, num);
    *p++ = '/';
    wifi_hex4(num, w->htc_drained >> 16);
    p = snd_cpy(p, num);
    p = snd_cpy(p, " got ");
    snd_hex(num, w->htc_msg0);
    p = snd_cpy(p, num);
    p = snd_cpy(p, " t ");
    snd_u32(num, w->htc_try);
    p = snd_cpy(p, num);
    *p++ = '/';
    snd_u32(num, w->htc_err);
    snd_cpy(p, num);
    draw_string(VRAM_BOT_A, 8, 136, BOT_SCREEN_HEIGHT, line, COLOR_HM_TEXT2,
               COLOR_HM_BG);

    p = snd_cpy(line, "tr ");
    snd_hex(num, w->trace);
    p = snd_cpy(p, num);
    p = snd_cpy(p, " f ");
    wifi_hex4(num, w->fail_cmd);
    p = snd_cpy(p, num);
    *p++ = ' ';
    snd_hex(num, w->fail_stat);
    p = snd_cpy(p, num);
    p = snd_cpy(p, " @");
    snd_hex(num, w->fail_at);
    snd_cpy(p, num + 4); /* the low 24 bits: the send count */
    draw_string(VRAM_BOT_A, 8, 108, BOT_SCREEN_HEIGHT, line, COLOR_HM_TEXT2,
               COLOR_HM_BG);

    p = snd_cpy(line, "s ");
    for (int i = 0; i < 3; i++) {
      snd_hex(num, w->htc_snap[i]);
      p = snd_cpy(p, num + 2); /* without the "0x", to fit three */
      *p++ = ' ';
    }
    *p = 0;
    draw_string(VRAM_BOT_A, 8, 150, BOT_SCREEN_HEIGHT, line, COLOR_HM_TEXT2,
               COLOR_HM_BG);
  }

  /* Parse the CIS for the CISTPL_MANFID (0x20) tuple: manufacturer + card ID.
   * Atheros vendor = 0x0271. */
  {
    u32 manf = 0, card = 0;
    int found = 0, pos = 0;
    for (int g = 0; g < 24 && pos + 1 < 48; g++) {
      u8 code = w->cis[pos];
      if (code == 0xFF)
        break; /* end of CIS */
      if (code == 0x00) {
        pos++;
        continue;
      } /* null tuple */
      u8 link = w->cis[pos + 1];
      if (code == 0x20 && pos + 5 < 48) {
        manf = w->cis[pos + 2] | ((u32)w->cis[pos + 3] << 8);
        card = w->cis[pos + 4] | ((u32)w->cis[pos + 5] << 8);
        found = 1;
        break;
      }
      pos += 2 + link;
    }
    if (found && manf == 0x0271)
      p = snd_cpy(line, "ATHEROS ");
    else
      p = snd_cpy(line, "id ");
    wifi_hex4(num, manf);
    p = snd_cpy(p, num);
    *p++ = ':';
    wifi_hex4(num, card);
    p = snd_cpy(p, num);
    p = snd_cpy(p, "  fn1 ");
    p = snd_cpy(p, (w->ior & 0x02) ? "RDY" : "no");
    p = snd_cpy(p, "  wc ");
    wifi_hex4(num, w->wificnt & 0xFFu);
    p = snd_cpy(p, num);
    p = snd_cpy(p, " o ");
    snd_u32(num, wifi_opts);
    p = snd_cpy(p, num);
    Color mc = (found && manf == 0x0271) ? COLOR_AURORA : COLOR_WHITE;
    draw_string(VRAM_BOT_A, 8, BOT_SCREEN_HEIGHT - 72, BOT_SCREEN_HEIGHT, line,
                mc, COLOR_HM_BG);
  }

  /* Boot progress, or the probe's diag-window reads. */
  if (w->boot_step != WIFI_BOOT_NONE) {
    static const char *const bl[] = {"NONE", "NOFW", "BADVER", "HI",
                                     "STUB", "MAIN", "DONE"};
    u32 bs = w->boot_step;
    p = snd_cpy(line, "FW T");
    snd_u32(num, w->fw_type);
    p = snd_cpy(p, num);
    *p++ = ' ';
    p = snd_cpy(p, bs <= WIFI_BOOT_DONE ? bl[bs] : "?");
    p = snd_cpy(p, " rdy ");
    snd_u32(num, w->boot_ready);
    p = snd_cpy(p, num);
    p = snd_cpy(p, " s ");
    snd_u32(num, w->bmi_sends);
    p = snd_cpy(p, num);
    p = snd_cpy(p, " nc ");
    snd_u32(num, w->bmi_nocred);
    p = snd_cpy(p, num);
    if (w->boot_tries > 1) {
      p = snd_cpy(p, " b ");
      snd_u32(num, w->boot_tries);
      p = snd_cpy(p, num);
    }
    if (w->fw_chk) {
      p = snd_cpy(p, " p ");
      snd_hex(num, w->fw_chk);
      p = snd_cpy(p, num);
    }
    *p = 0;
    Color bc = w->boot_ready == 1  ? COLOR_AURORA
               : (bs == WIFI_BOOT_NOFW || bs == WIFI_BOOT_BADVER) ? COLOR_ORANGE
                                                                  : COLOR_HM_TEXT2;
    draw_string(VRAM_BOT_A, 8, BOT_SCREEN_HEIGHT - 58, BOT_SCREEN_HEIGHT, line,
                bc, COLOR_HM_BG);

    if (w->htc_ready) {
      p = snd_cpy(line, "HTC id ");
      wifi_hex4(num, w->htc_msgid);
      p = snd_cpy(p, num);
      p = snd_cpy(p, " cr ");
      snd_u32(num, w->htc_credits);
      p = snd_cpy(p, num);
      p = snd_cpy(p, " sz ");
      snd_u32(num, w->htc_credsz);
      p = snd_cpy(p, num);
      if (w->htc_stage >= WIFI_HTC_CONNECTED) {
        p = snd_cpy(p, " ep ");
        snd_u32(num, w->htc_ep);
        p = snd_cpy(p, num);
      }
      *p = 0;
    } else {
      /* mb is fw_dbrd: the stub readback, or a mailbox 0 peek if nothing was
       * flagged while waiting for HTC_READY. */
      p = snd_cpy(line, "r ");
      snd_hex(num, w->htc_regs);
      p = snd_cpy(p, num);
      p = snd_cpy(p, " mb ");
      snd_hex(num, w->fw_dbrd);
      snd_cpy(p, num);
    }
    draw_string(VRAM_BOT_A, 8, BOT_SCREEN_HEIGHT - 44, BOT_SCREEN_HEIGHT, line,
                (w->htc_msgid == 1) ? COLOR_AURORA : COLOR_HM_TEXT2, COLOR_HM_BG);
  } else {
    p = snd_cpy(line, "diag ");
    snd_hex(num, w->diag_a);
    p = snd_cpy(p, num);
    *p++ = ' ';
    snd_hex(num, w->diag_b);
    snd_cpy(p, num);
    int ok = (w->diag_a != 0 && w->diag_a != 0xFFFFFFFF) ||
             (w->diag_b != 0 && w->diag_b != 0xFFFFFFFF);
    draw_string(VRAM_BOT_A, 8, BOT_SCREEN_HEIGHT - 44, BOT_SCREEN_HEIGHT, line,
                ok ? COLOR_AURORA : COLOR_ORANGE, COLOR_HM_BG);
  }
  if (w->boot_step != WIFI_BOOT_NONE) {
    if (w->wmi_event) {
      p = snd_cpy(line, "WMI ");
      wifi_hex4(num, w->wmi_event);
      p = snd_cpy(p, num);
      p = snd_cpy(p, " MAC ");
      for (int i = 0; i < 6; i++) {
        u32 b = (i < 4) ? ((w->wmi_mac0 >> (i * 8)) & 0xFFu)
                        : ((w->wmi_mac1 >> ((i - 4) * 8)) & 0xFFu);
        wifi_hex4(num, b);
        p = snd_cpy(p, num + 2); /* the low two digits */
      }
      *p = 0;
    } else {
      p = snd_cpy(line, "HTC ep ");
      snd_u32(num, w->htc_ep);
      p = snd_cpy(p, num);
      p = snd_cpy(p, " st ");
      snd_u32(num, w->htc_status);
      p = snd_cpy(p, num);
      p = snd_cpy(p, " sg ");
      snd_u32(num, w->htc_stage);
      p = snd_cpy(p, num);
      p = snd_cpy(p, " r ");
      snd_hex(num, w->htc_regs);
      snd_cpy(p, num);
    }
    draw_string(VRAM_BOT_A, 8, BOT_SCREEN_HEIGHT - 30, BOT_SCREEN_HEIGHT, line,
                (w->wmi_event == 0x1001) ? COLOR_AURORA : COLOR_ORANGE,
                COLOR_HM_BG);
  } else {
    p = snd_cpy(line, "BMI ver ");
    snd_hex(num, w->bmi_ver);
    p = snd_cpy(p, num);
    p = snd_cpy(p, " ty ");
    snd_hex(num, w->bmi_type);
    snd_cpy(p, num);
    int bmi_ok = w->bmi_ver != 0 && w->bmi_ver != 0xFFFFFFFF;
    draw_string(VRAM_BOT_A, 8, BOT_SCREEN_HEIGHT - 30, BOT_SCREEN_HEIGHT, line,
                bmi_ok ? COLOR_AURORA : COLOR_HM_TEXT2, COLOR_HM_BG);
  }

  draw_string(VRAM_BOT_A, 8, BOT_SCREEN_HEIGHT - 15, BOT_SCREEN_HEIGHT,
              "A:Probe R:T4 L:T1 X:opt B:Back", COLOR_HM_TEXT2, COLOR_HM_BG);
  screen_present_bottom();
}

static void wifi_test_screen(void) {
  settings_header(ASSET_ICON_GLOBE_64, icon_wifi_bits, L(STR_WIFI_TEST),
                  COLOR_WHITE);
  static WifiShared w;
  wifi_run_probe(&w);
  wifitest_draw(&w);
  while (1) {
    u32 k = get_keys_down();
    int tx, ty;
    if ((k & BUTTON_A) || touch_tap(&tx, &ty)) {
      wifi_run_probe(&w);
      wifitest_draw(&w);
    }
    if (k & BUTTON_R) {
      wifi_run_boot(&w, WIFI_FW_TYPE4, wifi_opts);
      wifitest_draw(&w);
    }
    if (k & BUTTON_L) {
      wifi_run_boot(&w, WIFI_FW_TYPE1, wifi_opts);
      wifitest_draw(&w);
    }
    if (k & BUTTON_X) {
      /* 3 (default): the Linux sequence plus ath6kl's interrupt disable. */
      static const u32 presets[] = {WIFI_OPT_RESTORE_SOC | WIFI_OPT_NO_POST_LZ,
                                    WIFI_OPT_LINUX,
                                    WIFI_OPT_RESTORE_SOC,
                                    0};
      wifi_preset = (wifi_preset + 1) % 4;
      wifi_opts = presets[wifi_preset];
      wifitest_draw(&w);
    }
    if (k & BUTTON_B)
      return;
    ui_idle();
  }
}

/* GPU Test. GPL-2.0: part of the PICA200 driver (docs/gpu.md "License and
 * credits").
 *
 * The framebuffers start at 0x18300000, so the first VRAM bank is free for
 * the blit test. */

#define GPU_SCRATCH 0x18000000u

static const char *const gpu_step_names[] = {
    "NONE", "CLOCK", "QUIESCE", "IDLE", "ID", "ARMED", "STARTED", "DONE"};
static const char *const gpu_err_names[] = {"ok", "NOINIT", "BADARG", "TIMEOUT",
                                            "BUSY"};

/* A recognisable gradient to blit. The framebuffer is column-major. */
static void gpu_paint_scratch(void) {
  volatile u8 *s = (volatile u8 *)GPU_SCRATCH;
  for (int col = 0; col < TOP_SCREEN_WIDTH; col++) {
    u8 r = (u8)(col * 255 / (TOP_SCREEN_WIDTH - 1));
    for (int row = 0; row < TOP_SCREEN_HEIGHT; row++) {
      u32 o = (u32)(col * TOP_SCREEN_HEIGHT + row) * BYTES_PER_PIXEL;
      s[o + 0] = (u8)(row * 255 / (TOP_SCREEN_HEIGHT - 1)); /* b */
      s[o + 1] = 0x30;                                      /* g */
      s[o + 2] = r;                                         /* r */
    }
  }
}


static u32 bench_op_us = 0;   /* one GPU round trip, however small the work */
static u32 bench_cpu_us = 0;  /* CPU filling the bottom screen with a shade */
static u32 bench_psc_us = 0;  /* GPU doing a whole-screen solid fill        */
static u32 bench_blit_us = 0; /* GPU moving a finished screen to the panel  */
static u32 bench_bg_us = 0;   /* painting the background from its cache     */
static u32 bench_patch_us = 0;/* one list-row background patch              */
static u32 bench_card_us = 0; /* one list-row card, the gradient round rect  */

extern void os_mpu_enable(void);

static int mpu_on;

/* CP15 c1: bit 0 MPU, bit 2 D-cache, bit 12 I-cache. c2 (cacheable) and c3
 * (bufferable) hold a bit per region; c6,n is region n's base, size and
 * enable. */
static u32 cp15_c1(void) {
  u32 v;
  __asm__ volatile("mrc p15, 0, %0, c1, c0, 0" : "=r"(v));
  return v;
}
static u32 cp15_c2(void) {
  u32 v;
  __asm__ volatile("mrc p15, 0, %0, c2, c0, 0" : "=r"(v));
  return v;
}
static u32 cp15_c3(void) {
  u32 v;
  __asm__ volatile("mrc p15, 0, %0, c3, c0, 0" : "=r"(v));
  return v;
}
static u32 cp15_region(int n) {
  u32 v = 0;
  switch (n) {
    case 0: __asm__ volatile("mrc p15, 0, %0, c6, c0, 0" : "=r"(v)); break;
    case 1: __asm__ volatile("mrc p15, 0, %0, c6, c1, 0" : "=r"(v)); break;
    case 2: __asm__ volatile("mrc p15, 0, %0, c6, c2, 0" : "=r"(v)); break;
    case 3: __asm__ volatile("mrc p15, 0, %0, c6, c3, 0" : "=r"(v)); break;
    case 4: __asm__ volatile("mrc p15, 0, %0, c6, c4, 0" : "=r"(v)); break;
    case 5: __asm__ volatile("mrc p15, 0, %0, c6, c5, 0" : "=r"(v)); break;
    case 6: __asm__ volatile("mrc p15, 0, %0, c6, c6, 0" : "=r"(v)); break;
    default: __asm__ volatile("mrc p15, 0, %0, c6, c7, 0" : "=r"(v)); break;
  }
  return v;
}

static void gpu_run_bench(void) {
  const u32 back = (u32)VRAM_BOT_BACK;
  u32 t0;

  if (!timer_calibrated())
    return; /* calibrated at boot; re-running it here would reset the ticks */

  /* 64 tiny blits: what remains is the cost of any GPU request. */
  t0 = timer_ticks();
  for (int i = 0; i < 64; i++)
    gpu_texcopy(back, back + 0x1000u, 16u);
  bench_op_us = timer_us_since(t0) / 64u;

  t0 = timer_ticks();
  draw_vgradient(VRAM_BOT_A, 0, 0, BOT_SCREEN_WIDTH, BOT_SCREEN_HEIGHT,
                 BOT_SCREEN_HEIGHT, COLOR_HM_BG_TOP, COLOR_HM_BG_BOT);
  bench_cpu_us = timer_us_since(t0);

  t0 = timer_ticks();
  gpu_fill(back, back + BOT_FB_SIZE, 0x00161616u, GPU_FILL_24);
  bench_psc_us = timer_us_since(t0);

  t0 = timer_ticks();
  gpu_texcopy(back, (u32)VRAM_BOT_PHYS, BOT_FB_SIZE);
  bench_blit_us = timer_us_since(t0);

  t0 = timer_ticks();
  ui_wallpaper(VRAM_BOT_A, BOT_SCREEN_WIDTH, BOT_SCREEN_HEIGHT,
               BOT_SCREEN_HEIGHT);
  bench_bg_us = timer_us_since(t0);

  t0 = timer_ticks();
  ui_patch_round_rect(VRAM_BOT_A, 10, 40, 300, 30, 8, 2, BOT_SCREEN_HEIGHT);
  bench_patch_us = timer_us_since(t0);

  t0 = timer_ticks();
  draw_gradient_round_rect(VRAM_BOT_A, 10, 40, 300, 30, 8, BOT_SCREEN_HEIGHT,
                           COLOR_HM_SLOT_TOP, COLOR_HM_SLOT_BOT);
  bench_card_us = timer_us_since(t0);
}

static void gputest_draw(const GpuShared *g, const char *last, int lastok) {
  clear_screen(VRAM_BOT_A, BOT_FB_SIZE, COLOR_HM_BG);
  draw_string(VRAM_BOT_A, 8, 6, BOT_SCREEN_HEIGHT, L(STR_GPU_TEST),
              COLOR_HM_TEXT2, COLOR_HM_BG);

  char line[48], num[16], *p;
  int y = 28;

  p = snd_cpy(line, "core v");
  snd_u32(num, audio_version());
  p = snd_cpy(p, num);
  p = snd_cpy(p, "  gpu ");
  p = snd_cpy(p, gpu_alive() ? (g->ready ? "ready" : "seen") : "---");
  draw_string(VRAM_BOT_A, 8, y, BOT_SCREEN_HEIGHT, line,
              g->ready ? COLOR_AURORA : COLOR_ORANGE, COLOR_HM_BG);
  y += 18;

  p = snd_cpy(line, "HW ID ");
  snd_hex(num, g->hw_id);
  p = snd_cpy(p, num);
  int id_ok = g->hw_id != 0 && g->hw_id != 0xFFFFFFFF;
  draw_string(VRAM_BOT_A, 8, y, BOT_SCREEN_HEIGHT, line,
              id_ok ? COLOR_AURORA : COLOR_ORANGE, COLOR_HM_BG);
  y += 18;

  p = snd_cpy(line, "step ");
  p = snd_cpy(p, g->step < 8 ? gpu_step_names[g->step] : "?");
  p = snd_cpy(p, "  err ");
  p = snd_cpy(p, g->err < 5 ? gpu_err_names[g->err] : "?");
  draw_string(VRAM_BOT_A, 8, y, BOT_SCREEN_HEIGHT, line,
              g->err ? COLOR_ORANGE : COLOR_HM_TEXT2, COLOR_HM_BG);
  y += 18;

  p = snd_cpy(line, "busy ");
  snd_hex(num, g->busy_before);
  p = snd_cpy(p, num);
  *p++ = '>';
  snd_hex(num, g->busy_after);
  snd_cpy(p, num);
  draw_string(VRAM_BOT_A, 8, y, BOT_SCREEN_HEIGHT, line, COLOR_HM_TEXT2,
              COLOR_HM_BG);
  y += 18;

  p = snd_cpy(line, "ctl ");
  snd_hex(num, g->ctl_after);
  p = snd_cpy(p, num);
  p = snd_cpy(p, " wait ");
  snd_u32(num, g->waits);
  p = snd_cpy(p, num);
  p = snd_cpy(p, " ops ");
  snd_u32(num, g->ops);
  snd_cpy(p, num);
  draw_string(VRAM_BOT_A, 8, y, BOT_SCREEN_HEIGHT, line, COLOR_HM_TEXT2,
              COLOR_HM_BG);
  y += 18;

  if (last) {
    p = snd_cpy(line, "last: ");
    snd_cpy(p, last);
    draw_string(VRAM_BOT_A, 8, y, BOT_SCREEN_HEIGHT, line,
                lastok ? COLOR_AURORA : COLOR_ORANGE, COLOR_HM_BG);
  }

  if (bench_op_us || bench_cpu_us) {
    p = snd_cpy(line, "op ");
    snd_u32(num, bench_op_us);
    p = snd_cpy(p, num);
    p = snd_cpy(p, "us  psc ");
    snd_u32(num, bench_psc_us);
    p = snd_cpy(p, num);
    p = snd_cpy(p, "us");
    draw_string(VRAM_BOT_A, 8, y, BOT_SCREEN_HEIGHT, line, COLOR_AURORA,
                COLOR_HM_BG);
    y += 14;
    p = snd_cpy(line, "cpu bg ");
    snd_u32(num, bench_cpu_us);
    p = snd_cpy(p, num);
    p = snd_cpy(p, "us  blit ");
    snd_u32(num, bench_blit_us);
    p = snd_cpy(p, num);
    snd_cpy(p, "us");
    draw_string(VRAM_BOT_A, 8, y, BOT_SCREEN_HEIGHT, line, COLOR_AURORA,
                COLOR_HM_BG);
    y += 14;
    p = snd_cpy(line, "bg paint ");
    snd_u32(num, bench_bg_us);
    p = snd_cpy(p, num);
    p = snd_cpy(p, "us  edge ");
    snd_u32(num, bench_patch_us);
    p = snd_cpy(p, num);
    p = snd_cpy(p, "us  card ");
    snd_u32(num, bench_card_us);
    p = snd_cpy(p, num);
    snd_cpy(p, "us");
    draw_string(VRAM_BOT_A, 8, y, BOT_SCREEN_HEIGHT, line, COLOR_AURORA,
                COLOR_HM_BG);
  }

  y += 16;
  p = snd_cpy(line, mpu_on ? "on  c1 " : "OFF c1 ");
  snd_hex(num, cp15_c1());
  p = snd_cpy(p, num);
  p = snd_cpy(p, " c2 ");
  snd_hex(num, cp15_c2());
  p = snd_cpy(p, num);
  p = snd_cpy(p, " c3 ");
  snd_hex(num, cp15_c3());
  snd_cpy(p, num);
  draw_string(VRAM_BOT_A, 8, y, BOT_SCREEN_HEIGHT, line, COLOR_WHITE,
              COLOR_HM_BG);
  for (int grp = 0; grp < 4; grp++) {
    y += 14;
    p = line;
    for (int i = 0; i < 2; i++) {
      snd_hex(num, cp15_region(grp * 2 + i));
      p = snd_cpy(p, num);
      p = snd_cpy(p, " ");
    }
    *p = 0;
    draw_string(VRAM_BOT_A, 8, y, BOT_SCREEN_HEIGHT, line, COLOR_WHITE,
                COLOR_HM_BG);
  }

  draw_string(VRAM_BOT_A, 8, BOT_SCREEN_HEIGHT - 15, BOT_SCREEN_HEIGHT,
              "X:Fill Y:Blit L:Bench R:Render B:Back", COLOR_HM_TEXT2,
              COLOR_HM_BG);
  screen_present_bottom();
}

static void gpu_test_screen(void) {
  settings_header(ASSET_ICON_MONITOR_64, icon_boot_bits, L(STR_GPU_TEST),
                  COLOR_WHITE);
  static GpuShared g;
  const char *last = 0;
  int lastok = 0;
  static const u32 fill_colors[] = {0x00301060u, 0x00104030u, 0x00603010u};
  int fill_idx = 0;

  int ok = gpu_init();
  gpu_get(&g);
  last = ok ? "init" : "init failed";
  lastok = ok;
  gputest_draw(&g, last, lastok);

  while (1) {
    u32 k = get_keys_down();

    if (k & BUTTON_A) {
      lastok = gpu_init();
      last = lastok ? "init" : "init failed";
    } else if (k & BUTTON_X) {
      /* Straight to the panel, so it shows without a present. */
      lastok = gpu_clear_fb((u32)VRAM_TOP_PHYS, TOP_FB_SIZE,
                            fill_colors[fill_idx]);
      fill_idx = (fill_idx + 1) % 3;
      last = lastok ? "PSC fill" : "PSC fill failed";
    } else if (k & BUTTON_Y) {
      gpu_paint_scratch();
      lastok = gpu_texcopy(GPU_SCRATCH, (u32)VRAM_TOP_PHYS, TOP_FB_SIZE);
      last = lastok ? "PPF blit" : "PPF blit failed";
    } else if (k & BUTTON_L) {
      gpu_run_bench();
      last = "benchmark";
      lastok = 1;
    } else if (k & BUTTON_R) {
      render_test_screen();
      settings_header(ASSET_ICON_MONITOR_64, icon_boot_bits, L(STR_GPU_TEST),
                      COLOR_WHITE);
      last = "render test";
      lastok = 1;
    } else if (k & BUTTON_B) {
      return;
    } else {
      ui_idle();
      continue;
    }

    gpu_get(&g);
    gputest_draw(&g, last, lastok);
  }
}

static char *about_u32(char *p, u32 v) {
  char tmp[12];
  int n = 0;
  if (!v)
    *p++ = '0';
  while (v) {
    tmp[n++] = (char)('0' + v % 10u);
    v /= 10u;
  }
  while (n--)
    *p++ = tmp[n];
  return p;
}

static char *about_str(char *p, const char *s) {
  while (*s)
    *p++ = *s++;
  return p;
}

/* 2^21 sectors to a GB, so this is a shift rather than a 64-bit divide. */
static char *about_gb(char *p, unsigned long long sectors) {
  u32 tenths = (u32)((sectors * 10ull + (1ull << 20)) >> 21);
  p = about_u32(p, tenths / 10u);
  *p++ = '.';
  *p++ = (char)('0' + tenths % 10u);
  return p;
}

#define ABOUT_ROWS 5
#define ABOUT_Y    14
#define ABOUT_STEP 32

static void about_top(void) {
  volatile u8 *fb = VRAM_TOP_LA;
  const int sh = TOP_SCREEN_HEIGHT;
  static const char *const credits[] = {
      "Wi-Fi driver: Octoblimp (GPL-2.0)",
      "GPU driver: Linux 3DS PICA200 (GPL-2.0)",
      "Clock switch: fastboot3DS, libn3ds (GPL-3.0)",
      "Font: Figtree (SIL OFL 1.1)",
  };

  ui_wallpaper(fb, TOP_SCREEN_WIDTH, sh, sh);
  hm_status_bar();
  ui_icon(fb, (TOP_SCREEN_WIDTH - 64) / 2, 30, 64, sh, ASSET_ICON_INFO_64,
          icon_settings_bits, g_accent);
  ui_text_mid(fb, TOP_SCREEN_WIDTH / 2, 98, sh, "AuroraOS", COLOR_WHITE,
              COLOR_HM_BG_BOT, &ui_title);
  ui_text_mid(fb, TOP_SCREEN_WIDTH / 2, 124, sh, AURORA_VERSION,
              COLOR_HM_TEXT2, COLOR_HM_BG_BOT, &ui_font);
  draw_gradient_round_rect(fb, 40, 150, TOP_SCREEN_WIDTH - 80, 82, 11, sh,
                           COLOR_PANEL_TOP, COLOR_PANEL_BOT);
  for (int i = 0; i < 4; i++)
    ui_text_mid(fb, TOP_SCREEN_WIDTH / 2, 157 + i * 18, sh, credits[i],
                COLOR_WHITE, COLOR_PANEL_TOP, &ui_small);
  screen_present_top();
}

#define ABOUT_BTN_W  136
#define ABOUT_BTN_H  36
#define ABOUT_BTN_Y  (BOT_SCREEN_HEIGHT - ABOUT_BTN_H - 12)
#define ABOUT_MORE_X 12
#define ABOUT_BACK_X (BOT_SCREEN_WIDTH - 12 - ABOUT_BTN_W)

static void about_button(int x, const char *label, int primary) {
  static const Color ink_dark = {0x12, 0x12, 0x16};
  volatile u8 *fb = VRAM_BOT_A;
  const int sh = BOT_SCREEN_HEIGHT;

  if (primary)
    draw_filled_round_rect(fb, x, ABOUT_BTN_Y, ABOUT_BTN_W, ABOUT_BTN_H, 10, sh,
                           g_accent);
  else
    draw_gradient_round_rect(fb, x, ABOUT_BTN_Y, ABOUT_BTN_W, ABOUT_BTN_H, 10,
                             sh, COLOR_HM_SLOT_TOP, COLOR_HM_SLOT_BOT);
  ui_text_mid(fb, x + ABOUT_BTN_W / 2,
              ABOUT_BTN_Y + (ABOUT_BTN_H - ui_th(&ui_bold)) / 2, sh, label,
              primary ? ink_dark : COLOR_WHITE,
              primary ? g_accent : COLOR_HM_SLOT, &ui_bold);
}

static void about_bottom(const char *sd) {
  static const Color rule = {0x30, 0x30, 0x34};
  volatile u8 *fb = VRAM_BOT_A;
  const int sh = BOT_SCREEN_HEIGHT;
  const int px = 12, pw = BOT_SCREEN_WIDTH - 24;
  const int ph = 12 + (ABOUT_ROWS - 1) * ABOUT_STEP + ui_th(&ui_font) + 12;
  const char *label[ABOUT_ROWS] = {L(STR_VERSION), L(STR_CONSOLE),
                                   L(STR_BATTERY), L(STR_SD_CARD),
                                   L(STR_LICENSE)};
  const char *value[ABOUT_ROWS];
  char bat[40], *p = bat;
  int pct = battery_percent();

  if (pct < 0) {
    p = about_str(p, "---");
  } else {
    p = about_u32(p, (u32)pct);
    *p++ = '%';
    if (battery_charging() > 0) {
      p = about_str(p, ", ");
      p = about_str(p, L(STR_CHARGING));
    }
  }
  *p = 0;

  value[0] = AURORA_VERSION;
  value[1] = aurora_is_new3ds() ? "New 3DS" : "Old 3DS";
  value[2] = bat;
  value[3] = sd;
  value[4] = "GPL-3.0";

  ui_wallpaper(fb, BOT_SCREEN_WIDTH, sh, sh);
  draw_gradient_round_rect(fb, px, ABOUT_Y, pw, ph, 12, sh, COLOR_PANEL_TOP,
                           COLOR_PANEL_BOT);
  for (int i = 0; i < ABOUT_ROWS; i++) {
    int y = ABOUT_Y + 12 + i * ABOUT_STEP;
    if (i)
      draw_filled_rect(fb, px + 12, y - 8, pw - 24, 1, sh, rule);
    ui_text(fb, px + 14, y, sh, label[i], COLOR_HM_TEXT2, COLOR_PANEL_TOP,
            &ui_font);
    ui_text(fb, px + pw - 14 - ui_tw(&ui_font, value[i]), y, sh, value[i],
            COLOR_WHITE, COLOR_PANEL_TOP, &ui_font);
  }
  about_button(ABOUT_MORE_X, L(STR_MORE_INFO), 1);
  about_button(ABOUT_BACK_X, L(STR_BACK), 0);
  screen_present_bottom();
}

/* The printed serial lives in encrypted NAND, so More Info shows the eMMC CID
 * instead. */
#define INFO_ROWS 4
#define INFO_X    50
#define INFO_Y    100
#define INFO_STEP 30
#define INFO_W    (TOP_SCREEN_WIDTH - 2 * INFO_X)

typedef struct {
  u32 asset;
  const char *name;
  const char *role;
} Person;

static const Person team[] = {
    {ASSET_PERSON_DISLOPIK_40, "DisLoPik", "CEO"},
    {ASSET_PERSON_STAR_40,     "Star",     "Founder"},
    {ASSET_PERSON_ATEXBG_40,   "AtexBg",   "Low-level Developer"},
    {ASSET_PERSON_KYAERO_40,   "Kyaero",   "Interface Designer"},
    {ASSET_PERSON_SECRET_40,   "Secret",   "Drivers and GPIO"},
};

#define TEAM_COUNT ((int)(sizeof(team) / sizeof(team[0])))
#define TEAM_X     16
#define TEAM_Y0    8
#define TEAM_STEP  45
#define TEAM_AV    40

/* The eMMC CID as GodMode9 prints it: the 16 bytes in order. `out` takes 33
 * bytes. */
static const char *info_cid(char *out) {
  static const char digits[] = "0123456789ABCDEF";
  u8 cid[16];

  if (!sdmmc_nand_cid(cid))
    return L(STR_NOT_AVAILABLE);
  for (int i = 0; i < 16; i++) {
    out[i * 2] = digits[cid[i] >> 4];
    out[i * 2 + 1] = digits[cid[i] & 0xF];
  }
  out[32] = 0;
  return out;
}

static void info_top(void) {
  static const Color rule = {0x30, 0x30, 0x34};
  volatile u8 *fb = VRAM_TOP_LA;
  const int sh = TOP_SCREEN_HEIGHT;
  const char *label[INFO_ROWS] = {L(STR_VERSION), L(STR_SOFTWARE_UPDATE),
                                  L(STR_EMMC_CID), L(STR_DEVICE)};
  const char *value[INFO_ROWS];
  char cid[33];

  value[0] = AURORA_VERSION;
  value[1] = L(STR_UPDATE_NONE);
  value[2] = info_cid(cid);
  value[3] = aurora_is_new3ds() ? "New Nintendo 3DS" : "Nintendo 3DS";

  ui_wallpaper(fb, TOP_SCREEN_WIDTH, sh, sh);
  hm_status_bar();
  draw_filled_round_rect(fb, INFO_X, 46, INFO_W, 38, 10, sh, COLOR_HM_BAR);
  ui_text_mid(fb, TOP_SCREEN_WIDTH / 2, 46 + (38 - ui_th(&ui_bold)) / 2, sh,
              "Aurora for Nintendo 3DS", COLOR_WHITE, COLOR_HM_BAR, &ui_bold);

  for (int i = 0; i < INFO_ROWS; i++) {
    char shown[64];
    int y = INFO_Y + i * INFO_STEP;
    int lw = ui_tw(&ui_font, label[i]);
    int avail = INFO_W - lw - 16;
    /* The CID is 32 characters, which only fits at the smaller size. */
    const Font *vf = ui_tw(&ui_font, value[i]) > avail ? &ui_small : &ui_font;

    if (i)
      draw_filled_rect(fb, INFO_X, y - 8, INFO_W, 1, sh, rule);
    ui_text(fb, INFO_X, y, sh, label[i], COLOR_WHITE, COLOR_HM_BG_BOT,
            &ui_font);
    ui_fit(shown, (int)sizeof(shown), vf, value[i], avail);
    ui_text(fb, INFO_X + INFO_W - ui_tw(vf, shown),
            y + (ui_th(&ui_font) - ui_th(vf)) / 2, sh, shown, COLOR_HM_TEXT2,
            COLOR_HM_BG_BOT, vf);
  }
  ui_text_mid(fb, TOP_SCREEN_WIDTH / 2, sh - 24, sh, L(STR_B_BACK),
              COLOR_HM_TEXT2, COLOR_HM_BG_BOT, &ui_small);
  screen_present_top();
}

static void info_bottom(void) {
  volatile u8 *fb = VRAM_BOT_A;
  const int sh = BOT_SCREEN_HEIGHT;

  ui_wallpaper(fb, BOT_SCREEN_WIDTH, sh, sh);
  draw_gradient_round_rect(fb, 8, 2, BOT_SCREEN_WIDTH - 16, sh - 6, 14, sh,
                           COLOR_PANEL_TOP, COLOR_PANEL_BOT);
  for (int i = 0; i < TEAM_COUNT; i++) {
    int y = TEAM_Y0 + i * TEAM_STEP;
    /* A disc under the face, so the row still reads without the asset pack. */
    draw_filled_round_rect(fb, TEAM_X, y, TEAM_AV, TEAM_AV, TEAM_AV / 2, sh,
                           COLOR_HM_SLOT);
    ui_icon(fb, TEAM_X, y, TEAM_AV, sh, team[i].asset, 0, COLOR_WHITE);
    ui_text(fb, TEAM_X + TEAM_AV + 14, y + 2, sh, team[i].name, COLOR_WHITE,
            COLOR_PANEL_TOP, &ui_bold);
    ui_text(fb, TEAM_X + TEAM_AV + 14, y + 22, sh, team[i].role,
            COLOR_HM_TEXT2, COLOR_PANEL_TOP, &ui_small);
  }
  screen_present_bottom();
}

static void info_screen(void) {
  info_top();
  info_bottom();
  for (;;) {
    u32 k = get_keys_down();
    int tx, ty;
    if ((k & (BUTTON_A | BUTTON_B)) || touch_tap(&tx, &ty))
      return;
    ui_idle();
  }
}

static void about_screen(void) {
  static FATFS afs;
  char sd[48], *p = sd;
  DWORD free_clst;
  FATFS *fs;

  about_top();
  about_bottom("...");

  if (f_mount(&afs, "", 1) != FR_OK) {
    p = about_str(p, L(STR_NO_CARD));
  } else {
    if (f_getfree("", &free_clst, &fs) == FR_OK) {
      p = about_gb(p, (unsigned long long)free_clst * fs->csize);
      p = about_str(p, " / ");
      p = about_gb(p, (unsigned long long)(fs->n_fatent - 2u) * fs->csize);
      p = about_str(p, " GB ");
      p = about_str(p, L(STR_FREE));
    } else {
      p = about_str(p, "---");
    }
    f_mount(NULL, "", 0);
  }
  *p = 0;
  ui_idle(); /* collect the present before drawing into that buffer again */
  about_bottom(sd);

  while (1) {
    u32 k = get_keys_down();
    int tx, ty, more = (k & BUTTON_A) != 0;

    if (touch_tap(&tx, &ty)) {
      if (touch_in(tx, ty, ABOUT_MORE_X, ABOUT_BTN_Y, ABOUT_BTN_W, ABOUT_BTN_H))
        more = 1;
      else if (touch_in(tx, ty, ABOUT_BACK_X, ABOUT_BTN_Y, ABOUT_BTN_W,
                        ABOUT_BTN_H))
        return;
    }
    if (k & BUTTON_B)
      return;
    if (more) {
      info_screen();
      about_top();
      about_bottom(sd);
    }
    ui_idle();
  }
}

/* A new calibration is in use as soon as it is accepted; saving only decides
 * whether it survives a restart. */
static void touch_cal_screen(void) {
  TouchCal cal;
  int ok;

  if (!touch_calibrate(&cal))
    return;
  g_cfg.touch = cal;
  g_cfg.touch_set = 1;
  ok = user_config_save(&g_cfg);
  ui_wallpaper(VRAM_BOT_A, BOT_SCREEN_WIDTH, BOT_SCREEN_HEIGHT,
               BOT_SCREEN_HEIGHT);
  ui_dialog(VRAM_BOT_A, BOT_SCREEN_WIDTH, BOT_SCREEN_HEIGHT, BOT_SCREEN_HEIGHT,
            L(ok ? STR_CAL_SAVED : STR_CAL_SAVE_FAILED), "", g_accent, 0);
  screen_present_bottom();
  for (;;) {
    u32 k = get_keys_down();
    int tx, ty;
    if ((k & (BUTTON_A | BUTTON_B)) || touch_tap(&tx, &ty))
      return;
    ui_idle();
  }
}

static void settings_open(void) {
  settings_header(ASSET_ICON_SETTINGS_64, icon_settings_bits,
                  L(STR_SETTINGS), COLOR_WHITE);
  int sel = 0;
  settings_draw(sel);
  while (1) {
    u32 k = get_keys_down();
    int prev = sel;
    int activate = 0;

    if ((k & BUTTON_DUP) && sel > 0)
      sel--;
    if ((k & BUTTON_DDOWN) && sel < SET_COUNT - 1)
      sel++;

    int tx, ty;
    if (touch_tap(&tx, &ty)) {
      int top = sel - SET_VISIBLE / 2;
      if (top > SET_COUNT - SET_VISIBLE)
        top = SET_COUNT - SET_VISIBLE;
      if (top < 0)
        top = 0;
      for (int r = 0; r < SET_VISIBLE && top + r < SET_COUNT; r++) {
        int y = ROW_Y0 + r * ROW_STEP;
        if (touch_in(tx, ty, ROW_X - 3, y - 3, ROW_W + 6, ROW_H + 6)) {
          sel = top + r;
          activate = 1;
          break;
        }
      }
    }

    if (sel != prev)
      settings_update(prev, sel);
    if ((k & BUTTON_A) || activate) {
      if (sel == SET_WIFI)
        wifi_screen();
      else if (sel == SET_ACCENT)
        accent_screen();
      else if (sel == SET_TOUCHCAL)
        touch_cal_screen();
      else if (sel == SET_WIFITEST)
        wifi_test_screen();
      else if (sel == SET_GPUTEST)
        gpu_test_screen();
      else if (sel == SET_ABOUT)
        about_screen();
      else if (sel == SET_CRASH)
        crash_force(); /* never returns: shows the crash screen */
      settings_header(ASSET_ICON_SETTINGS_64, icon_settings_bits,
                  L(STR_SETTINGS), COLOR_WHITE);
      settings_draw(sel);
    }
    if (k & BUTTON_B)
      return;
    ui_idle();
  }
}

static char *hm_str_copy(char *dst, const char *src) {
  while ((*dst = *src)) {
    dst++;
    src++;
  }
  return dst;
}

/* `ext` in capitals. Long names keep the case they were copied with, so the
 * match ignores it. */
static int hm_has_ext(const char *name, const char *ext) {
  int n = (int)str_len(name), e = (int)str_len(ext);
  if (n <= e)
    return 0;
  for (int i = 0; i < e; i++) {
    char c = name[n - e + i];
    if (c >= 'a' && c <= 'z')
      c = (char)(c - 32);
    if (c != ext[i])
      return 0;
  }
  return 1;
}

/* The first `n` bytes of `src`, backed off to a whole UTF-8 character. */
static void hm_copy_cut(char *dst, const char *src, int n) {
  while (n > 0 && ((u8)src[n] & 0xC0u) == 0x80u)
    n--;
  for (int i = 0; i < n; i++)
    dst[i] = src[i];
  dst[n] = '\0';
}

static int hm_name_cmp(const char *a, const char *b) {
  while (*a && *a == *b) {
    a++;
    b++;
  }
  return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static int read_app_icon(const char *path, unsigned char *dest) {
  static FIL f;
  UINT br;
  if (f_open(&f, path, FA_READ) != FR_OK)
    return 0;

  aos_header_t hdr;
  if (aurora_parse_header(&f, &hdr) != AURORA_OK) {
    f_close(&f);
    return 0;
  }

  char magic[8];
  int ok = 1;
  if (f_lseek(&f, hdr.arm9_offset + AURORA_ICON_MAGIC_OFFSET) != FR_OK ||
      f_read(&f, magic, sizeof(magic), &br) != FR_OK || br != sizeof(magic)) {
    f_close(&f);
    return 0;
  }
  const char *m = AURORA_ICON_MAGIC;
  for (int i = 0; i < 8; i++)
    if (magic[i] != m[i])
      ok = 0;
  if (ok) {
    if (f_read(&f, dest, AURORA_ICON_BYTES, &br) != FR_OK ||
        br != AURORA_ICON_BYTES)
      ok = 0;
  }
  f_close(&f);
  return ok;
}

static int scan_apps(void) {
  static FATFS scan_fs;
  static DIR dir;
  static FILINFO fno;
  app_count = 0;

  if (f_mount(&scan_fs, "", 1) != FR_OK)
    return 0;
  if (f_opendir(&dir, APPS_DIR) != FR_OK) {
    f_mount(NULL, "", 0);
    return 0;
  }
  while (app_count < MAX_APPS && f_readdir(&dir, &fno) == FR_OK &&
         fno.fname[0]) {
    if (fno.fattrib & AM_DIR)
      continue;
    if (!hm_has_ext(fno.fname, ".BIN"))
      continue;
    int n = (int)str_len(fno.fname) - 4; /* strip ".bin" for the display name */
    hm_copy_cut(app_name[app_count], fno.fname,
                n < APP_NAME_MAX - 1 ? n : APP_NAME_MAX - 1);
    char *p = hm_str_copy(app_path[app_count], APPS_DIR "/");
    hm_str_copy(p, fno.fname);
    app_count++;
  }
  f_closedir(&dir);

  for (int i = 1; i < app_count; i++) {
    static char tn[APP_NAME_MAX], tp[APP_PATH_MAX];
    hm_str_copy(tn, app_name[i]);
    hm_str_copy(tp, app_path[i]);
    int j = i - 1;
    while (j >= 0 && hm_name_cmp(app_name[j], tn) > 0) {
      hm_str_copy(app_name[j + 1], app_name[j]);
      hm_str_copy(app_path[j + 1], app_path[j]);
      j--;
    }
    hm_str_copy(app_name[j + 1], tn);
    hm_str_copy(app_path[j + 1], tp);
  }

  for (int i = 0; i < app_count; i++)
    app_has_icon[i] = read_app_icon(app_path[i], app_icon[i]);

  f_mount(NULL, "", 0);
  return app_count;
}

static void build_home(void) {
  static const Color app_tint = {0x2C, 0x50, 0x74};
  for (int i = 0; i < HOME_COUNT; i++) {
    home_apps[i] = (HomeApp){0};
    home_apps[i].asset_lg = UI_NO_ASSET;
    home_apps[i].asset_sm = UI_NO_ASSET;
  }

  int slot = 0;
  for (int i = 0; i < app_count && slot < HOME_COUNT; i++, slot++) {
    home_apps[slot].name = app_name[i];
    home_apps[slot].dev = "SD App";
    home_apps[slot].icon = app_has_icon[i] ? app_icon[i] : icon_boot_bits;
    if (!app_has_icon[i]) {
      home_apps[slot].asset_lg = ASSET_ICON_GAMECARD_64;
      home_apps[slot].asset_sm = ASSET_ICON_GAMECARD_32;
    }
    home_apps[slot].tint = app_tint;
    home_apps[slot].action = ACT_LAUNCH;
    home_apps[slot].path = app_path[i];
  }
  if (slot < HOME_COUNT) {
    static const Color music_tint = {0x2C, 0x5C, 0x40};
    home_apps[slot].name = L(STR_MUSIC);
    home_apps[slot].dev = "Aurora";
    home_apps[slot].icon = icon_music_bits;
    home_apps[slot].asset_lg = ASSET_ICON_MUSIC_64;
    home_apps[slot].asset_sm = ASSET_ICON_MUSIC_32;
    home_apps[slot].tint = music_tint;
    home_apps[slot].action = ACT_MUSIC;
    slot++;
  }
  if (slot < HOME_COUNT) {
    static const Color files_tint = {0x74, 0x52, 0x1C};
    home_apps[slot].name = "Files";
    home_apps[slot].dev = L(STR_SYSTEM);
    home_apps[slot].icon = icon_boot_bits;
    home_apps[slot].asset_lg = ASSET_APP_FILES_64;
    home_apps[slot].asset_sm = ASSET_APP_FILES_32;
    home_apps[slot].tint = files_tint;
    home_apps[slot].action = ACT_FILES;
    slot++;
  }
  if (slot < HOME_COUNT) {
    home_apps[slot].name = L(STR_POWER_OFF);
    home_apps[slot].dev = L(STR_SYSTEM);
    home_apps[slot].icon = icon_power_bits;
    home_apps[slot].asset_lg = ASSET_ICON_POWER_64;
    home_apps[slot].asset_sm = ASSET_ICON_POWER_32;
    home_apps[slot].tint = COLOR_DARK_RED;
    home_apps[slot].action = ACT_POWER;
  }
}

static void launch_msg(const char *msg, Color color) {
  ui_wallpaper(VRAM_BOT_A, BOT_SCREEN_WIDTH, BOT_SCREEN_HEIGHT,
               BOT_SCREEN_HEIGHT);
  ui_dialog(VRAM_BOT_A, BOT_SCREEN_WIDTH, BOT_SCREEN_HEIGHT, BOT_SCREEN_HEIGHT,
            "Launch app", msg, color, 0);
  screen_present_bottom();
}

static void launch_wait_back(void) {
  while (1) {
    if (get_keys_down() & BUTTON_B)
      return;
    ui_idle();
  }
}

/* Lets HOME restart the Home Menu after an app: OS snapshot, descriptor and
 * return stub. */
static void os_install_return(void) {
  u32 os_size = (u32)((const unsigned char *)_os_image_end -
                      (const unsigned char *)AOS_ARM9_LOAD_ADDR);

  volatile u8 *src = (volatile u8 *)AOS_ARM9_LOAD_ADDR;
  volatile u8 *snap = (volatile u8 *)AURORA_OS_SNAPSHOT_ADDR;
  for (u32 i = 0; i < os_size; i++)
    snap[i] = src[i];

  volatile u32 *desc = (volatile u32 *)AURORA_RETURN_DESC_ADDR;
  desc[0] = AURORA_RETURN_READY_MAGIC;
  desc[1] = os_size;

  volatile u8 *rs = (volatile u8 *)AURORA_RETURN_STUB_ADDR;
  const unsigned char *rc = os_return_stub;
  u32 rn = (u32)(os_return_stub_end - os_return_stub);
  for (u32 i = 0; i < rn; i++)
    rs[i] = rc[i];

  os_cache_sync();
}

/* Runs the hand-off stub from scratch memory, clear of both the staged
 * payload and the load region. Never returns. */
static void os_run_stub(u32 src, u32 dst, u32 size, u32 entry) {
  volatile u8 *s = (volatile u8 *)os_launch_stub;
  volatile u8 *d = (volatile u8 *)AURORA_APP_TRAMPOLINE_ADDR;
  u32 n = (u32)(os_launch_stub_end - os_launch_stub);
  for (u32 i = 0; i < n; i++)
    d[i] = s[i];
  os_cache_sync(); /* make the relocated code fetchable, not stale in I-cache */
  ((void (*)(u32, u32, u32, u32))AURORA_APP_TRAMPOLINE_ADDR)(src, dst, size,
                                                             entry);
}

/* Never returns on success: the app replaces the Home Menu. On an error it
 * shows a message and returns. */
static void os_launch_app(const char *path) {
  static FATFS app_fs;
  static FIL app_file;

  launch_msg(L(STR_LOADING), COLOR_WHITE);

  if (f_mount(&app_fs, "", 1) != FR_OK) {
    launch_msg("SD mount failed.  B: back", COLOR_RED);
    launch_wait_back();
    return;
  }
  if (f_open(&app_file, path, FA_READ) != FR_OK) {
    launch_msg("Open failed.  B: back", COLOR_RED);
    f_mount(NULL, "", 0);
    launch_wait_back();
    return;
  }

  aos_header_t hdr;
  aurora_status_t st = aurora_parse_header(&app_file, &hdr);
  if (st != AURORA_OK) {
    launch_msg(st == AURORA_ERR_MAGIC ? "Not an AUR1 app.  B: back"
                                      : "Header read failed.  B: back",
               COLOR_RED);
    f_close(&app_file);
    f_mount(NULL, "", 0);
    launch_wait_back();
    return;
  }
  if (aurora_load_arm9(&app_file, &hdr, (void *)AURORA_APP_STAGE_ADDR) !=
      AURORA_OK) {
    launch_msg("Payload read failed.  B: back", COLOR_RED);
    f_close(&app_file);
    f_mount(NULL, "", 0);
    launch_wait_back();
    return;
  }
  f_close(&app_file);
  f_mount(NULL, "", 0);

  os_install_return();

  os_run_stub(AURORA_APP_STAGE_ADDR, hdr.arm9_load_addr, hdr.arm9_size,
              hdr.arm9_entry);
}

/* Music: .aaf files, mono PCM (see audio/aaf_tool.py). */

#define MUSIC_DIR    "Aurora/Music"
#define MAX_TRACKS   32
#define MUS_NAME_MAX 64
#define MUS_PATH_MAX (sizeof(MUSIC_DIR) + FF_LFN_BUF + 1)
static char mus_name[MAX_TRACKS][MUS_NAME_MAX]; /* file name, cut for display */
static char mus_path[MAX_TRACKS][MUS_PATH_MAX]; /* "Aurora/Music/Name.aaf"    */
static int  mus_count;

static int scan_music(void) {
  static FATFS mfs;
  static DIR dir;
  static FILINFO fno;
  mus_count = 0;

  if (f_mount(&mfs, "", 1) != FR_OK)
    return 0;
  f_mkdir(MUSIC_DIR); /* create if missing; FR_EXIST is fine */
  if (f_opendir(&dir, MUSIC_DIR) != FR_OK) {
    f_mount(NULL, "", 0);
    return 0;
  }
  while (mus_count < MAX_TRACKS && f_readdir(&dir, &fno) == FR_OK &&
         fno.fname[0]) {
    if (fno.fattrib & AM_DIR)
      continue;
    if (!hm_has_ext(fno.fname, ".AAF"))
      continue;
    int n = (int)str_len(fno.fname);
    hm_copy_cut(mus_name[mus_count], fno.fname,
                n < MUS_NAME_MAX - 1 ? n : MUS_NAME_MAX - 1);
    char *p = hm_str_copy(mus_path[mus_count], MUSIC_DIR "/");
    hm_str_copy(p, fno.fname);
    mus_count++;
  }
  f_closedir(&dir);

  for (int i = 1; i < mus_count; i++) {
    static char tn[MUS_NAME_MAX], tp[MUS_PATH_MAX];
    hm_str_copy(tn, mus_name[i]);
    hm_str_copy(tp, mus_path[i]);
    int j = i - 1;
    while (j >= 0 && hm_name_cmp(mus_name[j], tn) > 0) {
      hm_str_copy(mus_name[j + 1], mus_name[j]);
      hm_str_copy(mus_path[j + 1], mus_path[j]);
      j--;
    }
    hm_str_copy(mus_name[j + 1], tn);
    hm_str_copy(mus_path[j + 1], tp);
  }

  f_mount(NULL, "", 0);
  return mus_count;
}

/* Load track `idx` into AUDIO_PCM_ADDR. Returns samples loaded (>0), 0 on I/O
 * error, or -1 if the file is not an "AAF1" file. Fills rate + depth. */
static u8 aaf_hdr[16];
static int load_track(int idx, u32 *rate_out, u32 *depth_out) {
  static FATFS mfs;
  static FIL mf;
  UINT br = 0;

  if (f_mount(&mfs, "", 1) != FR_OK)
    return 0;
  if (f_open(&mf, mus_path[idx], FA_READ) != FR_OK) {
    f_mount(NULL, "", 0);
    return 0;
  }
  if (f_read(&mf, aaf_hdr, 16, &br) != FR_OK || br != 16) {
    f_close(&mf);
    f_mount(NULL, "", 0);
    return 0;
  }
  if (aaf_hdr[0] != 'A' || aaf_hdr[1] != 'A' || aaf_hdr[2] != 'F' ||
      aaf_hdr[3] != '1') {
    f_close(&mf);
    f_mount(NULL, "", 0);
    return -1;
  }
  u32 depth = aaf_hdr[6];
  u32 rate = (u32)aaf_hdr[8] | ((u32)aaf_hdr[9] << 8) |
             ((u32)aaf_hdr[10] << 16) | ((u32)aaf_hdr[11] << 24);
  u32 nsamp = (u32)aaf_hdr[12] | ((u32)aaf_hdr[13] << 8) |
              ((u32)aaf_hdr[14] << 16) | ((u32)aaf_hdr[15] << 24);
  u32 bpp = (depth == 8) ? 1u : 2u;
  u32 total = nsamp * bpp;
  if (total > AUDIO_PCM_MAX)
    total = AUDIO_PCM_MAX;

  UINT rd = 0;
  f_read(&mf, (void *)AUDIO_PCM_ADDR, total, &rd);
  f_close(&mf);
  f_mount(NULL, "", 0);

  *rate_out = rate;
  *depth_out = depth;
  return (int)(rd / bpp);
}

static void music_draw_top(int sel, int playing, Color accent) {
  ui_wallpaper(VRAM_TOP_LA, TOP_SCREEN_WIDTH, TOP_SCREEN_HEIGHT,
               TOP_SCREEN_HEIGHT);
  draw_filled_rect(VRAM_TOP_LA, 0, 0, TOP_SCREEN_WIDTH, 22, TOP_SCREEN_HEIGHT,
                   COLOR_HM_BAR);
  ui_text(VRAM_TOP_LA, 10, 4, TOP_SCREEN_HEIGHT, L(STR_MUSIC), COLOR_WHITE,
          COLOR_HM_BAR, &ui_bold);

  ui_icon(VRAM_TOP_LA, (TOP_SCREEN_WIDTH - 96) / 2, 40, 96, TOP_SCREEN_HEIGHT,
          ASSET_ICON_MUSIC_64, icon_music_bits, accent);

  const char *np = (mus_count > 0) ? mus_name[sel] : "---";
  const char *st = playing ? L(STR_PLAYING) : L(STR_STOPPED);
  ui_text_mid_fit(VRAM_TOP_LA, TOP_SCREEN_WIDTH / 2, 148, TOP_SCREEN_HEIGHT, np,
                  TOP_SCREEN_WIDTH - 40, COLOR_WHITE, COLOR_HM_BG, &ui_title);
  ui_text_mid(VRAM_TOP_LA, TOP_SCREEN_WIDTH / 2, 176, TOP_SCREEN_HEIGHT, st,
              playing ? accent : COLOR_HM_TEXT2, COLOR_HM_BG, &ui_font);
  screen_present_top();
}

#define MUS_VISIBLE 6
#define MUS_ROW_Y0  32
#define MUS_ROW_H   26
#define MUS_ROW_STEP 30

static void music_draw_bottom(int sel) {
  clear_screen(VRAM_BOT_A, BOT_FB_SIZE, COLOR_HM_BG);
  draw_string(VRAM_BOT_A, 12, 12, BOT_SCREEN_HEIGHT, L(STR_MUSIC),
              COLOR_HM_TEXT2, COLOR_HM_BG);

  if (mus_count == 0) {
    draw_string(VRAM_BOT_A, 12, 60, BOT_SCREEN_HEIGHT, L(STR_NO_TRACKS),
                COLOR_HM_TEXT2, COLOR_HM_BG);
    draw_string(VRAM_BOT_A, 12, 78, BOT_SCREEN_HEIGHT, "SD:/Aurora/Music",
                COLOR_HM_TEXT2, COLOR_HM_BG);
  } else {
    int top = sel - MUS_VISIBLE / 2;
    if (top < 0)
      top = 0;
    if (top > mus_count - MUS_VISIBLE)
      top = mus_count - MUS_VISIBLE;
    if (top < 0)
      top = 0;
    for (int r = 0; r < MUS_VISIBLE && top + r < mus_count; r++) {
      int i = top + r;
      int y = MUS_ROW_Y0 + r * MUS_ROW_STEP;
      int on = (i == sel);
      draw_filled_round_rect(VRAM_BOT_A, 10, y - 2, BOT_SCREEN_WIDTH - 20,
                             MUS_ROW_H + 4, 8, BOT_SCREEN_HEIGHT,
                             on ? g_accent : COLOR_HM_BG);
      draw_filled_round_rect(VRAM_BOT_A, 12, y, BOT_SCREEN_WIDTH - 24,
                             MUS_ROW_H, 6, BOT_SCREEN_HEIGHT, COLOR_HM_SLOT);
      ui_text_fit(VRAM_BOT_A, 22, y + (MUS_ROW_H - ui_th(&ui_font)) / 2,
                  BOT_SCREEN_HEIGHT, mus_name[i], BOT_SCREEN_WIDTH - 44,
                  COLOR_WHITE, COLOR_HM_SLOT, &ui_font);
    }
  }
  draw_string(VRAM_BOT_A, 12, BOT_SCREEN_HEIGHT - 18, BOT_SCREEN_HEIGHT,
              "A: Play   START: Stop   B: Back", COLOR_HM_TEXT2, COLOR_HM_BG);
  screen_present_bottom();
}

static void music_player_screen(void) {
  Color accent = g_accent;
  scan_music();
  int sel = 0, playing = 0;
  music_draw_top(sel, playing, accent);
  music_draw_bottom(sel);

  while (1) {
    u32 k = get_keys_down();
    int prev = sel;
    if ((k & BUTTON_DUP) && sel > 0)
      sel--;
    if ((k & BUTTON_DDOWN) && sel < mus_count - 1)
      sel++;

    int play_now = 0;
    int tx, ty;
    if (touch_tap(&tx, &ty) && mus_count > 0) {
      int top = sel - MUS_VISIBLE / 2;
      if (top < 0)
        top = 0;
      if (top > mus_count - MUS_VISIBLE)
        top = mus_count - MUS_VISIBLE;
      if (top < 0)
        top = 0;
      for (int r = 0; r < MUS_VISIBLE && top + r < mus_count; r++) {
        int y = MUS_ROW_Y0 + r * MUS_ROW_STEP;
        if (touch_in(tx, ty, 10, y - 2, BOT_SCREEN_WIDTH - 20, MUS_ROW_H + 4)) {
          sel = top + r;
          play_now = 1;
          break;
        }
      }
    }

    if (sel != prev) {
      music_draw_bottom(sel);
      music_draw_top(sel, playing, accent);
    }

    if (((k & BUTTON_A) || play_now) && mus_count > 0) {
      audio_stop(); /* free the buffer before overwriting it */
      draw_string(VRAM_BOT_A, 12, BOT_SCREEN_HEIGHT - 36, BOT_SCREEN_HEIGHT,
                  L(STR_LOADING), COLOR_AURORA, COLOR_HM_BG);
      screen_present_bottom();
      u32 rate = 8000, depth = 16;
      int n = load_track(sel, &rate, &depth);
      if (n > 0) {
        audio_play_pcm((u32)n, rate, depth);
        playing = 1;
      } else {
        playing = 0;
      }
      music_draw_bottom(sel);
      music_draw_top(sel, playing, accent);
    }
    if (k & BUTTON_START) {
      audio_stop();
      playing = 0;
      music_draw_top(sel, playing, accent);
    }
    if (k & BUTTON_B) {
      audio_stop();
      return;
    }
    ui_idle();
  }
}

static void home_activate(int sel) {
  if (home_apps[sel].action == ACT_POWER) {
    os_power_off();
  } else if (home_apps[sel].action == ACT_LAUNCH) {
    os_launch_app(home_apps[sel].path);
    hm_draw_full(sel); /* only reached if the launch failed and returned */
  } else if (home_apps[sel].action == ACT_MUSIC) {
    music_player_screen();
    hm_draw_full(sel);
  } else if (home_apps[sel].action == ACT_FILES) {
    files_screen(os_launch_app);
    hm_draw_full(sel);
  }
}

void os_main(void) {
  /* Protection unit and caches first; the firm leaves them off. Holding SELECT
   * at boot skips this, so a build that misbehaves with caches still starts. */
  if (!(get_keys() & BUTTON_SELECT)) {
    os_mpu_enable();
    mpu_on = 1;
  }

  /* First, so any later fault reaches the crash screen. */
  crash_init();

  I2C_init();

  /* FCRAM holds garbage on a cold boot, and a stray "pressed" would fire a
   * phantom tap on the first screen. */
  {
    volatile TouchShared *ts = (volatile TouchShared *)TOUCH_SHARED_ADDR;
    ts->seq = 0;
    ts->pressed = 0;
    ts->raw_x = 0;
    ts->raw_y = 0;
    os_cache_sync();
  }

  /* Before anything draws, since the ARM11 core owns the touchscreen. */
  AudioBoot core = audio_boot();
  audio_voices_stop(); /* an app's music must not outlive the app */

  /* With the GPU up, draw into cached FCRAM backbuffers and present each screen
   * with one blit; otherwise draw straight to VRAM. */
  if (gpu_init()) {
    /* Async, so the copy overlaps the next input poll; ui_idle() collects it
     * before anything draws. */
    g_screen_blit = gpu_texcopy_async;
    g_screen_wait = gpu_wait_idle;
    screen_use_backbuffer(1);
  }

  /* Calibrated once, here: it waits out up to two RTC seconds, and key repeat
   * and frame pacing read it every pass. */
  timer_ready();

  ui_init();

  int loaded = user_config_load(&g_cfg);
  if (!loaded || !g_cfg.setup_done) {
    setup_run(&g_cfg);
    user_config_save(&g_cfg);
  }

  g_accent_idx = g_cfg.accent;
  g_accent = aurora_accent_presets[g_cfg.accent];
  g_lang = g_cfg.language;
  if (g_cfg.touch_set && !touch_cal_set(&g_cfg.touch))
    g_cfg.touch_set = 0; /* unusable values: keep the default */

  /* Now that the accent is known. */
  ui_bg_build();

  scan_apps();
  build_home();

  int sel = 0;
  hm_draw_full(sel);

  /* A core from before AUDIO_PARK_VERSION cannot be swapped while it runs, so
   * this build's core needs a power-off to load. */
  if (core == AUDIO_BOOT_STALE) {
    if (fv_confirm(L(STR_CORE_OLD), L(STR_CORE_OLD_HINT), L(STR_POWER_OFF),
                   L(STR_LATER)))
      os_power_off();
    hm_draw_full(sel);
  }

  int clock_tick = 0, last_min = -1;

  while (1) {
    u32 kdown = get_keys_down();
    int prev = sel;
    int col = sel % HOME_COLS, row = sel / HOME_COLS;

    if ((kdown & BUTTON_DLEFT) && col > 0)
      sel--;
    if ((kdown & BUTTON_DRIGHT) && col < HOME_COLS - 1 && sel + 1 < HOME_COUNT)
      sel++;
    if ((kdown & BUTTON_DUP) && row > 0)
      sel -= HOME_COLS;
    if ((kdown & BUTTON_DDOWN) && row < HOME_ROWS - 1 &&
        sel + HOME_COLS < HOME_COUNT)
      sel += HOME_COLS;

    if (sel != prev)
      hm_update(prev, sel);

    if (kdown & BUTTON_A)
      home_activate(sel);

    if (kdown & BUTTON_START) {
      settings_open();
      hm_draw_full(sel);
    }

    /* the settings icon in the top bar, or an app tile */
    int tx, ty;
    if (touch_tap(&tx, &ty)) {
      if (ty < 40 && tx > BOT_SCREEN_WIDTH - 46) {
        settings_open();
        hm_draw_full(sel);
      } else {
        for (int i = 0; i < HOME_COUNT; i++) {
          int c = i % HOME_COLS, r = i / HOME_COLS;
          int x = GRID_X + c * SLOT_STEP, y = GRID_Y + r * SLOT_STEP;
          if (touch_in(tx, ty, x - 3, y - 3, SLOT_SIZE + 6, SLOT_SIZE + 6)) {
            if (i != sel) {
              hm_update(sel, i);
              sel = i;
            }
            home_activate(sel);
            break;
          }
        }
      }
    }

    /* The MCU is on a slow I2C bus: sample it every 96 passes and repaint only
     * when the minute changes. */
    if (++clock_tick >= 96) {
      clock_tick = 0;
      RtcTime now;
      if (rtc_read(&now) && now.min != last_min) {
        last_min = now.min;
        hm_status_bar();
        screen_present_top();
      }
    }

    ui_idle();
  }
}
