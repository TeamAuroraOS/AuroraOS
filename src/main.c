#include "assets.h"
#include "aurora.h"
#include "aurora_logo.h"
#include "ff.h"
#include "font.h"
#include "i2c.h"
#include "icons.h"
#include "loader.h"
#include "sdmmc.h"
#include "user.h"

void delay(volatile u32 cycles) {
  while (cycles--) {
    __asm__ volatile("nop");
  }
}

static inline void arm9_drain_write_buffer(void) {
  __asm__ volatile("mcr p15, 0, %0, c7, c10, 4" ::"r"(0) : "memory");
}

void power_off(void) {
  I2C_init();

  /* Power the LCDs off before cutting system power. */
  I2C_writeReg(I2C_DEV_MCU, 0x22, 1 << 0);
  arm9_drain_write_buffer(); /* Match the ARM DSB sequence used by GodMode9. */

  /* Cut power to the console; bit 2 would trigger a reboot. */
  I2C_writeReg(I2C_DEV_MCU, 0x20, 1 << 0);

  while (1) {
    __asm__ volatile("mcr p15, 0, r0, c7, c0, 4");
  }
}

static u32 str_len(const char *s) {
  u32 len = 0;
  while (*s++)
    len++;
  return len;
}

static u32 prev_keys = 0;

u32 get_keys(void) {
  return ~REG_HID_PAD & 0x3FF;
}

u32 get_keys_down(void) {
  u32 cur = get_keys();
  u32 down = cur & ~prev_keys;
  prev_keys = cur;
  return down;
}

#define BRIGHTNESS_LEVELS 5
static int current_brightness = 3;

void set_brightness(u32 level) {
  if (level >= BRIGHTNESS_LEVELS)
    level = BRIGHTNESS_LEVELS - 1;
  current_brightness = (int)level;
}

int sd_is_inserted(void) {
  return !(sdmmc_read16(REG_SDSTATUS0) & TMIO_STAT0_SIGSTATE);
}

int sd_init(void) {
  return sd_is_inserted();
}

int check_files_directory(void) {
  return sd_init();
}

/* ---- Home-Menu-style typography & icon helpers (no OS dependencies) ---- */

static Font bm_small, bm_font, bm_bold, bm_title;
static int bm_assets_ok;

static void bm_assets_init(void) {
  bm_assets_ok = assets_load();
  if (!bm_assets_ok)
    return;
  if (!asset_font(ASSET_FONT_SMALL, &bm_small) ||
      !asset_font(ASSET_FONT_UI, &bm_font) ||
      !asset_font(ASSET_FONT_BOLD, &bm_bold) ||
      !asset_font(ASSET_FONT_TITLE, &bm_title))
    bm_assets_ok = 0;
}

static int bm_have(const Font *f) { return bm_assets_ok && f && f->head != 0; }

static int bm_tw(const Font *f, const char *s) {
  return bm_have(f) ? text_width(f, s) : (int)str_len(s) * FONT_WIDTH;
}

static int bm_th(const Font *f) {
  return bm_have(f) ? text_line_height(f) : FONT_HEIGHT;
}

static void bm_text(volatile u8 *fb, int x, int y, int sh, const char *s,
                    Color fg, Color bg, const Font *f) {
  if (bm_have(f))
    draw_text(fb, x, y + text_ascent(f), sh, s, fg, f);
  else
    draw_string(fb, x, y, sh, s, fg, bg);
}

static void bm_text_mid(volatile u8 *fb, int cx, int y, int sh, const char *s,
                        Color fg, Color bg, const Font *f) {
  bm_text(fb, cx - bm_tw(f, s) / 2, y, sh, s, fg, bg, f);
}

static void bm_icon(volatile u8 *fb, int bx, int by, int box, int sh,
                    u32 asset_id, const unsigned char *bits, Color tint) {
  if (bm_assets_ok && draw_asset_boxed(fb, bx, by, box, box, sh, asset_id, tint))
    return;
  if (bits) {
    int s = box / ICON_SIZE;
    if (s < 1)
      s = 1;
    draw_icon_scaled(fb, bx + (box - ICON_SIZE * s) / 2,
                     by + (box - ICON_SIZE * s) / 2, sh, bits, tint, s);
  }
}

static void bm_bg(volatile u8 *fb, int w, int h) {
  draw_vgradient(fb, 0, 0, w, h, h, COLOR_HM_BG_TOP, COLOR_HM_BG_BOT);
}

