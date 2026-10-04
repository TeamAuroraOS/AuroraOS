/* Copies the Wi-Fi firmware from this console's NWM system module (title
 * 0004013000002D02) to SD:/Aurora/wifi: the files tools/nwm_extract.py makes
 * on a PC, with the same extraction.
 *
 * The module is an NCCH in CTRNAND (src/os/Nand.c). Its exheader and ExeFS
 * are AES-CTR encrypted with keyslot 0x2C: keyX from the bootrom, keyY the
 * first 16 bytes of the NCCH's signature. The ExeFS .code is then packed with
 * the 3DS's backwards LZ (exheader flag bit 0). Only that original NCCH crypto
 * is handled; a module encrypted another way is reported, not guessed at.
 * See docs/wifi.md "Copying the firmware on the console". */

#include "fwdump.h"
#include "aes.h"
#include "anim.h"
#include "ff.h"
#include "image.h"
#include "lang.h"
#include "nand.h"
#include "statusbar.h"
#include "timer.h"
#include "touch.h"
#include "ui.h"
#include <string.h>

#define BW  BOT_SCREEN_WIDTH
#define BSH BOT_SCREEN_HEIGHT
#define TW  TOP_SCREEN_WIDTH
#define TSH TOP_SCREEN_HEIGHT

/* The .code as read and decrypted, then unpacked: the image viewer's buffers,
 * idle while Settings is open. */
#define FD_IN      ((u8 *)IMAGE_FILE_ADDR)
#define FD_IN_MAX  (4u << 20)
#define FD_OUT     ((u8 *)IMAGE_RAW_ADDR)
#define FD_OUT_MAX (4u << 20)

#define NCCH_SLOT 0x2C
#define FW_DIR    "0:/Aurora/wifi"

static const struct {
  const char *dir;
  u32 tid_lo;
} fd_mods[2] = {
    {"1:/title/00040130/00002d02/content", 0x00002D02},
    {"1:/title/00040130/20002d02/content", 0x20002D02}, /* a New 3DS build */
};

/* In the unpacked .code, a literal pool marked 0x00524C00, 0x000003ED holds
 * (end, start) load addresses of each blob; the code loads at 0x100000. */
#define LOAD_BASE   0x00100000u
#define POOL_MARKER 0x00524C00u
#define POOL_VERIFY 0x000003EDu

static const struct {
  const char *name;
  u8 end, start; /* word indexes in the pool */
  u32 max;       /* what the Wi-Fi driver will load */
  int needed;
} fd_blobs[6] = {
    {"STUBDATA.BIN", 14, 15, 0x1000, 1},  {"STUBCODE.BIN", 11, 12, 0x1000, 1},
    {"DATABASE.BIN", 8, 9, 0x1000, 1},    {"MAINTYP1.BIN", 2, 3, 0x60000, 1},
    {"MAINTYP4.BIN", 4, 5, 0x60000, 1},   {"MAINTYP5.BIN", 6, 7, 0x60000, 0},
};

static u32 fd_off[6], fd_len[6];

static u32 rd32(const u8 *p) {
  return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24);
}

static char *put_str(char *p, const char *s) {
  while (*s)
    *p++ = *s++;
  *p = 0;
  return p;
}

static int hexval(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  c = (char)(c | 0x20);
  return (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
}

/* "0000001e.app" -> 0x1E. */
static int app_id(const char *name, u32 *id) {
  u32 v = 0;
  for (int i = 0; i < 8; i++) {
    int h = hexval(name[i]);
    if (h < 0)
      return 0;
    v = (v << 4) | (u32)h;
  }
  if (name[8] != '.' || (name[9] | 0x20) != 'a' || (name[10] | 0x20) != 'p' ||
      (name[11] | 0x20) != 'p' || name[12])
    return 0;
  *id = v;
  return 1;
}

static u32 ncch[0x200 / 4], exh[0x400 / 4], efs[0x200 / 4];

/* The newest content of the first NWM title CTRNAND has. */
static int fd_find(char *out) {
  static DIR dir;
  static FILINFO fno;
  static FIL f;
  const u8 *h = (const u8 *)ncch;

  for (int m = 0; m < 2; m++) {
    u32 best = 0;
    int have = 0;
    if (f_opendir(&dir, fd_mods[m].dir) != FR_OK)
      continue;
    while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0]) {
      char path[64];
      UINT br = 0;
      u32 id;
      int ok;
      if ((fno.fattrib & AM_DIR) || !app_id(fno.fname, &id))
        continue;
      put_str(put_str(put_str(path, fd_mods[m].dir), "/"), fno.fname);
      if (f_open(&f, path, FA_READ) != FR_OK)
        continue;
      ok = f_read(&f, ncch, 0x200, &br) == FR_OK && br == 0x200 &&
           !memcmp(h + 0x100, "NCCH", 4) &&
           rd32(h + 0x118) == fd_mods[m].tid_lo &&
           rd32(h + 0x11C) == 0x00040130;
      f_close(&f);
      if (ok && (!have || id > best)) {
        best = id;
        have = 1;
        put_str(out, path);
      }
    }
    f_closedir(&dir);
    if (have)
      return 1;
  }
  return 0;
}

