

#include "aurora.h"
#include "aurora_logo.h"
#include "font.h"
#include "ff.h"
#include "i2c.h"
#include "icons.h"
#include "loader.h"
#include "sdmmc.h"

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

static void draw_splash_screen(void) {
  volatile u8 *fb = VRAM_TOP_LA;

  for (int x = 0; x < TOP_SCREEN_WIDTH; x++) {
    for (int y = 0; y < TOP_SCREEN_HEIGHT; y++) {
      u8 r = (u8)(5 + (y * 30) / TOP_SCREEN_HEIGHT);
      u8 g = (u8)(10 + (y * 120) / TOP_SCREEN_HEIGHT);
      u8 b = (u8)(20 + (y * 80) / TOP_SCREEN_HEIGHT);

      u32 offset =
          ((x * TOP_SCREEN_HEIGHT) + (TOP_SCREEN_HEIGHT - 1 - y)) * 3;
      fb[offset + 0] = b;
      fb[offset + 1] = g;
      fb[offset + 2] = r;
    }
  }

  int ribbon_y_start = 90;
  int ribbon_y_end = 150;
  for (int x = 0; x < TOP_SCREEN_WIDTH; x++) {
    for (int y = ribbon_y_start; y < ribbon_y_end; y++) {
      int center = (ribbon_y_start + ribbon_y_end) / 2;
      int dist = y - center;
      if (dist < 0)
        dist = -dist;
      int max_dist = (ribbon_y_end - ribbon_y_start) / 2;

      u8 intensity = (u8)(255 - (dist * 255 / max_dist));

      u8 r = (u8)((50 * intensity) / 255);
      u8 g = (u8)((232 * intensity) / 255);
      u8 b = (u8)((200 * intensity) / 255);

      u32 offset =
          ((x * TOP_SCREEN_HEIGHT) + (TOP_SCREEN_HEIGHT - 1 - y)) * 3;
      u8 bg_b = fb[offset + 0];
      u8 bg_g = fb[offset + 1];
      u8 bg_r = fb[offset + 2];

      fb[offset + 0] =
          (u8)((b * intensity + bg_b * (255 - intensity)) / 255);
      fb[offset + 1] =
          (u8)((g * intensity + bg_g * (255 - intensity)) / 255);
      fb[offset + 2] =
          (u8)((r * intensity + bg_r * (255 - intensity)) / 255);
    }
  }

  int logo_x = (TOP_SCREEN_WIDTH - AURORA_LOGO_WIDTH) / 2;
  int logo_y = 90;
  draw_aurora_logo(VRAM_TOP_LA, logo_x, logo_y, TOP_SCREEN_HEIGHT,
                   COLOR_WHITE);

  const char *version = "v0.0.8 - Initial Build";
  int ver_len = (int)str_len(version);
  int ver_x = (TOP_SCREEN_WIDTH - ver_len * FONT_WIDTH) / 2;
  int ver_y = 165;
  Color ver_color = {0x80, 0xC0, 0xA0};
  draw_string(VRAM_TOP_LA, ver_x, ver_y, TOP_SCREEN_HEIGHT, version, ver_color,
              ver_color);
}

/* Launcher tile layout for the three main actions. */
#define TILE_SIZE       80
#define TILE_Y          70
#define TILE_GAP        16
#define TILE_TOTAL_W    (TILE_SIZE * 3 + TILE_GAP * 2)
#define TILE_X_START    ((BOT_SCREEN_WIDTH - TILE_TOTAL_W) / 2)
#define TILE_STEP       (TILE_SIZE + TILE_GAP)
#define TILE_BOOT_X     TILE_X_START
#define TILE_POWER_X    (TILE_X_START + TILE_STEP)
#define TILE_SETTINGS_X (TILE_X_START + TILE_STEP * 2)

#define SEL_BORDER 3

static void draw_home_screen(int selection);

static void draw_tile(int x, int y, Color bg_color,
                      const unsigned char *icon_bits, const char *label,
                      int selected) {
  volatile u8 *fb = VRAM_BOT_A;

  if (selected) {
    draw_filled_rect(fb, x - SEL_BORDER, y - SEL_BORDER,
                     TILE_SIZE + SEL_BORDER * 2, TILE_SIZE + SEL_BORDER * 2,
                     BOT_SCREEN_HEIGHT, COLOR_WHITE);
  }

  draw_filled_rect(fb, x, y, TILE_SIZE, TILE_SIZE, BOT_SCREEN_HEIGHT, bg_color);

  int icon_x = x + (TILE_SIZE - ICON_SIZE) / 2;
  int icon_y = y + (TILE_SIZE - ICON_SIZE) / 2 - 6;
  draw_icon_32(fb, icon_x, icon_y, BOT_SCREEN_HEIGHT, icon_bits, COLOR_WHITE);

  int label_len = (int)str_len(label);
  int label_x = x + (TILE_SIZE - label_len * FONT_WIDTH) / 2;
  int label_y = y + TILE_SIZE - FONT_HEIGHT - 6;
  draw_string(fb, label_x, label_y, BOT_SCREEN_HEIGHT, label, COLOR_WHITE,
              bg_color);
}

