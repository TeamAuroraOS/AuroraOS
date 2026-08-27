#include "aurora.h"
#include "container.h"
#include "ff.h"
#include "loader.h"

static FATFS loader_fs;
static FIL loader_file;

static char *lcpy(char *dst, const char *src) {
  while ((*dst = *src)) {
    dst++;
    src++;
  }
  return dst;
}

static char *ldec(char *p, int v) {
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

static char *lhex32(char *p, u32 v) {
  static const char d[] = "0123456789ABCDEF";
  *p++ = '0';
  *p++ = 'x';
  for (int i = 28; i >= 0; i -= 4)
    *p++ = d[(v >> i) & 0xF];
  *p = '\0';
  return p;
}

static int loader_y;

static void loader_line(const char *msg, Color color) {
  draw_string(VRAM_BOT_A, 8, loader_y, BOT_SCREEN_HEIGHT, msg, color,
              COLOR_BG_DARK);
  loader_y += 14;
  screen_present_bottom();
}

static void loader_wait_back(void) {
  draw_string(VRAM_BOT_A, 8, BOT_SCREEN_HEIGHT - 18, BOT_SCREEN_HEIGHT,
              "B: Back", COLOR_LIGHT_GRAY, COLOR_BG_DARK);
  screen_present_bottom();
  while (1) {
    if (get_keys_down() & BUTTON_B)
      break;
    delay(60000);
  }
  f_mount(NULL, "", 0);
}

void boot_aurora(void) {
  clear_screen(VRAM_BOT_A, BOT_FB_SIZE, COLOR_BG_DARK);
  draw_string(VRAM_BOT_A, 8, 8, BOT_SCREEN_HEIGHT, "Boot Aurora", COLOR_AURORA,
              COLOR_BG_DARK);
  loader_y = 30;

  char line[64];
  char *p;
  UINT br;

  FRESULT fr = f_mount(&loader_fs, "", 1);
  if (fr != FR_OK) {
    p = lcpy(line, "Mount failed: ");
    ldec(p, (int)fr);
    loader_line(line, COLOR_RED);
    loader_wait_back();
    return;
  }

  fr = f_open(&loader_file, "AURORAOS.BIN", FA_READ);
  if (fr != FR_OK) {
    p = lcpy(line, "Open AURORAOS.BIN: ");
    ldec(p, (int)fr);
    loader_line(line, COLOR_RED);
    loader_line("Put AURORAOS.BIN on SD root", COLOR_YELLOW);
    loader_wait_back();
    return;
  }

  /* Header + magic: shared with the Home Menu app launcher (container.c). */
  aos_header_t hdr;
  aurora_status_t st = aurora_parse_header(&loader_file, &hdr);
  if (st == AURORA_ERR_READ) {
    loader_line("Header read failed", COLOR_RED);
    f_close(&loader_file);
    loader_wait_back();
    return;
  }
  if (st == AURORA_ERR_MAGIC) {
    loader_line("Bad AOS1/AUR1 magic", COLOR_RED);
    f_close(&loader_file);
    loader_wait_back();
    return;
  }

  /* ARM9 payload. The firm runs low in memory, so it can read the payload
   * straight to its final load address. */
  p = lcpy(line, "ARM9 ");
  p = ldec(p, (int)hdr.arm9_size);
  p = lcpy(p, "B -> ");
  lhex32(p, hdr.arm9_load_addr);
  loader_line(line, COLOR_WHITE);

  if (aurora_load_arm9(&loader_file, &hdr, (void *)hdr.arm9_load_addr) !=
      AURORA_OK) {
    loader_line("ARM9 load failed", COLOR_RED);
    f_close(&loader_file);
    loader_wait_back();
    return;
  }

  p = lcpy(line, "  first word: ");
  lhex32(p, *(volatile u32 *)hdr.arm9_load_addr);
  loader_line(line, COLOR_LIGHT_GRAY);

  /* Optional ARM11 payload. */
  if (hdr.arm11_size) {
    p = lcpy(line, "ARM11 ");
    p = ldec(p, (int)hdr.arm11_size);
    p = lcpy(p, "B -> ");
    lhex32(p, hdr.arm11_load_addr);
    loader_line(line, COLOR_WHITE);

    fr = f_lseek(&loader_file, hdr.arm11_offset);
    if (fr == FR_OK)
      fr = f_read(&loader_file, (void *)hdr.arm11_load_addr, hdr.arm11_size,
                  &br);
    if (fr != FR_OK || br != hdr.arm11_size) {
      loader_line("ARM11 load failed", COLOR_RED);
      f_close(&loader_file);
      loader_wait_back();
      return;
    }
    *(volatile u32 *)AOS_ARM11_MAILBOX = hdr.arm11_entry;
  }

  f_close(&loader_file);
  f_mount(NULL, "", 0);

  p = lcpy(line, "Jumping -> ");
  lhex32(p, hdr.arm9_entry);
  loader_line(line, COLOR_GREEN);
  delay(20000000); /* Keep the status on screen briefly. */

  aurora_jump_arm9(hdr.arm9_entry); /* Never returns. */
}
