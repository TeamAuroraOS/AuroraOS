#include "aurora.h"
#include "audio.h"
#include "container.h"
#include "crash.h"
#include "font.h"
#include "i2c.h"
#include "icons.h"
#include "lang.h"
#include "user.h"

void delay(volatile u32 cycles) {
  while (cycles--)
    __asm__ volatile("nop"); // VS Code gives error: "identifier "__asm__" is undefined" - Ignore it
}

u32 get_keys(void) { return ~REG_HID_PAD & 0x3FF; }

static u32 prev_keys = 0;
u32 get_keys_down(void) {
  crash_poll_arm11(); /* surface an ARM11 audio-core fault on the crash screen */
  u32 cur = get_keys();
  u32 down = cur & ~prev_keys;
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
  I2C_writeReg(I2C_DEV_MCU, 0x22, 1 << 0); /* LCDs off first */
  __asm__ volatile("mcr p15, 0, %0, c7, c10, 4" ::"r"(0) : "memory");
  I2C_writeReg(I2C_DEV_MCU, 0x20, 1 << 0); /* system power off */
  while (1)
    __asm__ volatile("mcr p15, 0, r0, c7, c0, 4");
}

static Color g_accent = COLOR_AURORA;
static int g_accent_idx = 0;

typedef enum { ACT_NONE = 0, ACT_POWER, ACT_LAUNCH, ACT_MUSIC } HomeAction;

typedef struct {
  const char *name; /* NULL => empty placeholder slot */
  const char *dev;
  const unsigned char *icon; /* 32x32 icon bits, or NULL */
  Color tint;                /* big-icon background tint */
  HomeAction action;
  const char *path;          /* ACT_LAUNCH: container path on the SD card */
} HomeApp;

#define HOME_COLS  5
#define HOME_ROWS  3
#define HOME_COUNT (HOME_COLS * HOME_ROWS)

/* Filled at startup by scan_apps()+build_home() (was a static const array with
 * only Power Off). Slots hold the apps discovered under SD:\Aurora\Apps plus a
 * permanent Power Off tile. */
static HomeApp home_apps[HOME_COUNT];

/* App-scan storage. FatFs is built without long file names (FF_USE_LFN=0), so
 * entries are 8.3 and these short buffers are plenty. */
#define APPS_DIR "Aurora/Apps"
#define MAX_APPS HOME_COUNT
static char app_name[MAX_APPS][16]; /* display name (filename minus ".BIN") */
static char app_path[MAX_APPS][40]; /* "Aurora/Apps/NAME.BIN"                */
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

static void hm_wifi(int x, int y) {
  for (int b = 0; b < 3; b++) {
    int bh = 3 + b * 3;
    draw_filled_rect(VRAM_TOP_LA, x + b * 5, y + (9 - bh), 3, bh,
                     TOP_SCREEN_HEIGHT, COLOR_WHITE);
  }
}
static void hm_grid_icon(int x, int y) {
  for (int r = 0; r < 2; r++)
    for (int c = 0; c < 2; c++)
      draw_filled_rect(VRAM_TOP_LA, x + c * 5, y + r * 5, 3, 3, TOP_SCREEN_HEIGHT,
                       COLOR_WHITE);
}
static void hm_battery(int x, int y) {
  draw_filled_rect(VRAM_TOP_LA, x, y, 15, 9, TOP_SCREEN_HEIGHT, COLOR_WHITE);
  draw_filled_rect(VRAM_TOP_LA, x + 1, y + 1, 13, 7, TOP_SCREEN_HEIGHT,
                   COLOR_HM_BAR);
  draw_filled_rect(VRAM_TOP_LA, x + 2, y + 2, 8, 5, TOP_SCREEN_HEIGHT, g_accent);
  draw_filled_rect(VRAM_TOP_LA, x + 15, y + 3, 2, 3, TOP_SCREEN_HEIGHT,
                   COLOR_WHITE);
}