/* The counter for section `type` (1 exheader, 2 ExeFS): NCCH versions 0 and 2
 * use the partition ID big-endian and the type, version 1 the partition ID as
 * stored and the section's offset. */
static void ncch_ctr(const u8 *h, int type, u8 *ctr) {
  memset(ctr, 0, 16);
  if ((h[0x112] | (h[0x113] << 8)) == 1) {
    u32 x = type == 1 ? 0x200u : rd32(h + 0x1A0) * 0x200u;
    for (int i = 0; i < 8; i++)
      ctr[i] = h[0x108 + i];
    for (int i = 0; i < 4; i++)
      ctr[12 + i] = (u8)(x >> (24 - 8 * i));
  } else {
    for (int i = 0; i < 8; i++)
      ctr[i] = h[0x10F - i];
    ctr[8] = (u8)type;
  }
}

static int read_at(FIL *f, u32 off, void *buf, u32 len) {
  UINT br = 0;
  return f_lseek(f, off) == FR_OK && f_read(f, buf, len, &br) == FR_OK &&
         br == len;
}

/* The module's .code into FD_IN, decrypted: its length and whether it is
 * packed, or 0 with the reason. */
static u32 fd_load(const char *path, int *packed, const char **why) {
  static FIL f;
  const u8 *h = (const u8 *)ncch, *e = (const u8 *)efs;
  const u8 *flags = h + 0x188;
  u32 exefs, off = 0, size = 0, rounded;
  int crypt;
  u8 ctr[16];

  *why = L(STR_FD_E_READ);
  if (f_open(&f, path, FA_READ) != FR_OK)
    return 0;
  if (!read_at(&f, 0, ncch, 0x200) || !read_at(&f, 0x200, exh, 0x400)) {
    f_close(&f);
    return 0;
  }
  exefs = rd32(h + 0x1A0) * 0x200u;
  if (!exefs || !read_at(&f, exefs, efs, 0x200)) {
    f_close(&f);
    return 0;
  }

  crypt = !(flags[7] & 0x04); /* NoCrypto */
  if (crypt && ((flags[7] & 0x21) || flags[3])) {
    /* a fixed key, a seed, or 7.x and later keyslots: not set on the ARM9 */
    *why = L(STR_FD_E_CRYPT);
    f_close(&f);
    return 0;
  }
  if (crypt) {
    aes_keyy(NCCH_SLOT, h);
    ncch_ctr(h, 1, ctr);
    aes_ctr(NCCH_SLOT, ctr, exh, 0x400);
    ncch_ctr(h, 2, ctr);
    aes_ctr(NCCH_SLOT, ctr, efs, 0x200);
  }

  for (int i = 0; i < 10; i++)
    if (!memcmp(e + i * 16, ".code\0\0\0", 8)) {
      off = rd32(e + i * 16 + 8);
      size = rd32(e + i * 16 + 12);
    }
  rounded = (size + 15u) & ~15u;
  if (!size || rounded > FD_IN_MAX) {
    *why = L(crypt ? STR_FD_E_DECRYPT : STR_FD_E_NOCODE);
    f_close(&f);
    return 0;
  }
  if (!read_at(&f, exefs + 0x200u + off, FD_IN, rounded)) {
    f_close(&f);
    return 0;
  }
  f_close(&f);
  if (crypt) {
    ncch_ctr(h, 2, ctr);
    aes_ctr_add(ctr, (0x200u + off) / 16u);
    aes_ctr(NCCH_SLOT, ctr, FD_IN, rounded);
  }
  *packed = ((const u8 *)exh)[0x0D] & 1;
  *why = 0;
  return size;
}