/* ---- Home-Menu-style Bootloader Top & Bottom Screens ---- */

typedef struct {
  const char *label;
  const char *title;
  const char *desc;
  u32 asset_sm;
  u32 asset_lg;
  const unsigned char *bits;
  Color accent;
} BootOption;

static const BootOption boot_opts[3] = {
    {
        "Start",
        "AuroraOS",
        "Launch AuroraOS on your Nintendo 3DS",
        ASSET_ICON_FILE_AURORA_32,
        ASSET_ICON_FILE_AURORA_64,
        icon_boot_bits,
        {0x64, 0xE8, 0xC8},
    },
    {
        "Power",
        "Power Off",
        "Turn off your Nintendo 3DS safely",
        ASSET_ICON_POWER_32,
        ASSET_ICON_POWER_64,
        icon_power_bits,
        {0xFF, 0x5A, 0x52},
    },
    {
        "Settings",
        "Settings",
        "Adjust screen brightness",
        ASSET_ICON_SETTINGS_32,
        ASSET_ICON_SETTINGS_64,
        icon_settings_bits,
        {0x7A, 0xB8, 0xFF},
    },
};

static void draw_top_preview(int selection) {
  volatile u8 *fb = VRAM_TOP_LA;
  const int W = TOP_SCREEN_WIDTH, H = TOP_SCREEN_HEIGHT;
  if (selection < 0 || selection > 2)
    selection = 0;
  const BootOption *opt = &boot_opts[selection];

  bm_bg(fb, W, H);

  /* Top status bar matching Home Menu */
  draw_filled_rect(fb, 0, 0, W, 22, H, COLOR_HM_BAR);
  bm_text(fb, 12, (22 - bm_th(&bm_small)) / 2, H, "AuroraOS",
          COLOR_AURORA, COLOR_HM_BAR, &bm_small);
  int vw = bm_tw(&bm_small, AURORA_VERSION);
  bm_text(fb, W - 12 - vw, (22 - bm_th(&bm_small)) / 2, H, AURORA_VERSION,
          COLOR_HM_TEXT2, COLOR_HM_BAR, &bm_small);

  /* Large rounded icon box (same style as Home Menu top preview) */
  int box = 76;
  int bx = (W - box) / 2, by = 42;
  draw_gradient_round_rect(fb, bx, by, box, box, 16, H, COLOR_HM_SLOT_TOP,
                           COLOR_HM_SLOT_BOT);
  draw_round_ring(fb, bx - 2, by - 2, box + 4, box + 4, 18, 2, H, opt->accent);
  bm_icon(fb, bx + 6, by + 6, box - 12, H, opt->asset_lg, opt->bits,
          COLOR_WHITE);

  /* Rounded info card below icon (matching Home Menu PV_CARD) */
  int cw = 264, ch = 64;
  int cx = (W - cw) / 2, cy = 132;
  draw_filled_round_rect(fb, cx, cy, cw, ch, 12, H, COLOR_HM_CARD);
  bm_text_mid(fb, W / 2, cy + 12, H, opt->title, COLOR_WHITE, COLOR_HM_CARD,
              &bm_title);
  bm_text_mid(fb, W / 2, cy + 40, H, opt->desc, COLOR_HM_TEXT2, COLOR_HM_CARD,
              &bm_small);

  /* Subtle bottom hint */
  bm_text_mid(fb, W / 2, H - 22, H, "D-Pad: Choose   A: Select",
              COLOR_HM_TEXT2, COLOR_HM_BG_BOT, &bm_small);
  screen_present_top();
}

#define TILE_SIZE    74
#define TILE_LIFT    4
#define TILE_GAP     18
#define TILE_Y       74
#define TILE_TOTAL_W (TILE_SIZE * 3 + TILE_GAP * 2)
#define TILE_X_START ((BOT_SCREEN_WIDTH - TILE_TOTAL_W) / 2)
#define TILE_STEP    (TILE_SIZE + TILE_GAP)

static void draw_home_screen(int selection);