static void hm_top_static(void) {
  clear_screen(VRAM_TOP_LA, TOP_FB_SIZE, COLOR_HM_BG);

  /* faint faceted diamond texture */
  for (int o = -TOP_SCREEN_HEIGHT; o < TOP_SCREEN_WIDTH; o += 46) {
    for (int t = 0; t < TOP_SCREEN_HEIGHT; t++) {
      int a = o + t;
      int b = o + (TOP_SCREEN_HEIGHT - t);
      if (a >= 0 && a < TOP_SCREEN_WIDTH)
        draw_pixel(VRAM_TOP_LA, a, t, TOP_SCREEN_HEIGHT, COLOR_HM_FACET);
      if (b >= 0 && b < TOP_SCREEN_WIDTH)
        draw_pixel(VRAM_TOP_LA, b, t, TOP_SCREEN_HEIGHT, COLOR_HM_FACET);
    }
  }

  draw_filled_rect(VRAM_TOP_LA, 0, 0, TOP_SCREEN_WIDTH, 22, TOP_SCREEN_HEIGHT,
                   COLOR_HM_BAR);
  draw_string(VRAM_TOP_LA, 10, 7, TOP_SCREEN_HEIGHT, "12:08", COLOR_WHITE,
              COLOR_HM_BAR);
  draw_string(VRAM_TOP_LA, 60, 7, TOP_SCREEN_HEIGHT, "Tue 25 Aug",
              COLOR_HM_TEXT2, COLOR_HM_BAR);
  hm_wifi(322, 7);
  draw_string(VRAM_TOP_LA, 340, 7, TOP_SCREEN_HEIGHT, "|", COLOR_HM_TEXT2,
              COLOR_HM_BAR);
  hm_grid_icon(352, 7);
  hm_battery(371, 7);
  screen_present_top();
}

static void hm_top_item(int selected) {
  const HomeApp *app = &home_apps[selected];

  int box = 96, bx = (TOP_SCREEN_WIDTH - box) / 2, by = 34;
  draw_filled_round_rect(VRAM_TOP_LA, bx, by, box, box, 16, TOP_SCREEN_HEIGHT,
                         app->name ? app->tint : COLOR_HM_SLOT_EMPTY);
  if (app->icon)
    draw_icon_32(VRAM_TOP_LA, bx + (box - ICON_SIZE) / 2,
                 by + (box - ICON_SIZE) / 2, TOP_SCREEN_HEIGHT, app->icon,
                 COLOR_WHITE);

  int cy = 150, cw = TOP_SCREEN_WIDTH - 80, ch = 58;
  draw_filled_round_rect(VRAM_TOP_LA, 40, cy, cw, ch, 12, TOP_SCREEN_HEIGHT,
                         COLOR_HM_CARD);
  const char *name = app->name ? app->name : L(STR_EMPTY_SLOT);
  const char *dev = app->dev ? app->dev : "";
  draw_string(VRAM_TOP_LA,
              (TOP_SCREEN_WIDTH - (int)str_len(name) * FONT_WIDTH) / 2, cy + 16,
              TOP_SCREEN_HEIGHT, name, COLOR_WHITE, COLOR_HM_CARD);
  draw_string(VRAM_TOP_LA,
              (TOP_SCREEN_WIDTH - (int)str_len(dev) * FONT_WIDTH) / 2, cy + 34,
              TOP_SCREEN_HEIGHT, dev, COLOR_HM_TEXT2, COLOR_HM_CARD);
  screen_present_top();
}

#define SLOT_SIZE 46
#define SLOT_GAP  12
#define SLOT_STEP (SLOT_SIZE + SLOT_GAP)
#define GRID_W    (HOME_COLS * SLOT_SIZE + (HOME_COLS - 1) * SLOT_GAP)
#define GRID_X    ((BOT_SCREEN_WIDTH - GRID_W) / 2)
#define GRID_Y    52