/* The 3DS's ExeFS code packing, undone (as ctrtool's lzss.c does). The data
 * is read from the end backwards: a footer gives where the packed part
 * starts and how much longer the result is; then each flag byte, high bit
 * first, says whether the next item is a byte or a (length, distance) copy
 * from what is already unpacked above it. Returns the unpacked size, 0 if the
 * data is not valid. */
static u32 blz_unpack(const u8 *in, u32 len, u8 *out, u32 max) {
  u32 tb, total, i, o, stop;
  if (len < 8)
    return 0;
  tb = rd32(in + len - 8);
  total = len + rd32(in + len - 4);
  if (total > max || total < len || (tb >> 24) > len ||
      (tb & 0xFFFFFFu) > len)
    return 0;
  memcpy(out, in, len);
  i = len - (tb >> 24);
  stop = len - (tb & 0xFFFFFFu);
  o = total;
  while (i > stop) {
    u8 ctl = in[--i];
    for (int b = 0; b < 8 && i > stop; b++, ctl = (u8)(ctl << 1)) {
      if (ctl & 0x80) {
        u32 seg, n, d;
        if (i < 2)
          return 0;
        i -= 2;
        seg = in[i] | (in[i + 1] << 8);
        n = ((seg >> 12) & 15u) + 3u;
        d = (seg & 0x0FFFu) + 2u;
        if (o < n)
          return 0;
        while (n--) {
          if (o + d >= total)
            return 0;
          out[o - 1] = out[o + d];
          o--;
        }
      } else {
        if (o < 1)
          return 0;
        out[--o] = in[--i];
      }
    }
  }
  return total;
}

/* The pool whose stub and database entries are load addresses too; the first
 * marked one otherwise (as tools/nwm_extract.py picks). */
static int fd_extract(const u8 *d, u32 len) {
  int pool = -1;
  const u8 *w;
  for (u32 j = 0; j + 24 * 4 <= len; j++)
    if (rd32(d + j) == POOL_MARKER && rd32(d + j + 4) == POOL_VERIFY) {
      u32 w8 = rd32(d + j + 32);
      if (pool < 0)
        pool = (int)j;
      if (w8 >= LOAD_BASE && w8 < 0x00200000u) {
        pool = (int)j;
        break;
      }
    }
  if (pool < 0)
    return 0;
  w = d + pool;
  for (int i = 0; i < 6; i++) {
    u32 s = rd32(w + fd_blobs[i].start * 4), e = rd32(w + fd_blobs[i].end * 4);
    if (s < LOAD_BASE || e <= s || e - LOAD_BASE > len ||
        e - s > fd_blobs[i].max)
      return 0;
    fd_off[i] = s - LOAD_BASE;
    fd_len[i] = e - s;
  }
  return 1;
}

static void fd_unlink_all(void) {
  char path[48];
  for (int i = 0; i < 6; i++) {
    put_str(put_str(put_str(path, FW_DIR), "/"), fd_blobs[i].name);
    f_unlink(path);
  }
}

