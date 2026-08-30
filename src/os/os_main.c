#include "aurora.h"
#include "container.h"
#include "font.h"
#include "i2c.h"
#include "icons.h"

void delay(volatile u32 cycles) {
  while (cycles--)
    __asm__ volatile("nop"); // VS Code gives error: "identifier "__asm__" is undefined" - Ignore it
}

u32 get_keys(void) { return ~REG_HID_PAD & 0x3FF; }

static u32 prev_keys = 0;
u32 get_keys_down(void) {
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

typedef enum { ACT_NONE = 0, ACT_POWER, ACT_LAUNCH } HomeAction;

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
  const char *name = app->name ? app->name : "Empty Slot";
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

static const Color accent_presets[] = {
    {0x64, 0xE8, 0xC8}, {0x4A, 0x90, 0xE2}, {0x3C, 0xCB, 0x5A}, {0xE2, 0x4A, 0x4A},
    {0xA0, 0x5C, 0xE2}, {0xE2, 0x91, 0x3C}, {0xE2, 0x5C, 0x9A}, {0x3C, 0xD6, 0xE2},
};
static const char *accent_names[] = {"Aurora", "Blue",   "Green", "Red",
                                     "Purple", "Orange", "Pink",  "Cyan"};
#define ACCENT_COUNT ((int)(sizeof(accent_presets) / sizeof(accent_presets[0])))

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
  SET_ABOUT,
  SET_COUNT
} SettingId;

#define ROW_X    12
#define ROW_W    (BOT_SCREEN_WIDTH - 24)
#define ROW_H    34
#define ROW_STEP (ROW_H + 8)
#define ROW_Y0   38

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
  clear_screen(VRAM_BOT_A, BOT_FB_SIZE, COLOR_HM_BG);
  draw_string(VRAM_BOT_A, ROW_X, 14, BOT_SCREEN_HEIGHT, "Settings",
              COLOR_HM_TEXT2, COLOR_HM_BG);
  settings_row(SET_WIFI, sel, icon_wifi_bits, "Wi-Fi", "Off");
  settings_row(SET_ACCENT, sel, NULL, "Accent Color", accent_names[g_accent_idx]);
  settings_row(SET_BRIGHTNESS, sel, icon_brightness_bits, "Brightness", "3 / 5");
  settings_row(SET_ABOUT, sel, icon_settings_bits, "About", "v0.0.7");
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

#define SW_SIZE 50
#define SW_GAP  14
#define SW_COLS 4
#define SW_W    (SW_COLS * SW_SIZE + (SW_COLS - 1) * SW_GAP)
#define SW_X    ((BOT_SCREEN_WIDTH - SW_W) / 2)
#define SW_Y    56

static void accent_draw(int sel) {
  hm_top_static();
  draw_filled_round_rect(VRAM_TOP_LA, (TOP_SCREEN_WIDTH - 96) / 2, 40, 96, 96, 16,
                         TOP_SCREEN_HEIGHT, accent_presets[sel]);
  const char *t = "Accent Color";
  draw_string(VRAM_TOP_LA, (TOP_SCREEN_WIDTH - (int)str_len(t) * FONT_WIDTH) / 2,
              150, TOP_SCREEN_HEIGHT, t, COLOR_WHITE, COLOR_HM_BG);
  draw_string(VRAM_TOP_LA,
              (TOP_SCREEN_WIDTH - (int)str_len(accent_names[sel]) * FONT_WIDTH) / 2,
              170, TOP_SCREEN_HEIGHT, accent_names[sel], COLOR_HM_TEXT2,
              COLOR_HM_BG);
  screen_present_top();

  clear_screen(VRAM_BOT_A, BOT_FB_SIZE, COLOR_HM_BG);
  draw_string(VRAM_BOT_A, 12, 14, BOT_SCREEN_HEIGHT, "Pick an accent colour",
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
              "A: Apply   B: Back", COLOR_HM_TEXT2, COLOR_HM_BG);
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

static void settings_open(void) {
  settings_header(icon_settings_bits, "Settings", COLOR_WHITE);
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
      settings_header(icon_settings_bits, "Settings", COLOR_WHITE);
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
    home_apps[slot].name = "Power Off";
    home_apps[slot].dev = "System";
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

  launch_msg("Loading...", COLOR_WHITE);

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

void os_main(void) {
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
      }
    }

    if (kdown & BUTTON_START) {
      settings_open();
      hm_draw_full(sel);
    }

    delay(60000);
  }
}