void ui_draw_home_screen(void) { draw_home_screen(0); }

static void draw_home_screen(int selection) {
  clear_screen(VRAM_BOT_A, BOT_FB_SIZE, COLOR_BG_DARK);

  const char *title = "Aurora Launcher";
  int title_len = (int)str_len(title);
  int title_x = (BOT_SCREEN_WIDTH - title_len * FONT_WIDTH) / 2;
  draw_string(VRAM_BOT_A, title_x, 16, BOT_SCREEN_HEIGHT, title, COLOR_AURORA,
              COLOR_BG_DARK);

  const char *subtitle = "Select an option";
  int sub_len = (int)str_len(subtitle);
  int sub_x = (BOT_SCREEN_WIDTH - sub_len * FONT_WIDTH) / 2;
  draw_string(VRAM_BOT_A, sub_x, 32, BOT_SCREEN_HEIGHT, subtitle,
              COLOR_LIGHT_GRAY, COLOR_BG_DARK);

  draw_tile(TILE_BOOT_X, TILE_Y, COLOR_BLUE, icon_boot_bits, "Boot",
            selection == 0);
  draw_tile(TILE_POWER_X, TILE_Y, COLOR_RED, icon_power_bits, "Power",
            selection == 1);
  draw_tile(TILE_SETTINGS_X, TILE_Y, COLOR_DARK_GRAY, icon_settings_bits,
            "Settings", selection == 2);

  const char *hint2 = "A:OK  START:SD  SELECT:FS";
  int hint2_len = (int)str_len(hint2);
  int hint2_x = (BOT_SCREEN_WIDTH - hint2_len * FONT_WIDTH) / 2;
  draw_string(VRAM_BOT_A, hint2_x, BOT_SCREEN_HEIGHT - 20, BOT_SCREEN_HEIGHT,
              hint2, COLOR_LIGHT_GRAY, COLOR_BG_DARK);
  screen_present_bottom();
}

int ui_power_off_confirm(void) {
  clear_screen(VRAM_BOT_A, BOT_FB_SIZE, COLOR_BG_DARK);

  int icon_x = (BOT_SCREEN_WIDTH - ICON_SIZE) / 2;
  int icon_y = 70;
  draw_icon_32(VRAM_BOT_A, icon_x, icon_y, BOT_SCREEN_HEIGHT,
               icon_power_bits, COLOR_RED);

  const char *msg1 = "Powering off...";
  int msg1_len = (int)str_len(msg1);
  int msg1_x = (BOT_SCREEN_WIDTH - msg1_len * FONT_WIDTH) / 2;
  draw_string(VRAM_BOT_A, msg1_x, 120, BOT_SCREEN_HEIGHT, msg1, COLOR_WHITE,
              COLOR_BG_DARK);
  screen_present_bottom();

  return 0;
}