static void hm_bottom_static(void) {
  clear_screen(VRAM_BOT_A, BOT_FB_SIZE, COLOR_HM_BG);

  draw_filled_round_rect(VRAM_BOT_A, 8, 8, BOT_SCREEN_WIDTH - 16, 30, 10,
                         BOT_SCREEN_HEIGHT, COLOR_HM_BAR);
  int sx = BOT_SCREEN_WIDTH - 62, sy = 16; /* search magnifier */
  draw_filled_round_rect(VRAM_BOT_A, sx, sy, 11, 11, 5, BOT_SCREEN_HEIGHT,
                         COLOR_HM_TEXT2);
  draw_filled_round_rect(VRAM_BOT_A, sx + 2, sy + 2, 7, 7, 3, BOT_SCREEN_HEIGHT,
                         COLOR_HM_BAR);
  draw_filled_rect(VRAM_BOT_A, sx + 10, sy + 10, 4, 2, BOT_SCREEN_HEIGHT,
                   COLOR_HM_TEXT2);
  int gx = BOT_SCREEN_WIDTH - 34, gy = 18; /* settings sliders */
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

  draw_filled_round_rect(VRAM_BOT_A, x - 3, y - 3, SLOT_SIZE + 6, SLOT_SIZE + 6,
                         12, BOT_SCREEN_HEIGHT,
                         selected ? g_accent : COLOR_HM_BG);
  draw_filled_round_rect(VRAM_BOT_A, x, y, SLOT_SIZE, SLOT_SIZE, 10,
                         BOT_SCREEN_HEIGHT,
                         app->name ? COLOR_HM_SLOT : COLOR_HM_SLOT_EMPTY);
  if (app->icon)
    draw_icon_32(VRAM_BOT_A, x + (SLOT_SIZE - ICON_SIZE) / 2,
                 y + (SLOT_SIZE - ICON_SIZE) / 2, BOT_SCREEN_HEIGHT, app->icon,
                 COLOR_WHITE);
}

static void hm_draw_full(int sel) {
  hm_top_static();
  hm_top_item(sel);
  hm_bottom_static();
  for (int i = 0; i < HOME_COUNT; i++)
    hm_slot(i, i == sel);
  screen_present_bottom();
}

/* Selection moved: repaint only the two affected slots and the top item. */
static void hm_update(int old_sel, int new_sel) {
  hm_slot(old_sel, 0);
  hm_slot(new_sel, 1);
  screen_present_bottom();
  hm_top_item(new_sel);
}

/* ============================== Settings ================================= */

/* Accent palette + names now live in os_setup.c, shared with the first-time
 * setup's Personalise screen so the saved accent index means the same thing
 * everywhere. Keep the short local names the Settings screens below use. */
#define accent_presets aurora_accent_presets
#define accent_names   aurora_accent_names
#define ACCENT_COUNT   AURORA_ACCENT_COUNT

static void settings_header(const unsigned char *icon, const char *title,
                            Color icon_color) {
  hm_top_static();
  int scale = 3, sz = ICON_SIZE * scale;
  draw_icon_scaled(VRAM_TOP_LA, (TOP_SCREEN_WIDTH - sz) / 2, 44,
                   TOP_SCREEN_HEIGHT, icon, icon_color, scale);
  draw_string(VRAM_TOP_LA,
              (TOP_SCREEN_WIDTH - (int)str_len(title) * FONT_WIDTH) / 2, 150,
              TOP_SCREEN_HEIGHT, title, COLOR_WHITE, COLOR_HM_BG);
  screen_present_top();
}

typedef enum {
  SET_WIFI = 0,
  SET_ACCENT,
  SET_BRIGHTNESS,
  SET_SOUND,
  SET_ABOUT,
  SET_CRASH,
  SET_COUNT
} SettingId;

#define ROW_X    12
#define ROW_W    (BOT_SCREEN_WIDTH - 24)
#define ROW_H    32
#define ROW_STEP (ROW_H + 6)
#define ROW_Y0   6