static int fd_save(const u8 *code) {
  static FIL f;
  f_mkdir("0:/Aurora");
  f_mkdir(FW_DIR);
  for (int i = 0; i < 6; i++) {
    char path[48];
    UINT bw = 0;
    int ok;
    put_str(put_str(put_str(path, FW_DIR), "/"), fd_blobs[i].name);
    if (f_open(&f, path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) {
      fd_unlink_all();
      return 0;
    }
    ok = f_write(&f, code + fd_off[i], fd_len[i], &bw) == FR_OK &&
         bw == fd_len[i];
    ok &= f_close(&f) == FR_OK;
    if (!ok) {
      fd_unlink_all();
      return 0;
    }
  }
  return 1;
}

const char *fwdump_copy(void (*step)(int n)) {
  static FATFS sdfs, nandfs;
  static char app[64];
  const char *why = 0;
  const u8 *code = FD_IN;
  u32 len;
  int packed = 0, err;

  if (step)
    step(0);
  err = nand_open();
  if (err) {
    why = nand_error(err);
    goto done;
  }
  if (f_mount(&nandfs, "1:", 1) != FR_OK) {
    why = L(STR_FD_E_CTRNAND);
    goto done;
  }
  if (step)
    step(1);
  if (!fd_find(app)) {
    why = L(STR_FD_E_NOMOD);
    goto done;
  }
  if (step)
    step(2);
  len = fd_load(app, &packed, &why);
  if (!len)
    goto done;
  if (step)
    step(3);
  if (packed) {
    len = blz_unpack(FD_IN, len, FD_OUT, FD_OUT_MAX);
    code = FD_OUT;
  }
  if (!len) {
    why = L(STR_FD_E_UNPACK);
    goto done;
  }
  if (!fd_extract(code, len)) {
    why = L(STR_FD_E_NOFW);
    goto done;
  }
  if (step)
    step(4);
  if (f_mount(&sdfs, "0:", 1) != FR_OK || !fd_save(code))
    why = L(STR_FD_E_WRITE);

done:
  f_mount(NULL, "1:", 0);
  f_mount(NULL, "0:", 0);
  nand_close();
  return why;
}

int fwdump_present(void) {
  static FATFS fs;
  static FILINFO fno;
  int ok = f_mount(&fs, "0:", 1) == FR_OK;
  for (int i = 0; ok && i < 6; i++) {
    char path[48];
    if (!fd_blobs[i].needed)
      continue;
    put_str(put_str(put_str(path, FW_DIR), "/"), fd_blobs[i].name);
    ok = f_stat(path, &fno) == FR_OK && fno.fsize > 0;
  }
  f_mount(NULL, "0:", 0);
  return ok;
}

#define STEPS 5

static const StringId fd_step_name[STEPS] = {STR_FD_STEP1, STR_FD_STEP2,
                                             STR_FD_STEP3, STR_FD_STEP4,
                                             STR_FD_STEP5};

static const Color c_ok = {0x4C, 0xD9, 0x64};
static const Color c_bad = {0xE0, 0x40, 0x40};
static const Color c_warn = {0xF0, 0xA0, 0x30};

static void top_head(u32 icon, Color tint, const char *title) {
  volatile u8 *fb = VRAM_TOP_LA;
  ui_wallpaper(fb, TW, TSH, TSH);
  status_bar_draw();
  ui_icon(fb, (TW - 64) / 2, 30, 64, TSH, icon, 0, tint);
  ui_text_mid(fb, TW / 2, 100, TSH, title, COLOR_WHITE, COLOR_HM_BG_BOT,
              &ui_title);
}

static void top_warning(void) {
  volatile u8 *fb = VRAM_TOP_LA;
  top_head(ASSET_ICON_REPORT_64, c_warn, L(STR_FD_NAND));
  ui_text_mid_fit(fb, TW / 2, 132, TSH, L(STR_FD_ACCESS), TW - 16,
                  COLOR_WHITE, COLOR_HM_BG_BOT, &ui_bold);
  ui_text_mid(fb, TW / 2, 151, TSH, L(STR_FD_PROCEED), COLOR_WHITE,
              COLOR_HM_BG_BOT, &ui_bold);
  ui_text_mid_fit(fb, TW / 2, 176, TSH, L(STR_FD_READ_ONLY), TW - 16,
                  COLOR_HM_TEXT2, COLOR_HM_BG_BOT, &ui_font);
  ui_text_mid_fit(fb, TW / 2, 195, TSH, L(STR_FD_COPIED_TO), TW - 16,
                  COLOR_HM_TEXT2, COLOR_HM_BG_BOT, &ui_font);
  ui_text_mid_fit(fb, TW / 2, 222, TSH, L(STR_FD_ENTER), TW - 16,
                  COLOR_HM_TEXT2, COLOR_HM_BG_BOT, &ui_small);
  screen_present_top();
}

/* `at` is the step running; `failed` marks it as the one that stopped. */
static void top_steps(int at, int failed) {
  volatile u8 *fb = VRAM_TOP_LA;
  int x = 112;
  top_head(ASSET_ICON_WIFI_64, COLOR_WHITE, L(STR_FD_TITLE));
  for (int i = 0; i < STEPS; i++) {
    int y = 132 + i * 18, cy = y + ui_th(&ui_font) / 2;
    Color c = i < at ? COLOR_WHITE : COLOR_HM_TEXT2;
    if (i < at)
      ui_icon(fb, x, cy - 8, 16, TSH, ASSET_ICON_CHECK_16, 0, c_ok);
    else if (i == at && failed)
      ui_icon(fb, x, cy - 8, 16, TSH, ASSET_ICON_CROSS_16, 0, c_bad);
    else
      draw_filled_round_rect(fb, x + 5, cy - 3, 6, 6, 3, TSH,
                             i == at ? g_accent : COLOR_HM_TEXT2);
    if (i == at)
      c = failed ? c_bad : COLOR_WHITE;
    ui_text(fb, x + 26, y, TSH, L(fd_step_name[i]), c, COLOR_HM_BG_BOT,
            &ui_font);
  }
  screen_present_top();
}

#define CARD_X 12
#define CARD_Y 14
#define CARD_W (BW - 24)
#define CARD_H 182

static void bot_card(void) {
  ui_wallpaper(VRAM_BOT_A, BW, BSH, BSH);
  draw_gradient_round_rect(VRAM_BOT_A, CARD_X, CARD_Y, CARD_W, CARD_H, 14, BSH,
                           COLOR_PANEL_TOP, COLOR_PANEL_BOT);
}

static int fd_at; /* the step running, for where a failure is shown */

static void fd_step(int n) {
  volatile u8 *fb = VRAM_BOT_A;
  fd_at = n;
  int fill = (CARD_W - 48) * (n + 1) / STEPS;
  top_steps(n, 0);
  bot_card();
  ui_text_mid_fit(fb, BW / 2, CARD_Y + 30, BSH, L(STR_FD_COPYING), CARD_W - 24,
                  COLOR_WHITE, COLOR_PANEL_TOP, &ui_bold);
  ui_text_mid_fit(fb, BW / 2, CARD_Y + 56, BSH, L(fd_step_name[n]),
                  CARD_W - 24, COLOR_HM_TEXT2, COLOR_PANEL_TOP, &ui_font);
  draw_filled_round_rect(fb, CARD_X + 24, CARD_Y + 100, CARD_W - 48, 8, 4, BSH,
                         COLOR_HM_SLOT);
  draw_filled_round_rect(fb, CARD_X + 24, CARD_Y + 100, fill, 8, 4, BSH,
                         g_accent);
  ui_text_mid(fb, BW / 2, CARD_Y + 140, BSH, L(STR_FD_KEEP_ON),
              COLOR_HM_TEXT2, COLOR_PANEL_BOT, &ui_small);
  screen_present_bottom();
}

/* The code: five of these, never the same twice in a row. */
static const u32 code_key[6] = {BUTTON_DUP,   BUTTON_DDOWN, BUTTON_DLEFT,
                                BUTTON_DRIGHT, BUTTON_A,    BUTTON_B};
static const char *const code_name[6] = {0, 0, 0, 0, "A", "B"};

#define CODE_N     5
#define CHIP_W     54
#define CHIP_H     44
#define CHIP_GAP   4
#define CHIP_X(i)  ((BW - CODE_N * CHIP_W - (CODE_N - 1) * CHIP_GAP) / 2 + \
                    (i) * (CHIP_W + CHIP_GAP))