static void draw_settings_screen(void) {
  clear_screen(VRAM_BOT_A, BOT_FB_SIZE, COLOR_BG_DARK);

  draw_filled_rect(VRAM_BOT_A, 0, 0, BOT_SCREEN_WIDTH, 24, BOT_SCREEN_HEIGHT,
                   COLOR_DARK_GRAY);
  const char *title = "Settings";
  int title_len = (int)str_len(title);
  int title_x = (BOT_SCREEN_WIDTH - title_len * FONT_WIDTH) / 2;
  draw_string(VRAM_BOT_A, title_x, 8, BOT_SCREEN_HEIGHT, title, COLOR_WHITE,
              COLOR_DARK_GRAY);

  int icon_x = 20;
  int icon_y = 50;
  draw_icon_32(VRAM_BOT_A, icon_x, icon_y, BOT_SCREEN_HEIGHT,
               icon_brightness_bits, COLOR_YELLOW);

  
  draw_string(VRAM_BOT_A, 60, 55, BOT_SCREEN_HEIGHT, "Screen Brightness",
              COLOR_WHITE, COLOR_BG_DARK);

  
  int bar_x = 60;
  int bar_y = 75;
  int bar_w = 200;
  int bar_h = 12;
  int seg_w = bar_w / BRIGHTNESS_LEVELS;

  
  draw_filled_rect(VRAM_BOT_A, bar_x, bar_y, bar_w, bar_h, BOT_SCREEN_HEIGHT,
                   COLOR_DARK_GRAY);

  
  int fill_w = seg_w * (current_brightness + 1);
  draw_filled_rect(VRAM_BOT_A, bar_x, bar_y, fill_w, bar_h, BOT_SCREEN_HEIGHT,
                   COLOR_AURORA);

  
  for (int i = 1; i < BRIGHTNESS_LEVELS; i++) {
    int mx = bar_x + seg_w * i;
    draw_filled_rect(VRAM_BOT_A, mx, bar_y, 2, bar_h, BOT_SCREEN_HEIGHT,
                     COLOR_BG_DARK);
  }

  
  char level_str[] = "Level: X/X";
  level_str[7] = '1' + (char)current_brightness;
  level_str[9] = '0' + BRIGHTNESS_LEVELS;
  draw_string(VRAM_BOT_A, 60, 95, BOT_SCREEN_HEIGHT, level_str,
              COLOR_LIGHT_GRAY, COLOR_BG_DARK);

  
  draw_string(VRAM_BOT_A, 20, BOT_SCREEN_HEIGHT - 40, BOT_SCREEN_HEIGHT,
              "D-Pad: Adjust Brightness", COLOR_LIGHT_GRAY, COLOR_BG_DARK);
  draw_string(VRAM_BOT_A, 20, BOT_SCREEN_HEIGHT - 24, BOT_SCREEN_HEIGHT,
              "B: Back to Home", COLOR_LIGHT_GRAY, COLOR_BG_DARK);
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
  clear_screen(VRAM_BOT_A, BOT_FB_SIZE, COLOR_BG_DARK);

  const char *title = "SD Card Test";
  int title_len = (int)str_len(title);
  int title_x = (BOT_SCREEN_WIDTH - title_len * FONT_WIDTH) / 2;
  draw_string(VRAM_BOT_A, title_x, 8, BOT_SCREEN_HEIGHT, title, COLOR_AURORA,
              COLOR_BG_DARK);
  screen_present_bottom();

  char line[48];
  int y = 32;

  int init_res = sdmmc_sdcard_init();
  char *p = str_cpy_ret(line, "Init: ");
  int_to_str(init_res, p);
  draw_string(VRAM_BOT_A, 8, y, BOT_SCREEN_HEIGHT, line,
              init_res == 0 ? COLOR_GREEN : COLOR_RED, COLOR_BG_DARK);
  y += 16;

  if (init_res != 0) {
    draw_string(VRAM_BOT_A, 8, y, BOT_SCREEN_HEIGHT, "SD init failed",
                COLOR_RED, COLOR_BG_DARK);
  } else {
    int read_res = sdmmc_sdcard_readsectors(0, 1, sd_test_buf);
    p = str_cpy_ret(line, "Read: ");
    int_to_str(read_res, p);
    draw_string(VRAM_BOT_A, 8, y, BOT_SCREEN_HEIGHT, line,
                read_res == 0 ? COLOR_GREEN : COLOR_RED, COLOR_BG_DARK);
    y += 20;

    draw_string(VRAM_BOT_A, 8, y, BOT_SCREEN_HEIGHT, "Sector 0, bytes 0-15:",
                COLOR_LIGHT_GRAY, COLOR_BG_DARK);
    y += 16;
    build_hex_line(&sd_test_buf[0], 8, line);
    draw_string(VRAM_BOT_A, 8, y, BOT_SCREEN_HEIGHT, line, COLOR_WHITE,
                COLOR_BG_DARK);
    y += 12;
    build_hex_line(&sd_test_buf[8], 8, line);
    draw_string(VRAM_BOT_A, 8, y, BOT_SCREEN_HEIGHT, line, COLOR_WHITE,
                COLOR_BG_DARK);
    y += 20;

    p = str_cpy_ret(line, "Signature: ");
    *p++ = hex_nibble((u8)(sd_test_buf[510] >> 4));
    *p++ = hex_nibble((u8)(sd_test_buf[510] & 0xF));
    *p++ = hex_nibble((u8)(sd_test_buf[511] >> 4));
    *p++ = hex_nibble((u8)(sd_test_buf[511] & 0xF));
    *p = '\0';
    draw_string(VRAM_BOT_A, 8, y, BOT_SCREEN_HEIGHT, line, COLOR_WHITE,
                COLOR_BG_DARK);
    y += 16;

    int pass = (sd_test_buf[510] == 0x55 && sd_test_buf[511] == 0xAA);
    draw_string(VRAM_BOT_A, 8, y, BOT_SCREEN_HEIGHT,
                pass ? "PASS: valid boot sector" : "No 0x55AA signature",
                pass ? COLOR_GREEN : COLOR_YELLOW, COLOR_BG_DARK);
  }

  draw_string(VRAM_BOT_A, 8, BOT_SCREEN_HEIGHT - 20, BOT_SCREEN_HEIGHT,
              "B: Back", COLOR_LIGHT_GRAY, COLOR_BG_DARK);
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
  clear_screen(VRAM_BOT_A, BOT_FB_SIZE, COLOR_BG_DARK);

  const char *title = "FatFs Test";
  int title_len = (int)str_len(title);
  int title_x = (BOT_SCREEN_WIDTH - title_len * FONT_WIDTH) / 2;
  draw_string(VRAM_BOT_A, title_x, 8, BOT_SCREEN_HEIGHT, title, COLOR_AURORA,
              COLOR_BG_DARK);
  screen_present_bottom();

  char line[48];
  int y = 32;

  FRESULT fr = f_mount(&fs_volume, "", 1);
  char *p = str_cpy_ret(line, "Mount: ");
  int_to_str((int)fr, p);
  draw_string(VRAM_BOT_A, 8, y, BOT_SCREEN_HEIGHT, line,
              fr == FR_OK ? COLOR_GREEN : COLOR_RED, COLOR_BG_DARK);
  y += 16;

  if (fr == FR_OK) {
    fr = f_open(&fs_file, "test.txt", FA_READ);
    p = str_cpy_ret(line, "Open test.txt: ");
    int_to_str((int)fr, p);
    draw_string(VRAM_BOT_A, 8, y, BOT_SCREEN_HEIGHT, line,
                fr == FR_OK ? COLOR_GREEN : COLOR_RED, COLOR_BG_DARK);
    y += 16;

    if (fr == FR_OK) {
      char data[33];
      UINT br = 0;
      fr = f_read(&fs_file, data, sizeof(data) - 1, &br);
      f_close(&fs_file);

      p = str_cpy_ret(line, "Read: ");
      p = str_cpy_ret(p, (fr == FR_OK) ? "OK  bytes=" : "ERR bytes=");
      int_to_str((int)br, p);
      draw_string(VRAM_BOT_A, 8, y, BOT_SCREEN_HEIGHT, line,
                  fr == FR_OK ? COLOR_GREEN : COLOR_RED, COLOR_BG_DARK);
      y += 20;

      for (UINT i = 0; i < br; i++) {
        if (data[i] < 0x20 || data[i] > 0x7E)
          data[i] = '.';
      }
      data[br] = '\0';
      draw_string(VRAM_BOT_A, 8, y, BOT_SCREEN_HEIGHT, "Contents:",
                  COLOR_LIGHT_GRAY, COLOR_BG_DARK);
      y += 16;
      draw_string(VRAM_BOT_A, 8, y, BOT_SCREEN_HEIGHT, data, COLOR_WHITE,
                  COLOR_BG_DARK);
      y += 20;

      if (fr == FR_OK && br > 0)
        draw_string(VRAM_BOT_A, 8, y, BOT_SCREEN_HEIGHT,
                    "PASS: file read OK", COLOR_GREEN, COLOR_BG_DARK);
    } else {
      draw_string(VRAM_BOT_A, 8, y, BOT_SCREEN_HEIGHT,
                  "Put test.txt on SD root", COLOR_YELLOW, COLOR_BG_DARK);
    }
  }

  f_mount(NULL, "", 0);

  draw_string(VRAM_BOT_A, 8, BOT_SCREEN_HEIGHT - 20, BOT_SCREEN_HEIGHT,
              "B: Back", COLOR_LIGHT_GRAY, COLOR_BG_DARK);
  screen_present_bottom();

  while (1) {
    u32 kdown = get_keys_down();
    if (kdown & BUTTON_B)
      return;
    delay(60000);
  }
}

int main(void) {
  screen_init();
  draw_splash_screen();
  set_brightness((u32)current_brightness);

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
        /* Boot Aurora: load AURORAOS.BIN (the OS) and jump into it. */
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

    if (kdown & BUTTON_START) {
      ui_sd_test();
      draw_home_screen(selection);
    }

    if (kdown & BUTTON_SELECT) {
      ui_fs_test();
      draw_home_screen(selection);
    }

    delay(60000);
  }

  return 0;
}