static void settings_row(int i, int sel, const unsigned char *icon,
                         const char *name, const char *value) {
  int y = ROW_Y0 + i * ROW_STEP;
  draw_filled_round_rect(VRAM_BOT_A, ROW_X - 3, y - 3, ROW_W + 6, ROW_H + 6, 10,
                         BOT_SCREEN_HEIGHT, (i == sel) ? g_accent : COLOR_HM_BG);
  draw_filled_round_rect(VRAM_BOT_A, ROW_X, y, ROW_W, ROW_H, 8,
                         BOT_SCREEN_HEIGHT, COLOR_HM_SLOT);
  if (icon)
    draw_icon_32(VRAM_BOT_A, ROW_X + 6, y + (ROW_H - ICON_SIZE) / 2,
                 BOT_SCREEN_HEIGHT, icon, COLOR_WHITE);
  else /* accent row: show a swatch of the current accent colour */
    draw_filled_round_rect(VRAM_BOT_A, ROW_X + 11, y + 9, 16, 16, 4,
                           BOT_SCREEN_HEIGHT, g_accent);
  draw_string(VRAM_BOT_A, ROW_X + 44, y + (ROW_H - FONT_HEIGHT) / 2,
              BOT_SCREEN_HEIGHT, name, COLOR_WHITE, COLOR_HM_SLOT);
  if (value)
    draw_string(VRAM_BOT_A, ROW_X + ROW_W - 16 - (int)str_len(value) * FONT_WIDTH,
                y + (ROW_H - FONT_HEIGHT) / 2, BOT_SCREEN_HEIGHT, value,
                COLOR_HM_TEXT2, COLOR_HM_SLOT);
  draw_string(VRAM_BOT_A, ROW_X + ROW_W - 12, y + (ROW_H - FONT_HEIGHT) / 2,
              BOT_SCREEN_HEIGHT, ">", COLOR_HM_TEXT2, COLOR_HM_SLOT);
}

static void settings_draw(int sel) {
  /* Title is shown on the top screen by settings_header(). */
  clear_screen(VRAM_BOT_A, BOT_FB_SIZE, COLOR_HM_BG);
  settings_row(SET_WIFI, sel, icon_wifi_bits, L(STR_WIFI), L(STR_OFF));
  settings_row(SET_ACCENT, sel, NULL, L(STR_ACCENT_COLOR),
               accent_names[g_accent_idx]);
  settings_row(SET_BRIGHTNESS, sel, icon_brightness_bits, L(STR_BRIGHTNESS),
               "3 / 5");
  settings_row(SET_SOUND, sel, icon_boot_bits, L(STR_SOUND_TEST),
               audio_alive() ? "Ready" : "---");
  settings_row(SET_ABOUT, sel, icon_settings_bits, L(STR_ABOUT), "v0.0.8");
  settings_row(SET_CRASH, sel, icon_power_bits, L(STR_DEBUG_CRASH), "");
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
  settings_header(icon_wifi_bits, "Wifi Configuration", COLOR_WHITE);
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
    delay(60000);
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
    if (i == g_accent_idx) /* mark the currently active accent */
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
    if (sel != prev)
      accent_draw(sel);
    if (k & BUTTON_A) {
      g_accent = accent_presets[sel];
      g_accent_idx = sel;
      accent_draw(sel); /* Refresh the active marker. */
    }
    if (k & BUTTON_B)
      return;
    delay(60000);
  }
}

/* ============================== Sound Test ============================== */
/* Drives the ARM11 audio core: shows whether it is alive + its status code, and
 * plays a test tone through CSND. This is the on-device bring-up harness. */

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

static void sound_test_row(int y, const char *label, const char *value,
                           Color vc) {
  char line[48];
  char *p = snd_cpy(line, label);
  snd_cpy(p, value);
  draw_string(VRAM_BOT_A, 12, y, BOT_SCREEN_HEIGHT, line, vc, COLOR_HM_BG);
}