#define CHIP_Y     (CARD_Y + 58)
#define CANCEL_X   ((BW - 112) / 2)
#define CANCEL_Y   (CARD_Y + 136)
#define CANCEL_W   112
#define CANCEL_H   30

/* A D-pad direction (0 up, 1 down, 2 left, 3 right) as an arrow centred on
 * (cx, cy): a head 14 pixels across and a shaft behind it. */
static void arrow(volatile u8 *fb, int cx, int cy, int dir, Color c) {
  for (int i = 0; i < 7; i++) {
    int along = -7 + i, half = i + 1;
    if (dir == 0)
      draw_filled_rect(fb, cx - half, cy + along, 2 * half, 1, BSH, c);
    else if (dir == 1)
      draw_filled_rect(fb, cx - half, cy - along - 1, 2 * half, 1, BSH, c);
    else if (dir == 2)
      draw_filled_rect(fb, cx + along, cy - half, 1, 2 * half, BSH, c);
    else
      draw_filled_rect(fb, cx - along - 1, cy - half, 1, 2 * half, BSH, c);
  }
  if (dir < 2)
    draw_filled_rect(fb, cx - 2, dir ? cy - 7 : cy, 4, 8, BSH, c);
  else
    draw_filled_rect(fb, dir == 2 ? cx : cx - 7, cy - 2, 8, 4, BSH, c);
}