static void draw_tile(int idx, int selected) {
  volatile u8 *fb = VRAM_BOT_A;
  const int H = BOT_SCREEN_HEIGHT;
  const BootOption *opt = &boot_opts[idx];
  int lift = selected ? TILE_LIFT : 0;
  int size = TILE_SIZE + lift * 2;
  int x = TILE_X_START + idx * TILE_STEP - lift;
  int y = TILE_Y - lift;
  int r = 14 + lift / 2;

  draw_gradient_round_rect(fb, x, y, size, size, r, H, COLOR_HM_SLOT_TOP,
                           COLOR_HM_SLOT_BOT);
  if (selected) {
    draw_round_ring(fb, x - 3, y - 3, size + 6, size + 6, r + 3, 3, H,
                    opt->accent);
  }

  int icon_box = 36;
  bm_icon(fb, x + (size - icon_box) / 2, y + (size - icon_box) / 2 - 8,
          icon_box, H, opt->asset_sm, opt->bits,
          selected ? opt->accent : COLOR_WHITE);

  bm_text_mid(fb, x + size / 2, y + size - bm_th(&bm_small) - 8, H, opt->label,
              selected ? COLOR_WHITE : COLOR_HM_TEXT2, COLOR_HM_SLOT_BOT,
              &bm_small);
}

void ui_draw_home_screen(void) { draw_home_screen(0); }

static void draw_home_screen(int selection) {
  volatile u8 *fb = VRAM_BOT_A;
  const int W = BOT_SCREEN_WIDTH, H = BOT_SCREEN_HEIGHT;

  draw_top_preview(selection);
  bm_bg(fb, W, H);

  /* Home Menu-style rounded top bar */
  draw_filled_round_rect(fb, 8, 8, W - 16, 30, 10, H, COLOR_HM_BAR);
  bm_text_mid(fb, W / 2, 8 + (30 - bm_th(&bm_bold)) / 2, H, "AuroraOS",
              COLOR_AURORA, COLOR_HM_BAR, &bm_bold);

  for (int i = 0; i < 3; i++)
    draw_tile(i, selection == i);

  /* Page indicator dots (matching Home Menu bottom style) */
  for (int i = 0; i < 3; i++) {
    int dw = (i == selection) ? 14 : 6;
    int dx = W / 2 - 22 + i * 16 - (i == selection ? 4 : 0);
    draw_filled_round_rect(fb, dx, 174, dw, 6, 3, H,
                           i == selection ? COLOR_WHITE : COLOR_HM_TEXT2);
  }

  /* Rounded bottom hint pill */
  draw_filled_round_rect(fb, 12, H - 36, W - 24, 26, 9, H, COLOR_HM_BAR);
  bm_text_mid(fb, W / 2, H - 36 + (26 - bm_th(&bm_small)) / 2, H,
              "D-Pad: Choose   A: Select", COLOR_HM_TEXT2,
              COLOR_HM_BAR, &bm_small);
  screen_present_bottom();
}

int ui_power_off_confirm(void) {
  volatile u8 *fb = VRAM_BOT_A;
  const int W = BOT_SCREEN_WIDTH, H = BOT_SCREEN_HEIGHT;

  bm_bg(fb, W, H);
  draw_filled_round_rect(fb, 36, 56, W - 72, 128, 14, H, COLOR_HM_CARD);
  bm_icon(fb, (W - 40) / 2, 74, 40, H, ASSET_ICON_POWER_32, icon_power_bits,
          (Color){0xFF, 0x5A, 0x52});
  bm_text_mid(fb, W / 2, 132, H, "Powering off...", COLOR_WHITE, COLOR_HM_CARD,
              &bm_bold);
  screen_present_bottom();

  return 0;
}

static void draw_settings_screen(void) {
  volatile u8 *fb = VRAM_BOT_A;
  const int W = BOT_SCREEN_WIDTH, H = BOT_SCREEN_HEIGHT;

  bm_bg(fb, W, H);

  /* Rounded top bar */
  draw_filled_round_rect(fb, 8, 8, W - 16, 30, 10, H, COLOR_HM_BAR);
  bm_text_mid(fb, W / 2, 8 + (30 - bm_th(&bm_bold)) / 2, H, "Settings",
              COLOR_WHITE, COLOR_HM_BAR, &bm_bold);

  /* Brightness card */
  draw_gradient_round_rect(fb, 14, 54, W - 28, 114, 12, H, COLOR_HM_SLOT_TOP,
                           COLOR_HM_SLOT_BOT);
  bm_icon(fb, 28, 70, 32, H, ASSET_ICON_DISPLAY_32, icon_brightness_bits,
          COLOR_AURORA);
  bm_text(fb, 70, 74, H, "Screen Brightness", COLOR_WHITE, COLOR_HM_SLOT_TOP,
          &bm_bold);

  int bar_x = 28, bar_y = 114, bar_w = W - 56, bar_h = 14;
  int gap = 6;
  int seg_w = (bar_w - gap * (BRIGHTNESS_LEVELS - 1)) / BRIGHTNESS_LEVELS;
  for (int i = 0; i < BRIGHTNESS_LEVELS; i++) {
    int sx = bar_x + i * (seg_w + gap);
    Color c = (i <= current_brightness) ? COLOR_AURORA : COLOR_HM_BAR;
    draw_filled_round_rect(fb, sx, bar_y, seg_w, bar_h, 5, H, c);
  }

  char level_str[] = "Level X / 5";
  level_str[6] = (char)('1' + current_brightness);
  bm_text(fb, 70, 94, H, level_str, COLOR_HM_TEXT2, COLOR_HM_SLOT_TOP,
          &bm_small);

  /* Bottom hint pill */
  draw_filled_round_rect(fb, 12, H - 36, W - 24, 26, 9, H, COLOR_HM_BAR);
  bm_text_mid(fb, W / 2, H - 36 + (26 - bm_th(&bm_small)) / 2, H,
              "Left / Right: Adjust   B: Back", COLOR_HM_TEXT2, COLOR_HM_BAR,
              &bm_small);
  screen_present_bottom();
}