static void sound_test_draw(u32 freq) {
  clear_screen(VRAM_BOT_A, BOT_FB_SIZE, COLOR_HM_BG);
  draw_string(VRAM_BOT_A, 12, 8, BOT_SCREEN_HEIGHT, L(STR_SOUND_TEST),
              COLOR_HM_TEXT2, COLOR_HM_BG);

  int alive = audio_alive();
  u32 ver = audio_version();
  char num[16], line[48];

  /* Core + version: green if current, orange if a stale core is resident. */
  char *p = snd_cpy(line, alive ? "core alive  v" : "core MISSING v");
  snd_u32(num, ver);
  p = snd_cpy(p, num);
  p = snd_cpy(p, " / ");
  snd_u32(num, AUDIO_CORE_VERSION);
  snd_cpy(p, num);
  Color vcol = (alive && ver == AUDIO_CORE_VERSION)
                   ? COLOR_AURORA
                   : (alive ? COLOR_ORANGE : COLOR_RED);
  draw_string(VRAM_BOT_A, 12, 28, BOT_SCREEN_HEIGHT, line, vcol, COLOR_HM_BG);

  snd_u32(num, audio_status());
  sound_test_row(44, "status:    ", num, COLOR_WHITE);

  snd_hex(num, audio_diag(0));
  sound_test_row(60, "codec rd:  ", num, COLOR_HM_TEXT2);
  snd_hex(num, audio_diag(6));
  sound_test_row(74, "codec wr:  ", num, COLOR_HM_TEXT2);
  snd_u32(num, audio_diag(1));
  sound_test_row(88, "spi t/o:   ", num, COLOR_HM_TEXT2);
  snd_hex(num, audio_diag(4));
  sound_test_row(102, "cfg11 spi: ", num, COLOR_HM_TEXT2);
  snd_hex(num, audio_diag(5));
  sound_test_row(116, "nspi cnt:  ", num, COLOR_HM_TEXT2);
  snd_hex(num, audio_diag(3));
  sound_test_row(130, "csnd ch0:  ", num, COLOR_HM_TEXT2);

  p = snd_cpy(line, "freq: ");
  snd_u32(num, freq);
  p = snd_cpy(p, num);
  snd_cpy(p, " Hz");
  draw_string(VRAM_BOT_A, 12, 148, BOT_SCREEN_HEIGHT, line, COLOR_WHITE,
              COLOR_HM_BG);

  draw_string(VRAM_BOT_A, 12, BOT_SCREEN_HEIGHT - 52, BOT_SCREEN_HEIGHT,
              "A: Play  START: Stop", COLOR_HM_TEXT2, COLOR_HM_BG);
  draw_string(VRAM_BOT_A, 12, BOT_SCREEN_HEIGHT - 36, BOT_SCREEN_HEIGHT,
              "Up/Down: frequency", COLOR_HM_TEXT2, COLOR_HM_BG);
  draw_string(VRAM_BOT_A, 12, BOT_SCREEN_HEIGHT - 20, BOT_SCREEN_HEIGHT,
              "B: Back", COLOR_HM_TEXT2, COLOR_HM_BG);
  screen_present_bottom();
}

static void sound_test_screen(void) {
  settings_header(icon_boot_bits, L(STR_SOUND_TEST), COLOR_WHITE);
  u32 freq = 440;
  sound_test_draw(freq);
  while (1) {
    u32 k = get_keys_down();
    int redraw = 0;
    if ((k & BUTTON_DUP) && freq < 4000) {
      freq += 55;
      redraw = 1;
    }
    if ((k & BUTTON_DDOWN) && freq > 110) {
      freq -= 55;
      redraw = 1;
    }
    if (k & BUTTON_A) {
      audio_play_tone(freq);
      redraw = 1; /* refresh status after playing */
    }
    if (k & BUTTON_START) {
      audio_stop();
      redraw = 1;
    }
    if (redraw)
      sound_test_draw(freq);
    if (k & BUTTON_B) {
      audio_stop();
      return;
    }
    delay(60000);
  }
}

static void settings_open(void) {
  settings_header(icon_settings_bits, L(STR_SETTINGS), COLOR_WHITE);
  int sel = 0;
  settings_draw(sel);
  while (1) {
    u32 k = get_keys_down();
    int prev = sel;
    if ((k & BUTTON_DUP) && sel > 0)
      sel--;
    if ((k & BUTTON_DDOWN) && sel < SET_COUNT - 1)
      sel++;
    if (sel != prev)
      settings_draw(sel);
    if (k & BUTTON_A) {
      if (sel == SET_WIFI)
        wifi_screen();
      else if (sel == SET_ACCENT)
        accent_screen();
      else if (sel == SET_SOUND)
        sound_test_screen();
      else if (sel == SET_CRASH)
        crash_force(); /* never returns: shows the crash screen */
      settings_header(icon_settings_bits, L(STR_SETTINGS), COLOR_WHITE);
      settings_draw(sel);
    }
    if (k & BUTTON_B)
      return;
    delay(60000);
  }
}