static void code_draw(const u8 *code, int got, int wrong) {
  volatile u8 *fb = VRAM_BOT_A;
  bot_card();
  ui_text_mid_fit(fb, BW / 2, CARD_Y + 18, BSH, L(STR_FD_PRESS), CARD_W - 24,
                  COLOR_WHITE, COLOR_PANEL_TOP, &ui_bold);
  for (int i = 0; i < CODE_N; i++) {
    int x = CHIP_X(i);
    Color text = i < got ? COLOR_HM_BAR : i == got ? COLOR_WHITE
                                                   : COLOR_HM_TEXT2;
    if (wrong)
      draw_filled_round_rect(fb, x, CHIP_Y, CHIP_W, CHIP_H, 10, BSH, c_bad);
    else if (i < got)
      draw_filled_round_rect(fb, x, CHIP_Y, CHIP_W, CHIP_H, 10, BSH, g_accent);
    else
      draw_gradient_round_rect(fb, x, CHIP_Y, CHIP_W, CHIP_H, 10, BSH,
                               COLOR_HM_SLOT_TOP, COLOR_HM_SLOT_BOT);
    if (i == got && !wrong)
      draw_round_ring(fb, x - 3, CHIP_Y - 3, CHIP_W + 6, CHIP_H + 6, 12, 3,
                      BSH, g_accent);
    if (code_name[code[i]])
      ui_text_mid(fb, x + CHIP_W / 2, CHIP_Y + (CHIP_H - ui_th(&ui_title)) / 2,
                  BSH, code_name[code[i]], wrong ? COLOR_WHITE : text,
                  COLOR_HM_SLOT, &ui_title);
    else
      arrow(fb, x + CHIP_W / 2, CHIP_Y + CHIP_H / 2, code[i],
            wrong ? COLOR_WHITE : text);
  }
  ui_text_mid(fb, BW / 2, CHIP_Y + CHIP_H + 12, BSH,
              L(wrong ? STR_FD_WRONG : STR_FD_SELECT_CANCEL),
              wrong ? c_bad : COLOR_HM_TEXT2, COLOR_PANEL_BOT, &ui_small);
  draw_gradient_round_rect(fb, CANCEL_X, CANCEL_Y, CANCEL_W, CANCEL_H, 9, BSH,
                           COLOR_HM_SLOT_TOP, COLOR_HM_SLOT_BOT);
  ui_text_mid(fb, BW / 2, CANCEL_Y + (CANCEL_H - ui_th(&ui_font)) / 2, BSH,
              L(STR_CANCEL), COLOR_WHITE, COLOR_HM_SLOT, &ui_font);
  screen_present_bottom();
}

static void wait_ms(u32 ms) {
  u32 t = timer_ticks();
  while (timer_us_since(t) < ms * 1000u)
    ui_idle();
}

/* 1 once the code is entered, 0 if cancelled. A wrong button starts it over. */
static int code_run(void) {
  static u32 seed;
  u8 code[CODE_N];
  int got = 0, wrong = 0, tx, ty;
  u32 wrong_at = 0, held = get_keys();

  seed ^= timer_ticks() * 2654435761u;
  if (!seed)
    seed = 0x9E3779B9u; /* xorshift never leaves 0 */
  for (int i = 0; i < CODE_N; i++) {
    do {
      seed ^= seed << 13;
      seed ^= seed >> 17;
      seed ^= seed << 5;
      code[i] = (u8)(seed % 6u);
    } while (i && code[i] == code[i - 1]);
  }
  top_warning();
  code_draw(code, 0, 0);
  while (touch_read(&tx, &ty, 0, 0))
    ui_idle();
  touch_tap(0, 0);

  for (;;) {
    u32 k = get_keys_down(), b;
    k &= ~held; /* a held direction repeats; only new presses count */
    held = get_keys();
    if (k & (BUTTON_SELECT | BUTTON_START))
      return 0;
    if (touch_tap(&tx, &ty) &&
        touch_in(tx, ty, CANCEL_X, CANCEL_Y, CANCEL_W, CANCEL_H))
      return 0;
    b = k & (BUTTON_DUP | BUTTON_DDOWN | BUTTON_DLEFT | BUTTON_DRIGHT |
             BUTTON_A | BUTTON_B | BUTTON_X | BUTTON_Y);
    if (b) {
      if (b == code_key[code[got]]) {
        got++;
        wrong = 0;
      } else {
        got = 0;
        wrong = 1;
        wrong_at = timer_ticks();
      }
      code_draw(code, got, wrong);
      if (got == CODE_N) {
        wait_ms(250);
        return 1;
      }
    }
    if (wrong && timer_us_since(wrong_at) > 700000u) {
      wrong = 0;
      code_draw(code, got, 0);
    }
    ui_idle();
  }
}