void ui_settings_menu(void) {
  draw_settings_screen();

  while (1) {
    u32 kdown = get_keys_down();

    if (kdown & BUTTON_DRIGHT) {
      if (current_brightness < BRIGHTNESS_LEVELS - 1) {
        set_brightness((u32)(current_brightness + 1));
        draw_settings_screen();
      }
    }

    if (kdown & BUTTON_DLEFT) {
      if (current_brightness > 0) {
        set_brightness((u32)(current_brightness - 1));
        draw_settings_screen();
      }
    }

    if (kdown & BUTTON_B) {
      return;
    }

    delay(60000);
  }
}

static char hex_nibble(u8 v) {
  return (char)(v < 10 ? '0' + v : 'A' + (v - 10));
}

static char *str_cpy_ret(char *dst, const char *src) {
  while ((*dst = *src)) {
    dst++;
    src++;
  }
  return dst;
}

static void int_to_str(int v, char *out) {
  char tmp[12];
  int i = 0;
  unsigned uv = (v < 0) ? (unsigned)(-v) : (unsigned)v;
  if (uv == 0)
    tmp[i++] = '0';
  while (uv) {
    tmp[i++] = (char)('0' + (uv % 10));
    uv /= 10;
  }
  int p = 0;
  if (v < 0)
    out[p++] = '-';
  while (i)
    out[p++] = tmp[--i];
  out[p] = '\0';
}

static void build_hex_line(const u8 *data, int n, char *out) {
  int p = 0;
  for (int i = 0; i < n; i++) {
    out[p++] = hex_nibble((u8)(data[i] >> 4));
    out[p++] = hex_nibble((u8)(data[i] & 0xF));
    out[p++] = ' ';
  }
  out[p] = '\0';
}

static u8 sd_test_buf[512];