/* ============================== App scanning ============================ */

static char *hm_str_copy(char *dst, const char *src) {
  while ((*dst = *src)) {
    dst++;
    src++;
  }
  return dst;
}

/* True if `name` (an upper-case 8.3 FatFs name) ends in ".BIN". */
static int hm_has_bin_ext(const char *name) {
  int n = (int)str_len(name);
  if (n < 5)
    return 0;
  return name[n - 4] == '.' && name[n - 3] == 'B' && name[n - 2] == 'I' &&
         name[n - 1] == 'N';
}

/* Compare two 8.3 names (already upper-case). Returns <0 / 0 / >0. */
static int hm_name_cmp(const char *a, const char *b) {
  while (*a && *a == *b) {
    a++;
    b++;
  }
  return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

/* Read the embedded icon block from the app container at `path` into `dest`
 * (ICON_SIZE*ICON_ROW_BYTES bytes). Returns 1 if a valid "AURICON1" icon was
 * found, 0 otherwise (the caller falls back to a default icon). */
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
    /* The icon data follows the magic contiguously (payload offset 12). */
    if (f_read(&f, dest, AURORA_ICON_BYTES, &br) != FR_OK ||
        br != AURORA_ICON_BYTES)
      ok = 0;
  }
  f_close(&f);
  return ok;
}

/* Scan SD:\Aurora\Apps for *.BIN files into app_name/app_path, sorted
 * alphabetically, and read each app's embedded icon. Display name = filename
 * without the ".BIN" extension. Returns the count (0 on any error, missing
 * folder, or missing SD). */
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
    if (!hm_has_bin_ext(fno.fname))
      continue;
    int n = (int)str_len(fno.fname) - 4; /* strip ".BIN" for the display name */
    if (n > (int)sizeof(app_name[0]) - 1)
      n = (int)sizeof(app_name[0]) - 1;
    int i = 0;
    for (; i < n; i++)
      app_name[app_count][i] = fno.fname[i];
    app_name[app_count][i] = '\0';
    char *p = hm_str_copy(app_path[app_count], APPS_DIR "/");
    hm_str_copy(p, fno.fname);
    app_count++;
  }
  f_closedir(&dir);

  /* Alphabetical sort (small n -> insertion sort over both arrays). */
  for (int i = 1; i < app_count; i++) {
    char tn[16], tp[40];
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

  /* Read each app's icon (in sorted order so it lines up with app_name[i]). */
  for (int i = 0; i < app_count; i++)
    app_has_icon[i] = read_app_icon(app_path[i], app_icon[i]);

  f_mount(NULL, "", 0);
  return app_count;
}

/* Populate the home grid from the scan: sorted apps first, then a permanent
 * Power Off tile, then empty slots. Reuses the existing HomeApp rendering. */
static void build_home(void) {
  static const Color app_tint = {0x2C, 0x50, 0x74};
  for (int i = 0; i < HOME_COUNT; i++)
    home_apps[i] = (HomeApp){0};

  int slot = 0;
  for (int i = 0; i < app_count && slot < HOME_COUNT; i++, slot++) {
    home_apps[slot].name = app_name[i];
    home_apps[slot].dev = "SD App";
    /* Use the app's own embedded icon; fall back to a generic one. */
    home_apps[slot].icon = app_has_icon[i] ? app_icon[i] : icon_boot_bits;
    home_apps[slot].tint = app_tint;
    home_apps[slot].action = ACT_LAUNCH;
    home_apps[slot].path = app_path[i];
  }
  if (slot < HOME_COUNT) {
    static const Color music_tint = {0x2C, 0x5C, 0x40};
    home_apps[slot].name = L(STR_MUSIC);
    home_apps[slot].dev = "Aurora";
    home_apps[slot].icon = icon_music_bits;
    home_apps[slot].tint = music_tint;
    home_apps[slot].action = ACT_MUSIC;
    slot++;
  }
  if (slot < HOME_COUNT) {
    home_apps[slot].name = L(STR_POWER_OFF);
    home_apps[slot].dev = L(STR_SYSTEM);
    home_apps[slot].icon = icon_power_bits;
    home_apps[slot].tint = COLOR_DARK_RED;
    home_apps[slot].action = ACT_POWER;
  }
}

