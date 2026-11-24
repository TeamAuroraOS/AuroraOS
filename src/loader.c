#include "assets.h"
#include "aurora.h"
#include "container.h"
#include "ff.h"
#include "font.h"
#include "icons.h"
#include "loader.h"

static FATFS loader_fs;
static FIL loader_file;

static u32 l_strlen(const char *s) {
  u32 n = 0;
  while (s[n])
    n++;
  return n;
}

static char *l_cpy(char *dst, const char *src) {
  while ((*dst = *src)) {
    dst++;
    src++;
  }
  return dst;
}

static char *l_dec(char *p, int v) {
  char tmp[12];
  int i = 0;
  unsigned uv = (v < 0) ? (unsigned)(-v) : (unsigned)v;
  if (uv == 0)
    tmp[i++] = '0';
  while (uv) {
    tmp[i++] = (char)('0' + uv % 10);
    uv /= 10;
  }
  if (v < 0)
    *p++ = '-';
  while (i)
    *p++ = tmp[--i];
  *p = '\0';
  return p;
}

static char *l_hex32(char *p, u32 v) {
  static const char d[] = "0123456789ABCDEF";
  *p++ = '0';
  *p++ = 'x';
  for (int i = 28; i >= 0; i -= 4)
    *p++ = d[(v >> i) & 0xF];
  *p = '\0';
  return p;
}

static void l_text(volatile u8 *fb, int x, int y, const char *s, Color fg,
                   Color bg, u32 font_id) {
  Font f;
  if (assets_ok() && asset_font(font_id, &f) && f.head)
    draw_text(fb, x, y + text_ascent(&f), BOT_SCREEN_HEIGHT, s, fg, &f);
  else
    draw_string(fb, x, y, BOT_SCREEN_HEIGHT, s, fg, bg);
}

static void l_text_mid(volatile u8 *fb, int cx, int y, const char *s, Color fg,
                       Color bg, u32 font_id) {
  Font f;
  if (assets_ok() && asset_font(font_id, &f) && f.head) {
    int w = text_width(&f, s);
    draw_text(fb, cx - w / 2, y + text_ascent(&f), BOT_SCREEN_HEIGHT, s, fg,
              &f);
  } else {
    int w = (int)l_strlen(s) * FONT_WIDTH;
    draw_string(fb, cx - w / 2, y, BOT_SCREEN_HEIGHT, s, fg, bg);
  }
}

static void loader_draw_progress(const char *title, const char *subtitle,
                                 int progress) {
  volatile u8 *fb = VRAM_BOT_A;
  const int W = BOT_SCREEN_WIDTH, H = BOT_SCREEN_HEIGHT;

  draw_vgradient(fb, 0, 0, W, H, H, COLOR_HM_BG_TOP, COLOR_HM_BG_BOT);
  draw_filled_round_rect(fb, 24, 44, W - 48, 140, 14, H, COLOR_HM_CARD);

  if (!assets_ok() ||
      !draw_asset_boxed(fb, (W - 44) / 2, 60, 44, 44, H,
                        ASSET_ICON_FILE_AURORA_32, COLOR_AURORA)) {
    draw_icon_32(fb, (W - ICON_SIZE) / 2, 66, H, icon_boot_bits, COLOR_AURORA);
  }

  l_text_mid(fb, W / 2, 114, title, COLOR_WHITE, COLOR_HM_CARD,
             ASSET_FONT_BOLD);
  if (subtitle && subtitle[0])
    l_text_mid(fb, W / 2, 136, subtitle, COLOR_HM_TEXT2, COLOR_HM_CARD,
               ASSET_FONT_SMALL);

  int bx = 52, by = 160, bw = W - 104, bh = 8;
  draw_filled_round_rect(fb, bx, by, bw, bh, 4, H, COLOR_HM_BAR);
  int fw = (bw * progress) / 100;
  if (fw < 8)
    fw = 8;
  if (fw > bw)
    fw = bw;
  draw_filled_round_rect(fb, bx, by, fw, bh, 4, H, COLOR_AURORA);
  screen_present_bottom();
}

static void loader_draw_error(const char *title, const char *subtitle,
                              const char *d1, const char *d2, const char *d3,
                              int detailed) {
  volatile u8 *fb = VRAM_BOT_A;
  const int W = BOT_SCREEN_WIDTH, H = BOT_SCREEN_HEIGHT;
  Color red = {0xFF, 0x5A, 0x52};

  draw_vgradient(fb, 0, 0, W, H, H, COLOR_HM_BG_TOP, COLOR_HM_BG_BOT);

  if (!detailed) {
    draw_filled_round_rect(fb, 24, 44, W - 48, 140, 14, H, COLOR_HM_CARD);
    if (!assets_ok() ||
        !draw_asset_boxed(fb, (W - 44) / 2, 60, 44, 44, H,
                          ASSET_ICON_REPORT_32, red)) {
      draw_icon_32(fb, (W - ICON_SIZE) / 2, 66, H, icon_boot_bits, red);
    }
    l_text_mid(fb, W / 2, 116, title, COLOR_WHITE, COLOR_HM_CARD,
               ASSET_FONT_BOLD);
    if (subtitle && subtitle[0])
      l_text_mid(fb, W / 2, 140, subtitle, COLOR_HM_TEXT2, COLOR_HM_CARD,
                 ASSET_FONT_SMALL);
    draw_filled_round_rect(fb, 12, H - 36, W - 24, 26, 9, H, COLOR_HM_BAR);
    l_text_mid(fb, W / 2, H - 29, "START: Details   B: Boot Menu",
               COLOR_HM_TEXT2, COLOR_HM_BAR, ASSET_FONT_SMALL);
  } else {
    draw_filled_round_rect(fb, 8, 8, W - 16, 30, 10, H, COLOR_HM_BAR);
    l_text_mid(fb, W / 2, 15, "Error Details", red, COLOR_HM_BAR,
               ASSET_FONT_BOLD);
    draw_filled_round_rect(fb, 12, 46, W - 24, H - 90, 12, H, COLOR_HM_CARD);
    l_text(fb, 22, 58, title, COLOR_WHITE, COLOR_HM_CARD, ASSET_FONT_BOLD);
    if (d1 && d1[0])
      l_text(fb, 22, 86, d1, COLOR_HM_TEXT2, COLOR_HM_CARD, ASSET_FONT_SMALL);
    if (d2 && d2[0])
      l_text(fb, 22, 108, d2, COLOR_HM_TEXT2, COLOR_HM_CARD, ASSET_FONT_SMALL);
    if (d3 && d3[0])
      l_text(fb, 22, 130, d3, COLOR_HM_TEXT2, COLOR_HM_CARD, ASSET_FONT_SMALL);
    draw_filled_round_rect(fb, 12, H - 36, W - 24, 26, 9, H, COLOR_HM_BAR);
    l_text_mid(fb, W / 2, H - 29, "START: Simple   B: Boot Menu",
               COLOR_HM_TEXT2, COLOR_HM_BAR, ASSET_FONT_SMALL);
  }
  screen_present_bottom();
}