void ui_sd_test(void) {
  volatile u8 *fb = VRAM_BOT_A;
  const int W = BOT_SCREEN_WIDTH, H = BOT_SCREEN_HEIGHT;

  bm_bg(fb, W, H);
  draw_filled_round_rect(fb, 8, 8, W - 16, 30, 10, H, COLOR_HM_BAR);
  bm_text(fb, 20, 8 + (30 - bm_th(&bm_bold)) / 2, H, "SD Card Test",
          COLOR_AURORA, COLOR_HM_BAR, &bm_bold);
  draw_filled_round_rect(fb, 12, 46, W - 24, H - 90, 12, H, COLOR_HM_CARD);
  screen_present_bottom();

  char line[48];
  int y = 56;

  int init_res = sdmmc_sdcard_init();
  char *p = str_cpy_ret(line, "Init: ");
  int_to_str(init_res, p);
  bm_text(fb, 22, y, H, line, init_res == 0 ? COLOR_AURORA : COLOR_RED,
          COLOR_HM_CARD, &bm_small);
  y += 18;

  if (init_res != 0) {
    bm_text(fb, 22, y, H, "SD init failed", COLOR_RED, COLOR_HM_CARD, &bm_small);
  } else {
    int read_res = sdmmc_sdcard_readsectors(0, 1, sd_test_buf);
    p = str_cpy_ret(line, "Read: ");
    int_to_str(read_res, p);
    bm_text(fb, 22, y, H, line, read_res == 0 ? COLOR_AURORA : COLOR_RED,
            COLOR_HM_CARD, &bm_small);
    y += 18;

    bm_text(fb, 22, y, H, "Sector 0, bytes 0-15:", COLOR_HM_TEXT2,
            COLOR_HM_CARD, &bm_small);
    y += 16;
    build_hex_line(&sd_test_buf[0], 8, line);
    draw_string(fb, 22, y, H, line, COLOR_WHITE, COLOR_HM_CARD);
    y += 12;
    build_hex_line(&sd_test_buf[8], 8, line);
    draw_string(fb, 22, y, H, line, COLOR_WHITE, COLOR_HM_CARD);
    y += 18;

    p = str_cpy_ret(line, "Signature: ");
    *p++ = hex_nibble((u8)(sd_test_buf[510] >> 4));
    *p++ = hex_nibble((u8)(sd_test_buf[510] & 0xF));
    *p++ = hex_nibble((u8)(sd_test_buf[511] >> 4));
    *p++ = hex_nibble((u8)(sd_test_buf[511] & 0xF));
    *p = '\0';
    bm_text(fb, 22, y, H, line, COLOR_WHITE, COLOR_HM_CARD, &bm_small);
    y += 18;

    int pass = (sd_test_buf[510] == 0x55 && sd_test_buf[511] == 0xAA);
    bm_text(fb, 22, y, H,
            pass ? "PASS: valid boot sector" : "No 0x55AA signature",
            pass ? COLOR_AURORA : COLOR_YELLOW, COLOR_HM_CARD, &bm_small);
  }

  draw_filled_round_rect(fb, 12, H - 36, W - 24, 26, 9, H, COLOR_HM_BAR);
  bm_text_mid(fb, W / 2, H - 36 + (26 - bm_th(&bm_small)) / 2, H, "B: Back",
              COLOR_HM_TEXT2, COLOR_HM_BAR, &bm_small);
  screen_present_bottom();

  while (1) {
    u32 kdown = get_keys_down();
    if (kdown & BUTTON_B)
      return;
    delay(60000);
  }
}

static FATFS fs_volume;
static FIL fs_file;

void ui_fs_test(void) {
  volatile u8 *fb = VRAM_BOT_A;
  const int W = BOT_SCREEN_WIDTH, H = BOT_SCREEN_HEIGHT;

  bm_bg(fb, W, H);
  draw_filled_round_rect(fb, 8, 8, W - 16, 30, 10, H, COLOR_HM_BAR);
  bm_text(fb, 20, 8 + (30 - bm_th(&bm_bold)) / 2, H, "FatFs Test",
          COLOR_AURORA, COLOR_HM_BAR, &bm_bold);
  draw_filled_round_rect(fb, 12, 46, W - 24, H - 90, 12, H, COLOR_HM_CARD);
  screen_present_bottom();

  char line[48];
  int y = 56;

  FRESULT fr = f_mount(&fs_volume, "", 1);
  char *p = str_cpy_ret(line, "Mount: ");
  int_to_str((int)fr, p);
  bm_text(fb, 22, y, H, line, fr == FR_OK ? COLOR_AURORA : COLOR_RED,
          COLOR_HM_CARD, &bm_small);
  y += 18;

  if (fr == FR_OK) {
    fr = f_open(&fs_file, "test.txt", FA_READ);
    p = str_cpy_ret(line, "Open test.txt: ");
    int_to_str((int)fr, p);
    bm_text(fb, 22, y, H, line, fr == FR_OK ? COLOR_AURORA : COLOR_RED,
            COLOR_HM_CARD, &bm_small);
    y += 18;

    if (fr == FR_OK) {
      char data[33];
      UINT br = 0;
      fr = f_read(&fs_file, data, sizeof(data) - 1, &br);
      f_close(&fs_file);

      p = str_cpy_ret(line, "Read: ");
      p = str_cpy_ret(p, (fr == FR_OK) ? "OK  bytes=" : "ERR bytes=");
      int_to_str((int)br, p);
      bm_text(fb, 22, y, H, line, fr == FR_OK ? COLOR_AURORA : COLOR_RED,
              COLOR_HM_CARD, &bm_small);
      y += 18;

      for (UINT i = 0; i < br; i++) {
        if (data[i] < 0x20 || data[i] > 0x7E)
          data[i] = '.';
      }
      data[br] = '\0';
      bm_text(fb, 22, y, H, "Contents:", COLOR_HM_TEXT2, COLOR_HM_CARD,
              &bm_small);
      y += 16;
      draw_string(fb, 22, y, H, data, COLOR_WHITE, COLOR_HM_CARD);
      y += 18;

      if (fr == FR_OK && br > 0)
        bm_text(fb, 22, y, H, "PASS: file read OK", COLOR_AURORA, COLOR_HM_CARD,
                &bm_small);
    } else {
      bm_text(fb, 22, y, H, "Put test.txt on SD root", COLOR_YELLOW,
              COLOR_HM_CARD, &bm_small);
    }
  }

  f_mount(NULL, "", 0);

  draw_filled_round_rect(fb, 12, H - 36, W - 24, 26, 9, H, COLOR_HM_BAR);
  bm_text_mid(fb, W / 2, H - 36 + (26 - bm_th(&bm_small)) / 2, H, "B: Back",
              COLOR_HM_TEXT2, COLOR_HM_BAR, &bm_small);
  screen_present_bottom();

  while (1) {
    u32 kdown = get_keys_down();
    if (kdown & BUTTON_B)
      return;
    delay(60000);
  }
}