/* ============================== App launch ============================== */

static void launch_msg(const char *msg, Color color) {
  clear_screen(VRAM_BOT_A, BOT_FB_SIZE, COLOR_HM_BG);
  draw_string(VRAM_BOT_A, 12, 12, BOT_SCREEN_HEIGHT, "Launch app", COLOR_AURORA,
              COLOR_HM_BG);
  draw_string(VRAM_BOT_A, 12, 40, BOT_SCREEN_HEIGHT, msg, color, COLOR_HM_BG);
  screen_present_bottom();
}

static void launch_wait_back(void) {
  while (1) {
    if (get_keys_down() & BUTTON_B)
      return;
    delay(60000);
  }
}

/* Install the HOME-return path: snapshot the running OS image, record its size
 * and the "ready" magic in the descriptor, and relocate the return stub to its
 * fixed address. After this, an app that branches to AURORA_RETURN_STUB_ADDR is
 * brought back to a freshly restarted Home Menu. */
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

  os_cache_sync(); /* flush snapshot + descriptor + stub to memory */
}

/* Relocate the hand-off stub to scratch (clear of both the staged payload and
 * the load region) and jump into it. Never returns. */
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

/* Load and launch the AUR1/AOS1 app at `path`. On success this never returns:
 * the app replaces the running Home Menu (a deliberate one-way jump for v1 --
 * see the README). On any error it shows a message, waits for B, and returns. */
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

  /* Install the HOME-return path (snapshot + return stub) before the app
   * overwrites the OS, so pressing HOME can restore the Home Menu. */
  os_install_return();

  /* Hand off to the app. Does not return. */
  os_run_stub(AURORA_APP_STAGE_ADDR, hdr.arm9_load_addr, hdr.arm9_size,
              hdr.arm9_entry);
}

/* ============================== Audio Player ============================ */
/* Plays .aaf files (mono PCM, see audio/aaf_tool.py) from SD:\Aurora\Music,
 * which is created if missing. The ARM9 loads the PCM into AUDIO_PCM_ADDR and
 * the ARM11 core plays it through CSND. */

#define MUSIC_DIR  "Aurora/Music"
#define MAX_TRACKS 32
static char mus_name[MAX_TRACKS][16]; /* 8.3 file name (FF_USE_LFN = 0) */
static char mus_path[MAX_TRACKS][40]; /* "Aurora/Music/NAME.AAF"        */
static int  mus_count;

/* True if `name` (upper-case 8.3) ends in ".AAF". */
static int hm_has_aaf_ext(const char *name) {
  int n = (int)str_len(name);
  if (n < 5)
    return 0;
  return name[n - 4] == '.' && name[n - 3] == 'A' && name[n - 2] == 'A' &&
         name[n - 1] == 'F';
}

/* Ensure SD:\Aurora\Music exists (create if missing) and list its *.AAF files
 * into mus_name/mus_path, sorted. Returns the count. */
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
    if (!hm_has_aaf_ext(fno.fname))
      continue;
    int i = 0;
    for (; fno.fname[i] && i < (int)sizeof(mus_name[0]) - 1; i++)
      mus_name[mus_count][i] = fno.fname[i];
    mus_name[mus_count][i] = '\0';
    char *p = hm_str_copy(mus_path[mus_count], MUSIC_DIR "/");
    hm_str_copy(p, fno.fname);
    mus_count++;
  }
  f_closedir(&dir);

  for (int i = 1; i < mus_count; i++) { /* insertion sort */
    char tn[16], tp[40];
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
    total = AUDIO_PCM_MAX; /* truncate very long tracks */

  UINT rd = 0;
  f_read(&mf, (void *)AUDIO_PCM_ADDR, total, &rd);
  f_close(&mf);
  f_mount(NULL, "", 0);

  *rate_out = rate;
  *depth_out = depth;
  return (int)(rd / bpp);
}

