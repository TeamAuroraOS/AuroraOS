/* Screenshots. The panels are copied out of VRAM first, so the picture is the
 * frame that was showing even though the card write takes a while, and that
 * copy is also what puts the screen back after the notice. */
#include "screenshot.h"
#include "aurora.h"
#include "ff.h"
#include "gpu.h"
#include "power.h"
#include "timer.h"
#include "ui.h"

/* Clear of the image viewer's RGB buffer, which ends at 0x26900000, and the
 * ARM11 mailbox at 0x27000000. */
#define SHOT_TOP_ADDR 0x26A00000u /* 288,000 bytes -> 0x26A46500 */
#define SHOT_BOT_ADDR 0x26A50000u /* 230,400 bytes -> 0x26A88400 */
#define SHOT_BMP_ADDR 0x26A90000u /* 576,054 bytes -> 0x26B1CA36 */

#define SHOT_W    TOP_SCREEN_WIDTH
#define SHOT_H    (TOP_SCREEN_HEIGHT + BOT_SCREEN_HEIGHT)
#define SHOT_HEAD 54u
#define SHOT_SIZE (SHOT_HEAD + (u32)SHOT_W * (u32)SHOT_H * 3u)
#define SHOT_DIR  "0:/Aurora/Screenshots"
#define BOT_X     ((TOP_SCREEN_WIDTH - BOT_SCREEN_WIDTH) / 2)

#define NOTE_W  190
#define NOTE_H  30
#define NOTE_X  ((TOP_SCREEN_WIDTH - NOTE_W) / 2)
#define NOTE_Y  (TOP_SCREEN_HEIGHT - NOTE_H - 12)
#define NOTE_US 900000u

static void copy_words(volatile u8 *dst, const volatile u8 *src, u32 size) {
  volatile u32 *d = (volatile u32 *)dst;
  const volatile u32 *s = (const volatile u32 *)src;
  for (u32 i = 0; i < (size >> 2); i++)
    d[i] = s[i];
}

static void put_le(u8 *p, u32 v, int bytes) {
  for (int i = 0; i < bytes; i++)
    p[i] = (u8)(v >> (8 * i));
}

/* A BMP stores rows bottom-up and pixels as B, G, R, the order the framebuffers
 * already use. 400 * 3 is a multiple of 4, so rows need no padding. */
static void build_bmp(void) {
  const u8 *top = (const u8 *)SHOT_TOP_ADDR;
  const u8 *bot = (const u8 *)SHOT_BOT_ADDR;
  u8 *bmp = (u8 *)SHOT_BMP_ADDR;
  u8 *p = bmp + SHOT_HEAD;

  for (u32 i = 0; i < SHOT_HEAD; i++)
    bmp[i] = 0;
  bmp[0] = 'B';
  bmp[1] = 'M';
  put_le(bmp + 2, SHOT_SIZE, 4);
  put_le(bmp + 10, SHOT_HEAD, 4);
  put_le(bmp + 14, 40, 4);
  put_le(bmp + 18, SHOT_W, 4);
  put_le(bmp + 22, SHOT_H, 4);
  put_le(bmp + 26, 1, 2);
  put_le(bmp + 28, 24, 2);
  put_le(bmp + 34, SHOT_SIZE - SHOT_HEAD, 4);
  put_le(bmp + 38, 2835, 4); /* 72 dpi */
  put_le(bmp + 42, 2835, 4);

  /* The framebuffers run column by column from the bottom of each column. */
  for (int y = SHOT_H - 1; y >= 0; y--) {
    for (int x = 0; x < SHOT_W; x++, p += 3) {
      const u8 *s = 0;
      if (y < TOP_SCREEN_HEIGHT)
        s = top + ((u32)x * TOP_SCREEN_HEIGHT +
                   (u32)(TOP_SCREEN_HEIGHT - 1 - y)) * 3u;
      else if (x >= BOT_X && x < BOT_X + BOT_SCREEN_WIDTH)
        s = bot + ((u32)(x - BOT_X) * BOT_SCREEN_HEIGHT +
                   (u32)(SHOT_H - 1 - y)) * 3u;
      if (s) {
        p[0] = s[0];
        p[1] = s[1];
        p[2] = s[2];
      } else {
        p[0] = p[1] = p[2] = 0;
      }
    }
  }
}

static char *put_num(char *p, int v, int digits) {
  for (int i = digits - 1; i >= 0; i--) {
    p[i] = (char)('0' + v % 10);
    v /= 10;
  }
  return p + digits;
}