static int read_autoboot_flag(void) {
  static FATFS ab_fs;
  static FIL ab_f;
  u8 buf[USER_DAT_SIZE];
  UINT br = 0;
  int on = 0;
  for (int attempt = 0; attempt < 4; attempt++) {
    if (f_mount(&ab_fs, "", 1) == FR_OK) {
      if (f_open(&ab_f, USER_DAT_PATH, FA_READ) == FR_OK) {
        if (f_read(&ab_f, buf, sizeof(buf), &br) == FR_OK &&
            br > USER_DAT_AUTOBOOT &&
            buf[0] == 'A' && buf[1] == 'D' && buf[2] == 'A' && buf[3] == 'T') {
          on = (buf[USER_DAT_AUTOBOOT] & 1u) != 0;
        }
        f_close(&ab_f);
      }
      f_mount(NULL, "", 0);
      return on;
    }
    f_mount(NULL, "", 0);
    delay(60000);
  }
  return 0;
}

static void draw_autoboot_logo(void) {
  clear_screen(VRAM_TOP_LA, TOP_FB_SIZE, COLOR_BLACK);
  int lx = (TOP_SCREEN_WIDTH - AURORA_LOGO_WIDTH) / 2;
  int ly = (TOP_SCREEN_HEIGHT - AURORA_LOGO_HEIGHT) / 2;
  draw_aurora_logo(VRAM_TOP_LA, lx, ly, TOP_SCREEN_HEIGHT, COLOR_WHITE);
  screen_present_top();
  clear_screen(VRAM_BOT_A, BOT_FB_SIZE, COLOR_BLACK);
  screen_present_bottom();
}

int main(void) {
  screen_init();
  set_brightness((u32)current_brightness);

  /* Autoboot: show only the Aurora logo and launch AURORAOS.BIN directly.
   * Holding A across a short window (~350 ms) reroutes to the bootloader menu,
   * while a brief A-tap from a chainloader (e.g. Luma3DS) still autoboot-loads. */
  if (read_autoboot_flag()) {
    draw_autoboot_logo();
    int hold_a = 1;
    for (int i = 0; i < 40; i++) {
      delay(60000);
      if (!(get_keys() & BUTTON_A)) {
        hold_a = 0;
        break;
      }
    }
    if (!hold_a) {
      boot_aurora_ex(1);
      /* Only reaches here if autoboot hit an error and user pressed B. */
    }
  }

  /* Seed prev_keys so holding A to enter the menu doesn't immediately launch. */
  prev_keys = get_keys();
  bm_assets_init();

  int selection = 0;
  draw_home_screen(selection);

  while (1) {
    u32 kdown = get_keys_down();

    if (kdown & BUTTON_DLEFT) {
      if (selection > 0) {
        selection--;
        draw_home_screen(selection);
      }
    }

    if (kdown & BUTTON_DRIGHT) {
      if (selection < 2) {
        selection++;
        draw_home_screen(selection);
      }
    }

    if (kdown & BUTTON_A) {
      if (selection == 0) {
        boot_aurora();
        draw_home_screen(selection);
      } else if (selection == 1) {
        if (!ui_power_off_confirm())
          power_off();
        draw_home_screen(selection);
      } else if (selection == 2) {
        ui_settings_menu();
        draw_home_screen(selection);
      }
    }

    delay(60000);
  }

  return 0;
}