/* `s` on one line, or two broken at a space, centred at `y`. */
static void two_lines(const char *s, int y, Color c) {
  volatile u8 *fb = VRAM_BOT_A;
  char first[64];
  int maxw = CARD_W - 32, cut = -1;
  if (ui_tw(&ui_font, s) > maxw)
    for (int i = 0; s[i] && i < (int)sizeof(first) - 1; i++) {
      if (s[i] != ' ')
        continue;
      memcpy(first, s, (size_t)i);
      first[i] = 0;
      if (ui_tw(&ui_font, first) > maxw)
        break;
      cut = i;
    }
  if (cut < 0) {
    ui_text_mid_fit(fb, BW / 2, y, BSH, s, maxw, c, COLOR_PANEL_TOP, &ui_font);
    return;
  }
  memcpy(first, s, (size_t)cut);
  first[cut] = 0;
  ui_text_mid(fb, BW / 2, y, BSH, first, c, COLOR_PANEL_TOP, &ui_font);
  ui_text_mid_fit(fb, BW / 2, y + 20, BSH, s + cut + 1, maxw, c,
                  COLOR_PANEL_TOP, &ui_font);
}

static void result(const char *why) {
  volatile u8 *fb = VRAM_BOT_A;
  int tx, ty;
  top_steps(why ? fd_at : STEPS, why != 0);
  bot_card();
  ui_text_mid(fb, BW / 2, CARD_Y + 22, BSH,
              L(why ? STR_FD_FAILED : STR_FD_SAVED),
              why ? c_bad : COLOR_WHITE, COLOR_PANEL_TOP, &ui_title);
  two_lines(why ? why : L(STR_FD_FILES_IN), CARD_Y + 60, COLOR_HM_TEXT2);
  if (why)
    ui_text_mid(fb, BW / 2, CARD_Y + 110, BSH, L(STR_FD_PC_WAY),
                COLOR_HM_TEXT2, COLOR_PANEL_BOT, &ui_small);
  draw_gradient_round_rect(fb, CANCEL_X, CANCEL_Y, CANCEL_W, CANCEL_H, 9, BSH,
                           COLOR_HM_SLOT_TOP, COLOR_HM_SLOT_BOT);
  ui_text_mid(fb, BW / 2, CANCEL_Y + (CANCEL_H - ui_th(&ui_font)) / 2, BSH,
              L(STR_OK), COLOR_WHITE, COLOR_HM_SLOT, &ui_font);
  screen_present_bottom();
  touch_tap(0, 0);
  for (;;) {
    u32 k = get_keys_down();
    if ((k & (BUTTON_A | BUTTON_B)) ||
        (touch_tap(&tx, &ty) &&
         touch_in(tx, ty, CANCEL_X, CANCEL_Y, CANCEL_W, CANCEL_H)))
      break;
    ui_idle();
  }
}

/* After a key ended a screen: let it go, so the screen under does not act on
 * it too. */
static void settle(void) {
  while (get_keys() & (BUTTON_A | BUTTON_B | BUTTON_START | BUTTON_SELECT))
    ui_idle();
  get_keys_down();
}

int fwdump_run(void) {
  const char *why;
  anim_transition(ANIM_PUSH, ANIM_BOTH);
  if (!code_run()) {
    settle();
    anim_transition(ANIM_POP, ANIM_BOTH);
    return 0;
  }
  anim_transition(ANIM_FADE, ANIM_BOTH);
  why = fwdump_copy(fd_step);
  result(why);
  settle();
  anim_transition(ANIM_POP, ANIM_BOTH);
  return why == 0;
}