static char *put_str(char *p, const char *s) {
  while (*s)
    *p++ = *s++;
  return p;
}

/* "0:/Aurora/Screenshots/2026-09-16_14-05-33.bmp", or a numbered name when the
 * clock cannot be read. A suffix keeps two shots in one second apart. */
static int pick_name(char *path) {
  static FILINFO fno;
  RtcTime t;
  int have_time = rtc_read(&t);

  for (int n = 1; n < 1000; n++) {
    char *p = put_str(path, SHOT_DIR "/");
    if (have_time) {
      p = put_num(p, t.year, 4);
      *p++ = '-';
      p = put_num(p, t.month, 2);
      *p++ = '-';
      p = put_num(p, t.day, 2);
      *p++ = '_';
      p = put_num(p, t.hour, 2);
      *p++ = '-';
      p = put_num(p, t.min, 2);
      *p++ = '-';
      p = put_num(p, t.sec, 2);
      if (n > 1) {
        *p++ = '_';
        p = put_num(p, n, n < 10 ? 1 : (n < 100 ? 2 : 3));
      }
    } else {
      p = put_str(p, "Screenshot_");
      p = put_num(p, n, 3);
    }
    p = put_str(p, ".bmp");
    *p = 0;
    FRESULT r = f_stat(path, &fno);
    if (r == FR_NO_FILE)
      return 1;
    if (r != FR_OK)
      return 0; /* no folder, or no card: numbering on would not help */
  }
  return 0;
}

/* Uses the card as whoever is running has it mounted, since mounting over that
 * would invalidate their open files; mounts it only when nothing has. */
static int save(void) {
  static FATFS fs;
  static FIL f;
  static DIR probe;
  static char path[64];
  int own = 0, ok = 0;
  UINT bw = 0;
  FRESULT r = f_opendir(&probe, "0:/");

  if (r == FR_NOT_ENABLED) {
    own = 1;
    if (f_mount(&fs, "0:", 1) != FR_OK) {
      f_mount(NULL, "0:", 0);
      return 0;
    }
  } else if (r != FR_OK) {
    return 0;
  } else {
    f_closedir(&probe);
  }

  f_mkdir("0:/Aurora");
  f_mkdir(SHOT_DIR);
  if (pick_name(path) && f_open(&f, path, FA_WRITE | FA_CREATE_NEW) == FR_OK) {
    ok = f_write(&f, (const void *)SHOT_BMP_ADDR, SHOT_SIZE, &bw) == FR_OK &&
         bw == SHOT_SIZE;
    if (f_close(&f) != FR_OK)
      ok = 0;
    if (!ok)
      f_unlink(path);
  }
  if (own)
    f_mount(NULL, "0:", 0);
  return ok;
}

/* Drawn straight onto the panel and taken off again from the copy, so the
 * frame the running screen drew is left untouched. */
static void notice(int ok) {
  volatile u8 *fb = (volatile u8 *)gpu_front(0);
  const int sh = TOP_SCREEN_HEIGHT;
  const u8 *copy = (const u8 *)SHOT_TOP_ADDR;
  u32 t0 = timer_ticks();

  draw_filled_round_rect(fb, NOTE_X, NOTE_Y, NOTE_W, NOTE_H, 15, sh,
                         COLOR_HM_BAR);
  ui_text_mid(fb, TOP_SCREEN_WIDTH / 2, NOTE_Y + (NOTE_H - ui_th(&ui_font)) / 2,
              sh, ok ? "Screenshot saved" : "Screenshot not saved",
              ok ? COLOR_WHITE : COLOR_RED, COLOR_HM_BAR, &ui_font);

  if (timer_calibrated())
    while (timer_us_since(t0) < NOTE_US)
      ;
  else
    delay(6000000);

  for (int x = NOTE_X; x < NOTE_X + NOTE_W; x++) {
    u32 base = ((u32)x * (u32)sh + (u32)(sh - NOTE_Y - NOTE_H)) * 3u;
    for (u32 i = 0; i < (u32)NOTE_H * 3u; i++)
      fb[base + i] = copy[base + i];
  }
}

void screenshot_take(void) {
  /* After any present in flight: gpu_front() waits for it. */
  copy_words((volatile u8 *)SHOT_TOP_ADDR, (volatile u8 *)gpu_front(0),
             TOP_FB_SIZE);
  copy_words((volatile u8 *)SHOT_BOT_ADDR, (volatile u8 *)gpu_front(1),
             BOT_FB_SIZE);
  build_bmp();
  notice(save());
}