static void music_draw_top(int sel, int playing, Color accent) {
  clear_screen(VRAM_TOP_LA, TOP_FB_SIZE, COLOR_HM_BG);
  draw_filled_rect(VRAM_TOP_LA, 0, 0, TOP_SCREEN_WIDTH, 22, TOP_SCREEN_HEIGHT,
                   COLOR_HM_BAR);
  const char *title = L(STR_MUSIC);
  draw_string(VRAM_TOP_LA, 10, 7, TOP_SCREEN_HEIGHT, title, COLOR_WHITE,
              COLOR_HM_BAR);

  int scale = 3, sz = ICON_SIZE * scale;
  draw_icon_scaled(VRAM_TOP_LA, (TOP_SCREEN_WIDTH - sz) / 2, 40,
                   TOP_SCREEN_HEIGHT, icon_music_bits, accent, scale);

  const char *np = (mus_count > 0) ? mus_name[sel] : "---";
  draw_string(VRAM_TOP_LA,
              (TOP_SCREEN_WIDTH - (int)str_len(np) * FONT_WIDTH) / 2, 150,
              TOP_SCREEN_HEIGHT, np, COLOR_WHITE, COLOR_HM_BG);
  const char *st = playing ? L(STR_PLAYING) : L(STR_STOPPED);
  draw_string(VRAM_TOP_LA,
              (TOP_SCREEN_WIDTH - (int)str_len(st) * FONT_WIDTH) / 2, 172,
              TOP_SCREEN_HEIGHT, st, playing ? accent : COLOR_HM_TEXT2,
              COLOR_HM_BG);
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
      draw_string(VRAM_BOT_A, 22, y + (MUS_ROW_H - FONT_HEIGHT) / 2,
                  BOT_SCREEN_HEIGHT, mus_name[i], COLOR_WHITE, COLOR_HM_SLOT);
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
    if (sel != prev) {
      music_draw_bottom(sel);
      music_draw_top(sel, playing, accent);
    }

    if ((k & BUTTON_A) && mus_count > 0) {
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
    delay(60000);
  }
}

void os_main(void) {
  /* Install the custom crash handler first, so a fault anywhere below lands on
   * the Aurora crash screen instead of a silent hang. */
  crash_init();

  /* Load SD:\Aurora\USER.dat. If it is missing, not an "ADAT" file, or setup
   * never finished, run the first-time setup wizard and save the result so the
   * user only sees setup once. */
  static UserConfig cfg;
  int loaded = user_config_load(&cfg);
  if (!loaded || !cfg.setup_done) {
    setup_run(&cfg);
    user_config_save(&cfg);
  }

  /* Apply saved preferences before drawing the Home Menu. */
  g_accent_idx = cfg.accent;
  g_accent = aurora_accent_presets[cfg.accent];
  g_lang = cfg.language;

  /* Bring up the ARM11 audio core (idempotent across HOME returns). */
  audio_boot();

  scan_apps();
  build_home();

  int sel = 0;
  hm_draw_full(sel);

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

    if (kdown & BUTTON_A) {
      if (home_apps[sel].action == ACT_POWER) {
        os_power_off();
      } else if (home_apps[sel].action == ACT_LAUNCH) {
        os_launch_app(home_apps[sel].path);
        hm_draw_full(sel); /* only reached if the launch failed and returned */
      } else if (home_apps[sel].action == ACT_MUSIC) {
        music_player_screen();
        hm_draw_full(sel);
      }
    }

    if (kdown & BUTTON_START) {
      settings_open();
      hm_draw_full(sel);
    }

    delay(60000);
  }
}