static void loader_error(const char *title, const char *subtitle,
                         const char *d1, const char *d2, const char *d3) {
  int detailed = 0;
  f_mount(NULL, "", 0);
  if (!assets_ok())
    assets_load();
  loader_draw_error(title, subtitle, d1, d2, d3, detailed);
  while (1) {
    u32 k = get_keys_down();
    if (k & BUTTON_START) {
      detailed = !detailed;
      loader_draw_error(title, subtitle, d1, d2, d3, detailed);
    }
    if (k & BUTTON_B)
      break;
    delay(60000);
  }
}

void boot_aurora_ex(int quiet) {
  UINT br;
  char d1[64], d2[64], d3[64];
  char *p;

  if (!quiet)
    loader_draw_progress("Starting AuroraOS...", "Please wait", 15);

  FRESULT fr = f_mount(&loader_fs, "", 1);
  if (fr != FR_OK) {
    p = l_cpy(d1, "Stage: SD Mount (FatFs)");
    p = l_cpy(d2, "FRESULT code: ");
    l_dec(p, (int)fr);
    l_cpy(d3, "Target: 0:/AURORAOS.BIN");
    loader_error("SD Card Not Found", "Insert your SD card and try again.",
                 d1, d2, d3);
    return;
  }

  if (!quiet)
    loader_draw_progress("Starting AuroraOS...", "Loading system...", 35);

  fr = f_open(&loader_file, "AURORAOS.BIN", FA_READ);
  if (fr != FR_OK) {
    l_cpy(d1, "Stage: Open AURORAOS.BIN");
    p = l_cpy(d2, "FRESULT code: ");
    l_dec(p, (int)fr);
    l_cpy(d3, "Expected on SD card root");
    loader_error("AuroraOS Not Found", "Put AURORAOS.BIN on your SD card.",
                 d1, d2, d3);
    return;
  }

  aos_header_t hdr;
  aurora_status_t st = aurora_parse_header(&loader_file, &hdr);
  if (st != AURORA_OK) {
    f_close(&loader_file);
    l_cpy(d1, "Stage: Parse AOS header");
    l_cpy(d2, st == AURORA_ERR_MAGIC ? "Reason: Invalid AOS1/AUR1/AURC magic"
                                     : "Reason: Header read failed");
    l_cpy(d3, "File: 0:/AURORAOS.BIN");
    loader_error("Couldn't Start AuroraOS", "The system file appears damaged.",
                 d1, d2, d3);
    return;
  }

  if (!quiet)
    loader_draw_progress("Starting AuroraOS...", "Preparing system...", 65);

  if (aurora_load_arm9(&loader_file, &hdr, (void *)hdr.arm9_load_addr) !=
      AURORA_OK) {
    f_close(&loader_file);
    l_cpy(d1, "Stage: Load ARM9 payload");
    p = l_cpy(d2, "Size: ");
    p = l_dec(p, (int)hdr.arm9_size);
    l_cpy(p, " bytes");
    p = l_cpy(d3, "Load addr: ");
    l_hex32(p, hdr.arm9_load_addr);
    loader_error("Couldn't Start AuroraOS", "Failed to read the system file.",
                 d1, d2, d3);
    return;
  }

  if (hdr.arm11_size) {
    fr = f_lseek(&loader_file, hdr.arm11_offset);
    if (fr == FR_OK)
      fr = f_read(&loader_file, (void *)hdr.arm11_load_addr, hdr.arm11_size,
                  &br);
    if (fr != FR_OK || br != hdr.arm11_size) {
      f_close(&loader_file);
      l_cpy(d1, "Stage: Load ARM11 payload");
      p = l_cpy(d2, "FRESULT: ");
      p = l_dec(p, (int)fr);
      l_cpy(p, "  Size: ");
      p = l_cpy(d3, "Load addr: ");
      l_hex32(p, hdr.arm11_load_addr);
      loader_error("Couldn't Start AuroraOS", "Failed to read the system file.",
                   d1, d2, d3);
      return;
    }
    *(volatile u32 *)AOS_ARM11_MAILBOX = hdr.arm11_entry;
  }

  f_close(&loader_file);
  f_mount(NULL, "", 0);

  if (!quiet)
    loader_draw_progress("Starting AuroraOS...", "Almost ready...", 100);

  aurora_jump_arm9(hdr.arm9_entry);
}

void boot_aurora(void) { boot_aurora_ex(0); }
