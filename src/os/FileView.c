/* Text viewer (TXT, LOG) and hex editor (everything else). Both borrow the
 * image viewer's FCRAM buffers (include/image.h), which are free while no
 * picture is open. */
#include "fileview.h"
#include "assets.h"
#include "ff.h"
#include "image.h"
#include "lang.h"
#include "touch.h"
#include "ui.h"

#define TSH     TOP_SCREEN_HEIGHT
#define BSH     BOT_SCREEN_HEIGHT
#define TOP_BAR 24

static const Color ink_dark = {0x12, 0x12, 0x16};

static char *fv_u32(char *p, u32 v) {
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

static char *fv_str(char *p, const char *s) {
  while (*s)
    *p++ = *s++;
  return p;
}

static char *fv_size(char *p, u32 bytes) {
  u32 whole, frac;
  const char *unit;
  if (bytes < 1024u) {
    p = fv_u32(p, bytes);
    return fv_str(p, " B");
  }
  if (bytes < 1024u * 1024u) {
    whole = bytes / 1024u;
    frac = ((bytes % 1024u) * 10u) / 1024u;
    unit = " KB";
  } else {
    whole = bytes / (1024u * 1024u);
    frac = ((bytes % (1024u * 1024u)) / 1024u * 10u) / 1024u;
    unit = " MB";
  }
  p = fv_u32(p, whole);
  *p++ = '.';
  *p++ = (char)('0' + frac);
  return fv_str(p, unit);
}

static void fv_bar(const char *name, const char *right) {
  draw_filled_rect(VRAM_TOP_LA, 0, 0, TOP_SCREEN_WIDTH, TOP_BAR, TSH,
                   COLOR_HM_BAR);
  ui_text(VRAM_TOP_LA, 10, 4, TSH, name, COLOR_WHITE, COLOR_HM_BAR, &ui_bold);
  if (right && right[0])
    ui_text(VRAM_TOP_LA, TOP_SCREEN_WIDTH - 10 - ui_tw(&ui_small, right), 6,
            TSH, right, COLOR_HM_TEXT2, COLOR_HM_BAR, &ui_small);
}

/* Dismissed with A, B or a tap. */
static void fv_message(const char *title, const char *detail) {
  ui_dialog(VRAM_BOT_A, BOT_SCREEN_WIDTH, BSH, BSH, title, detail, g_accent, 1);
  screen_present_bottom();
  for (;;) {
    u32 k = get_keys_down();
    int tx, ty;
    if ((k & (BUTTON_A | BUTTON_B)) || touch_tap(&tx, &ty))
      return;
    ui_idle();
  }
}

/* A rounded button with a key badge on its left and the label centred in the
 * rest. The primary one is filled with the accent, so its text goes dark. */
static void fv_button(int x, int y, int w, int h, const char *label,
                      const char *key, int primary) {
  volatile u8 *fb = VRAM_BOT_A;
  Color ink = primary ? ink_dark : COLOR_WHITE;
  int bx = x + 10, by = y + (h - 18) / 2;

  if (primary)
    draw_filled_round_rect(fb, x, y, w, h, 10, BSH, g_accent);
  else
    draw_gradient_round_rect(fb, x, y, w, h, 10, BSH, COLOR_HM_SLOT_TOP,
                             COLOR_HM_SLOT_BOT);
  if (key) {
    draw_filled_round_rect(fb, bx, by, 18, 18, 9, BSH,
                           primary ? ink_dark : COLOR_HM_TEXT2);
    ui_text_mid(fb, bx + 9, by + (18 - ui_th(&ui_small)) / 2, BSH, key,
                primary ? g_accent : ink_dark, COLOR_HM_SLOT, &ui_small);
    x += 28;
    w -= 28;
  }
  ui_text_mid(fb, x + w / 2, y + (h - ui_th(&ui_bold)) / 2, BSH, label, ink,
              COLOR_HM_SLOT, &ui_bold);
}

#define CF_W     296
#define CF_H     132
#define CF_X     ((BOT_SCREEN_WIDTH - CF_W) / 2)
#define CF_Y     ((BOT_SCREEN_HEIGHT - CF_H) / 2)
#define CF_BTN_W 128
#define CF_BTN_H 38
#define CF_BTN_Y (CF_Y + CF_H - CF_BTN_H - 14)
#define CF_YES_X (CF_X + 14)
#define CF_NO_X  (CF_X + CF_W - 14 - CF_BTN_W)

int fv_confirm(const char *title, const char *subtitle, const char *yes,
               const char *no) {
  static const Color scrim = {0x00, 0x00, 0x00};
  volatile u8 *fb = VRAM_BOT_A;

  draw_filled_rect_alpha(fb, 0, 0, BOT_SCREEN_WIDTH, BSH, BSH, scrim, 150);
  draw_gradient_round_rect(fb, CF_X, CF_Y, CF_W, CF_H, 14, BSH,
                           COLOR_PANEL_TOP, COLOR_PANEL_BOT);
  ui_text_mid(fb, BOT_SCREEN_WIDTH / 2, CF_Y + 16, BSH, title, COLOR_WHITE,
              COLOR_PANEL_TOP, &ui_bold);
  if (subtitle && subtitle[0])
    ui_text_mid(fb, BOT_SCREEN_WIDTH / 2, CF_Y + 42, BSH, subtitle,
                COLOR_HM_TEXT2, COLOR_PANEL_TOP, &ui_font);
  fv_button(CF_YES_X, CF_BTN_Y, CF_BTN_W, CF_BTN_H, yes, "A", 1);
  fv_button(CF_NO_X, CF_BTN_Y, CF_BTN_W, CF_BTN_H, no, "B", 0);
  screen_present_bottom();

  for (;;) {
    u32 k = get_keys_down();
    int tx, ty;
    if (k & BUTTON_A)
      return 1;
    if (k & BUTTON_B)
      return 0;
    if (touch_tap(&tx, &ty)) {
      if (touch_in(tx, ty, CF_YES_X, CF_BTN_Y, CF_BTN_W, CF_BTN_H))
        return 1;
      if (touch_in(tx, ty, CF_NO_X, CF_BTN_Y, CF_BTN_W, CF_BTN_H))
        return 0;
    }
    ui_idle();
  }
}

/* The file is read whole, cleaned into drawable UTF-8 and wrapped once into an
 * index of line starts, so scrolling never touches the card. */

#define TV_FILE      ((const u8 *)IMAGE_FILE_ADDR)
#define TV_MAX       (2u * 1024u * 1024u) /* bytes read; cleaning at most doubles */
#define TV_TEXT      ((u8 *)IMAGE_RAW_ADDR)
#define TV_LINES     ((u32 *)IMAGE_RGB_ADDR)
#define TV_LINES_MAX (IMAGE_RGB_MAX / 4u)

#define TV_X     10
#define TV_W     (TOP_SCREEN_WIDTH - TV_X - 16) /* room for the scroll bar */
#define TV_Y     (TOP_BAR + 4)
#define TV_PITCH 17
#define TV_ROWS  12
#define TV_TAB   4 /* spaces to a tab stop */

#define TV_BTN_Y 124
#define TV_BTN_H 44
#define TV_UP_X  12
#define TV_DN_X  166
#define TV_BTN_W 142

static u32 tv_len, tv_lines;
static u8 tv_adv[256];
static int tv_tab_w;

/* Rebuild the bytes as drawable UTF-8. CRLF and a lone CR become LF; tabs and
 * newlines stay; other control bytes become '.'. A valid UTF-8 character in
 * Latin-1 is kept, one outside it becomes '?' (the fonts stop at U+00FF), and a
 * stray high byte is taken as Latin-1, which reads most Windows-1252 text. So
 * the result holds only 1- and 2-byte characters, at most twice the input. */
static void tv_clean(const u8 *in, u32 n) {
  u8 *out = TV_TEXT;
  u32 i = 0, o = 0;

  if (n >= 3 && in[0] == 0xEF && in[1] == 0xBB && in[2] == 0xBF)
    i = 3; /* byte order mark */
  while (i < n) {
    u8 c = in[i];
    if (c == '\r') {
      out[o++] = '\n';
      i += (i + 1 < n && in[i + 1] == '\n') ? 2u : 1u;
      continue;
    }
    if (c == '\n' || c == '\t') {
      out[o++] = c;
      i++;
      continue;
    }
    if (c < 0x20 || c == 0x7F) {
      out[o++] = '.';
      i++;
      continue;
    }
    if (c < 0x80) {
      out[o++] = c;
      i++;
      continue;
    }

    u32 len = ((c & 0xE0) == 0xC0) ? 2u
              : ((c & 0xF0) == 0xE0) ? 3u
              : ((c & 0xF8) == 0xF0) ? 4u : 0u;
    if (len && i + len <= n) {
      u32 k, cp = c & (0x7Fu >> len);
      for (k = 1; k < len && (in[i + k] & 0xC0) == 0x80; k++)
        cp = (cp << 6) | (in[i + k] & 0x3Fu);
      if (k == len && cp >= 0x80u) {
        if (cp >= 0xA0u && cp <= 0xFFu) {
          out[o++] = (u8)(0xC0u | (cp >> 6));
          out[o++] = (u8)(0x80u | (cp & 0x3Fu));
        } else {
          out[o++] = '?';
        }
        i += len;
        continue;
      }
    }
    if (c >= 0xA0) {
      out[o++] = (u8)(0xC0u | (c >> 6));
      out[o++] = (u8)(0x80u | (c & 0x3Fu));
    } else {
      out[o++] = '?';
    }
    i++;
  }
  tv_len = o;
}

/* Advance of every code point the cleaned text can hold, measured once. */
static void tv_metrics(void) {
  char s[3];
  for (int c = 0; c < 256; c++) {
    if (c < 0x80) {
      s[0] = (char)c;
      s[1] = 0;
    } else {
      s[0] = (char)(0xC0 | (c >> 6));
      s[1] = (char)(0x80 | (c & 0x3F));
      s[2] = 0;
    }
    tv_adv[c] = (c < 0x20) ? 0 : (u8)ui_tw(&ui_font, s);
  }
  tv_tab_w = TV_TAB * tv_adv[' '];
  if (tv_tab_w <= 0)
    tv_tab_w = 16;
}

static u32 tv_char(u32 pos, u32 *cp) {
  u8 c = TV_TEXT[pos];
  if (c < 0x80) {
    *cp = c;
    return 1;
  }
  *cp = ((c & 0x1Fu) << 6) | (TV_TEXT[pos + 1] & 0x3Fu);
  return 2;
}

static int tv_step(int x, u32 cp) {
  return (cp == '\t') ? tv_tab_w - x % tv_tab_w : tv_adv[cp & 0xFFu];
}

static int tv_measure(u32 a, u32 b) {
  int x = 0;
  while (a < b) {
    u32 cp, n = tv_char(a, &cp);
    x += tv_step(x, cp);
    a += n;
  }
  return x;
}

/* Wrap at the last space that fits, or mid-word when one word is wider than the
 * line. TV_LINES[i] is where line i starts; TV_LINES[tv_lines] is the end. */
static void tv_wrap(void) {
  u32 pos = 0, start = 0, brk = 0;
  int x = 0;

  tv_lines = 0;
  while (pos < tv_len && tv_lines < TV_LINES_MAX - 1) {
    u32 cp, n = tv_char(pos, &cp);
    int w;

    if (cp == '\n') {
      TV_LINES[tv_lines++] = start;
      pos++;
      start = pos;
      brk = 0;
      x = 0;
      continue;
    }
    w = tv_step(x, cp);
    if (x + w > TV_W && pos > start) {
      TV_LINES[tv_lines++] = start;
      if (brk > start) {
        start = brk;
        x = tv_measure(start, pos);
      } else {
        start = pos;
        x = 0;
      }
      brk = 0;
      continue; /* look at this character again on the new line */
    }
    x += w;
    pos += n;
    if (cp == ' ' || cp == '\t')
      brk = pos;
  }
  if (pos < tv_len)
    tv_len = pos; /* out of index space: the rest is not shown */
  if (start < tv_len || tv_lines == 0)
    TV_LINES[tv_lines++] = start;
  TV_LINES[tv_lines] = tv_len;
}

static void tv_flush(char *seg, int *n, int x, int y) {
  if (!*n)
    return;
  seg[*n] = 0;
  ui_text(VRAM_TOP_LA, TV_X + x, y, TSH, seg, COLOR_WHITE, COLOR_HM_BG,
          &ui_font);
  *n = 0;
}

/* One line, drawn as runs between tabs so a tab lands on its stop. */
static void tv_draw_line(int row, u32 line) {
  char seg[200];
  u32 pos = TV_LINES[line], end = TV_LINES[line + 1];
  int x = 0, seg_x = 0, n = 0;
  int y = TV_Y + row * TV_PITCH;

  while (pos < end && x <= TV_W) {
    u32 cp, len = tv_char(pos, &cp);
    if (cp == '\n')
      break;
    if (cp == '\t' || n + 3 > (int)sizeof(seg)) {
      tv_flush(seg, &n, seg_x, y);
      if (cp == '\t') {
        x += tv_step(x, cp);
        pos += len;
        seg_x = x;
        continue;
      }
      seg_x = x;
    }
    seg[n++] = (char)TV_TEXT[pos];
    if (len == 2)
      seg[n++] = (char)TV_TEXT[pos + 1];
    x += tv_adv[cp & 0xFFu];
    pos += len;
  }
  tv_flush(seg, &n, seg_x, y);
}

static void tv_draw_top(const char *name, u32 top) {
  char right[48], *p = right;
  u32 last = top + TV_ROWS < tv_lines ? top + TV_ROWS : tv_lines;

  draw_filled_rect(VRAM_TOP_LA, 0, TOP_BAR, TOP_SCREEN_WIDTH, TSH - TOP_BAR,
                   TSH, COLOR_HM_BG);
  for (u32 r = 0; r < TV_ROWS && top + r < tv_lines; r++)
    tv_draw_line((int)r, top + r);

  if (tv_lines > TV_ROWS) {
    u32 track_h = TV_ROWS * TV_PITCH;
    u32 thumb_h = track_h * TV_ROWS / tv_lines;
    if (thumb_h < 12u)
      thumb_h = 12u;
    u32 thumb_y = TV_Y + (track_h - thumb_h) * top / (tv_lines - TV_ROWS);
    draw_filled_round_rect(VRAM_TOP_LA, TOP_SCREEN_WIDTH - 9, TV_Y, 4,
                           (int)track_h, 2, TSH, COLOR_HM_SLOT);
    draw_filled_round_rect(VRAM_TOP_LA, TOP_SCREEN_WIDTH - 9, (int)thumb_y, 4,
                           (int)thumb_h, 2, TSH, g_accent);
  }

  p = fv_str(p, "Lines ");
  p = fv_u32(p, top + 1u);
  *p++ = '-';
  p = fv_u32(p, last);
  p = fv_str(p, " of ");
  p = fv_u32(p, tv_lines);
  *p = 0;
  fv_bar(name, right);
  screen_present_top();
}

static void tv_draw_bottom(const char *name, u32 size, int is_log,
                           int truncated) {
  volatile u8 *fb = VRAM_BOT_A;
  char d[64], *p = d;

  ui_wallpaper(fb, BOT_SCREEN_WIDTH, BSH, BSH);
  draw_gradient_round_rect(fb, 12, 12, BOT_SCREEN_WIDTH - 24, 96, 12, BSH,
                           COLOR_PANEL_TOP, COLOR_PANEL_BOT);
  ui_icon(fb, 22, 28, 64, BSH, ASSET_ICON_FILE_TEXT_64, 0, COLOR_WHITE);
  ui_text(fb, 96, 24, BSH, name, COLOR_WHITE, COLOR_PANEL_TOP, &ui_title);

  p = fv_str(p, is_log ? "Log file - " : "Text file - ");
  p = fv_size(p, size);
  *p = 0;
  ui_text(fb, 96, 52, BSH, d, COLOR_HM_TEXT2, COLOR_PANEL_TOP, &ui_font);
  p = fv_u32(d, tv_lines);
  p = fv_str(p, tv_lines == 1 ? " line" : " lines");
  if (truncated)
    p = fv_str(p, ", first 2 MB");
  *p = 0;
  ui_text(fb, 96, 74, BSH, d, COLOR_HM_TEXT2, COLOR_PANEL_BOT, &ui_font);

  fv_button(TV_UP_X, TV_BTN_Y, TV_BTN_W, TV_BTN_H, "Page up", "L", 0);
  fv_button(TV_DN_X, TV_BTN_Y, TV_BTN_W, TV_BTN_H, "Page down", "R", 0);
  ui_text_mid(fb, BOT_SCREEN_WIDTH / 2, 186, BSH,
              "D-Pad: scroll   X: start   Y: end", COLOR_HM_TEXT2,
              COLOR_HM_BG_BOT, &ui_small);
  ui_text_mid(fb, BOT_SCREEN_WIDTH / 2, 206, BSH, "B: close", COLOR_HM_TEXT2,
              COLOR_HM_BG_BOT, &ui_small);
  screen_present_bottom();
}

void text_view(const char *path, const char *name, u32 size) {
  static FIL f;
  UINT br = 0;
  u32 want, top = 0;
  int truncated = 0, is_log = 0;

  for (const char *s = name; *s; s++)
    if (*s == '.')
      is_log = (s[1] == 'l' || s[1] == 'L');

  if (size > 64u * 1024u) {
    ui_dialog(VRAM_BOT_A, BOT_SCREEN_WIDTH, BSH, BSH, name, L(STR_LOADING),
              g_accent, 1);
    screen_present_bottom();
  }

  if (f_open(&f, path, FA_READ) != FR_OK) {
    fv_message(name, "Could not open the file");
    return;
  }
  want = (u32)f_size(&f);
  if (want > TV_MAX) {
    want = TV_MAX;
    truncated = 1;
  }
  if (f_read(&f, (void *)TV_FILE, want, &br) != FR_OK) {
    f_close(&f);
    fv_message(name, "Could not read the file");
    return;
  }
  f_close(&f);

  tv_metrics();
  tv_clean(TV_FILE, br);
  tv_wrap();
  tv_draw_bottom(name, size, is_log, truncated);
  tv_draw_top(name, top);

  for (;;) {
    u32 k = get_keys_down();
    u32 max_top = tv_lines > TV_ROWS ? tv_lines - TV_ROWS : 0;
    u32 prev = top;
    int tx, ty;

    if ((k & BUTTON_DUP) && top > 0)
      top--;
    if ((k & BUTTON_DDOWN) && top < max_top)
      top++;
    if (touch_tap(&tx, &ty)) {
      if (touch_in(tx, ty, TV_UP_X, TV_BTN_Y, TV_BTN_W, TV_BTN_H))
        k |= BUTTON_L;
      else if (touch_in(tx, ty, TV_DN_X, TV_BTN_Y, TV_BTN_W, TV_BTN_H))
        k |= BUTTON_R;
    }
    if (k & (BUTTON_L | BUTTON_DLEFT))
      top = top > TV_ROWS ? top - TV_ROWS : 0;
    if (k & (BUTTON_R | BUTTON_DRIGHT))
      top = top + TV_ROWS < max_top ? top + TV_ROWS : max_top;
    if (k & BUTTON_X)
      top = 0;
    if (k & BUTTON_Y)
      top = max_top;

    if (top != prev)
      tv_draw_top(name, top);
    if (k & BUTTON_B)
      return;
    ui_idle();
  }
}

/* Only the 96 visible bytes are read, on each page change. Changes are kept as
 * (offset, value) pairs until saved, one seek and one byte each, so the file
 * keeps its size. */

#define HX_COLS     8
#define HX_ROWS     12
#define HX_PAGE     (HX_COLS * HX_ROWS)
#define HX_HEAD_Y   (TOP_BAR + 4)
#define HX_Y        (TOP_BAR + 22)
#define HX_PITCH    16
#define HX_OFF_X    10
#define HX_HEX_X    86
#define HX_HEX_STEP 24
#define HX_ASC_X    290
#define HX_ASC_STEP 12
#define HX_EDITS    1024

#define KP_X     12
#define KP_Y     70
#define KP_W     46
#define KP_H     30
#define KP_GAP   4
#define ACT_X    222
#define ACT_W    86
#define ACT_H    40
#define ACT_Y(i) (KP_Y + (i) * (ACT_H + 5))
#define FOOT_Y   212

static const char hx_digits[] = "0123456789ABCDEF";

static FIL hx_file;
static u32 hx_size, hx_top, hx_cur, hx_page_len;
static int hx_nib;     /* which digit of the selected byte a key sets: 0 high */
static int hx_editing; /* A toggles: the D-pad changes digits instead of moving */
static u8 hx_page[HX_PAGE];
static u32 hx_edit_off[HX_EDITS];
static u8 hx_edit_val[HX_EDITS];
static int hx_edits;

static char *fv_hex(char *p, u32 v, int digits) {
  for (int s = (digits - 1) * 4; s >= 0; s -= 4)
    *p++ = hx_digits[(v >> s) & 0xFu];
  return p;
}

static void hx_load(void) {
  UINT br = 0;
  hx_page_len = 0;
  if (f_lseek(&hx_file, hx_top) == FR_OK &&
      f_read(&hx_file, hx_page, HX_PAGE, &br) == FR_OK)
    hx_page_len = br;
}

static int hx_find(u32 off) {
  for (int i = 0; i < hx_edits; i++)
    if (hx_edit_off[i] == off)
      return i;
  return -1;
}

/* The file's own byte at `off`, which must be on the loaded page. */
static u8 hx_orig(u32 off) {
  return (off - hx_top < hx_page_len) ? hx_page[off - hx_top] : 0;
}

static u8 hx_byte(u32 off) {
  int i = hx_find(off);
  return i >= 0 ? hx_edit_val[i] : hx_orig(off);
}

/* Record a change. Setting a byte back to what the file holds drops the entry,
 * so the count shows real differences. Returns 0 when the list is full. */
static int hx_set(u32 off, u8 v) {
  int i = hx_find(off);
  u8 orig = hx_orig(off);

  if (i >= 0) {
    if (v == orig) {
      hx_edits--;
      hx_edit_off[i] = hx_edit_off[hx_edits];
      hx_edit_val[i] = hx_edit_val[hx_edits];
    } else {
      hx_edit_val[i] = v;
    }
    return 1;
  }
  if (v == orig)
    return 1;
  if (hx_edits >= HX_EDITS)
    return 0;
  hx_edit_off[hx_edits] = off;
  hx_edit_val[hx_edits] = v;
  hx_edits++;
  return 1;
}

static void hx_draw_head(const char *name) {
  volatile u8 *fb = VRAM_TOP_LA;
  char s[48], *p = s;

  draw_filled_rect(fb, 0, TOP_BAR, TOP_SCREEN_WIDTH, HX_Y - TOP_BAR, TSH,
                   COLOR_HM_BG);
  ui_text(fb, HX_OFF_X, HX_HEAD_Y, TSH, "Offset", COLOR_HM_TEXT2, COLOR_HM_BG,
          &ui_small);
  for (int c = 0; c < HX_COLS; c++) {
    char h[3] = {'+', hx_digits[c], 0};
    ui_text(fb, HX_HEX_X + c * HX_HEX_STEP, HX_HEAD_Y, TSH, h, COLOR_HM_TEXT2,
            COLOR_HM_BG, &ui_small);
  }
  ui_text(fb, HX_ASC_X, HX_HEAD_Y, TSH, "ASCII", COLOR_HM_TEXT2, COLOR_HM_BG,
          &ui_small);

  p = fv_size(p, hx_size);
  if (hx_edits) {
    p = fv_str(p, "   ");
    p = fv_u32(p, (u32)hx_edits);
    p = fv_str(p, hx_edits == 1 ? " change" : " changes");
  }
  *p = 0;
  fv_bar(name, s);
}

static void hx_draw_row(int r) {
  volatile u8 *fb = VRAM_TOP_LA;
  u32 off = hx_top + (u32)r * HX_COLS;
  int y = HX_Y + r * HX_PITCH;
  int sy = ui_have(&ui_small)
               ? y + text_ascent(&ui_font) - text_ascent(&ui_small)
               : y;
  char s[12], *p;

  draw_filled_rect(fb, 0, y, TOP_SCREEN_WIDTH, HX_PITCH, TSH, COLOR_HM_BG);
  if (off >= hx_size)
    return;
  p = fv_hex(s, off, 8);
  *p = 0;
  ui_text(fb, HX_OFF_X, sy, TSH, s, COLOR_HM_TEXT2, COLOR_HM_BG, &ui_small);

  for (int c = 0; c < HX_COLS && off + (u32)c < hx_size; c++) {
    u32 o = off + (u32)c;
    u8 v = hx_byte(o);
    int edited = hx_find(o) >= 0, sel = (o == hx_cur);
    int printable = (v >= 0x20 && v < 0x7F);
    int hx = HX_HEX_X + c * HX_HEX_STEP, ax = HX_ASC_X + c * HX_ASC_STEP;
    Color ink = sel ? ink_dark : (edited ? g_accent : COLOR_WHITE);
    char pair[3] = {hx_digits[v >> 4], hx_digits[v & 0xF], 0};
    char one[2] = {printable ? (char)v : '.', 0};

    if (sel) {
      draw_filled_round_rect(fb, hx - 4, y, 25, HX_PITCH, 4, TSH,
                             hx_editing ? COLOR_WHITE : g_accent);
      draw_filled_round_rect(fb, ax - 2, y, HX_ASC_STEP, HX_PITCH, 3, TSH,
                             COLOR_HM_SLOT);
    }
    ui_text(fb, hx, y, TSH, pair, ink, COLOR_HM_BG, &ui_font);
    if (sel) { /* underline the digit the next key sets */
      char d0[2] = {pair[0], 0};
      int dx = hx_nib ? ui_tw(&ui_font, d0) : 0;
      int dw = ui_tw(&ui_font, hx_nib ? pair + 1 : d0);
      draw_filled_rect(fb, hx + dx, y + 14, dw, 2, TSH, ink_dark);
    }
    ui_text_mid(fb, ax + HX_ASC_STEP / 2 - 2, y, TSH, one,
                !printable ? COLOR_HM_TEXT2
                           : (edited ? g_accent : COLOR_WHITE),
                COLOR_HM_BG, &ui_font);
  }
}

static void hx_draw_top(const char *name) {
  hx_draw_head(name);
  for (int r = 0; r < HX_ROWS; r++)
    hx_draw_row(r);
  screen_present_top();
}

static void hx_draw_info(void) {
  volatile u8 *fb = VRAM_BOT_A;
  char s[48], *p;
  u8 v = hx_byte(hx_cur);

  ui_patch_round_rect(fb, 12, 8, BOT_SCREEN_WIDTH - 24, 54, 12, 0, BSH);
  draw_gradient_round_rect(fb, 12, 8, BOT_SCREEN_WIDTH - 24, 54, 12, BSH,
                           COLOR_PANEL_TOP, COLOR_PANEL_BOT);
  p = fv_str(s, "Offset 0x");
  p = fv_hex(p, hx_cur, 8);
  *p = 0;
  ui_text(fb, 26, 14, BSH, s, COLOR_WHITE, COLOR_PANEL_TOP, &ui_bold);
  if (hx_editing)
    ui_text(fb, BOT_SCREEN_WIDTH - 26 - ui_tw(&ui_bold, "Editing"), 14, BSH,
            "Editing", g_accent, COLOR_PANEL_TOP, &ui_bold);

  p = fv_str(s, "Value 0x");
  p = fv_hex(p, v, 2);
  p = fv_str(p, "   ");
  p = fv_u32(p, v);
  if (v >= 0x20 && v < 0x7F) {
    p = fv_str(p, "   ");
    *p++ = (char)39; /* a quote mark */
    *p++ = (char)v;
    *p++ = (char)39;
  }
  *p = 0;
  ui_text(fb, 26, 36, BSH, s, COLOR_HM_TEXT2, COLOR_PANEL_BOT, &ui_font);
}

static void hx_draw_actions(void) {
  static const char *const label[3] = {"Save", "Revert", "Close"};
  for (int i = 0; i < 3; i++) {
    ui_patch_round_rect(VRAM_BOT_A, ACT_X, ACT_Y(i), ACT_W, ACT_H, 10, 0, BSH);
    fv_button(ACT_X, ACT_Y(i), ACT_W, ACT_H, label[i], 0, i == 0 && hx_edits);
  }
}

static void hx_draw_footer(void) {
  ui_wallpaper_rect(VRAM_BOT_A, 0, FOOT_Y - 2, BOT_SCREEN_WIDTH, 20, BSH);
  ui_text_mid(VRAM_BOT_A, BOT_SCREEN_WIDTH / 2, FOOT_Y, BSH,
              hx_editing ? "Up/Down: change   Left/Right: digit   A/B: done"
                         : "A: edit   L/R: page   START: save   B: close",
              COLOR_HM_TEXT2, COLOR_HM_BG_BOT, &ui_small);
}

static void hx_draw_bottom(void) {
  volatile u8 *fb = VRAM_BOT_A;

  ui_wallpaper(fb, BOT_SCREEN_WIDTH, BSH, BSH);
  hx_draw_info();
  for (int i = 0; i < 16; i++) {
    char lab[2] = {hx_digits[i], 0};
    fv_button(KP_X + (i % 4) * (KP_W + KP_GAP),
              KP_Y + (i / 4) * (KP_H + KP_GAP), KP_W, KP_H, lab, 0, 0);
  }
  hx_draw_actions();
  hx_draw_footer();
  screen_present_bottom();
}

/* Repaints what a cursor, digit or change-count update touches: at most two
 * rows, the info card and the buttons. */
static void hx_refresh(const char *name, u32 old_cur, int count_changed) {
  if (count_changed)
    hx_draw_head(name);
  if (old_cur != hx_cur)
    hx_draw_row((int)((old_cur - hx_top) / HX_COLS));
  hx_draw_row((int)((hx_cur - hx_top) / HX_COLS));
  screen_present_top();
  hx_draw_info();
  if (count_changed)
    hx_draw_actions();
  screen_present_bottom();
}

/* Put the cursor on `to`, scrolling by whole rows only as far as it needs. */
static void hx_move(const char *name, u32 to, int nib) {
  u32 old = hx_cur, old_top = hx_top;

  if (to >= hx_size)
    to = hx_size - 1u;
  hx_cur = to;
  hx_nib = nib;
  if (hx_cur < hx_top)
    hx_top = hx_cur - hx_cur % HX_COLS;
  else if (hx_cur >= hx_top + HX_PAGE)
    hx_top = hx_cur - hx_cur % HX_COLS - (HX_PAGE - HX_COLS);

  if (hx_top != old_top) {
    hx_load();
    hx_draw_top(name);
    hx_draw_info();
    screen_present_bottom();
  } else {
    hx_refresh(name, old, 0);
  }
}

/* A page at a time: the view and the cursor both move by a screenful. */
static void hx_page_by(const char *name, int dir) {
  u32 last_row = (hx_size - 1u) / HX_COLS * HX_COLS;
  u32 max_top =
      last_row >= HX_PAGE - HX_COLS ? last_row - (HX_PAGE - HX_COLS) : 0;
  u32 top = hx_top, cur = hx_cur;

  if (dir > 0) {
    top = top + HX_PAGE < max_top ? top + HX_PAGE : max_top;
    cur = cur + HX_PAGE < hx_size ? cur + HX_PAGE : hx_size - 1u;
  } else {
    top = top > HX_PAGE ? top - HX_PAGE : 0;
    cur = cur > HX_PAGE ? cur - HX_PAGE : 0;
  }
  if (cur < top)
    cur = top;
  if (cur >= top + HX_PAGE)
    cur = top + HX_PAGE - 1u;
  if (top == hx_top) {
    hx_move(name, cur, 0);
    return;
  }
  hx_top = top;
  hx_cur = cur;
  hx_nib = 0;
  hx_load();
  hx_draw_top(name);
  hx_draw_info();
  screen_present_bottom();
}

/* Set one digit of the selected byte. Typing (`advance`) moves on to the next
 * digit and then the next byte; the D-pad's up and down stay put. */
static void hx_set_digit(const char *name, u32 digit, int advance) {
  u8 v = hx_byte(hx_cur);
  int before = hx_edits;

  v = hx_nib ? (u8)((v & 0xF0u) | digit) : (u8)((v & 0x0Fu) | (digit << 4));
  if (!hx_set(hx_cur, v)) {
    fv_message("Too many changes", "Save, then keep editing");
    hx_draw_bottom();
    return;
  }
  if (advance && hx_nib && hx_cur + 1u < hx_size) {
    u32 old = hx_cur;
    if (before != hx_edits)
      hx_draw_head(name);
    hx_draw_row((int)((old - hx_top) / HX_COLS)); /* the finished byte */
    hx_move(name, hx_cur + 1u, 0);
    if (before != hx_edits) {
      hx_draw_actions();
      screen_present_bottom();
    }
    return;
  }
  if (advance)
    hx_nib = 1;
  hx_refresh(name, hx_cur, before != hx_edits);
}

static int hx_save(const char *path) {
  static FIL w;
  UINT bw;
  int ok = 0;

  f_close(&hx_file);
  if (f_open(&w, path, FA_READ | FA_WRITE) == FR_OK) {
    ok = 1;
    for (int i = 0; i < hx_edits && ok; i++)
      if (f_lseek(&w, hx_edit_off[i]) != FR_OK ||
          f_write(&w, &hx_edit_val[i], 1, &bw) != FR_OK || bw != 1)
        ok = 0;
    if (f_close(&w) != FR_OK)
      ok = 0;
  }
  f_open(&hx_file, path, FA_READ);
  if (ok)
    hx_edits = 0;
  hx_load();
  return ok;
}

static void hx_count_text(char *s, const char *lead, const char *tail) {
  char *p = fv_str(s, lead);
  p = fv_u32(p, (u32)hx_edits);
  p = fv_str(p, hx_edits == 1 ? " byte" : " bytes");
  p = fv_str(p, tail);
  *p = 0;
}

void hex_edit(const char *path, const char *name, u32 size) {
  (void)size; /* the open file's own size is used, in case the list is stale */

  if (f_open(&hx_file, path, FA_READ) != FR_OK) {
    fv_message(name, "Could not open the file");
    return;
  }
  hx_size = (u32)f_size(&hx_file);
  if (!hx_size) {
    f_close(&hx_file);
    fv_message(name, "The file is empty");
    return;
  }
  hx_top = hx_cur = 0;
  hx_nib = hx_editing = 0;
  hx_edits = 0;
  hx_load();
  hx_draw_top(name);
  hx_draw_bottom();

  for (;;) {
    u32 k = get_keys_down();
    int tx, ty, close = 0;
    char s[48];

    if (touch_tap(&tx, &ty)) {
      for (int i = 0; i < 16; i++)
        if (touch_in(tx, ty, KP_X + (i % 4) * (KP_W + KP_GAP),
                     KP_Y + (i / 4) * (KP_H + KP_GAP), KP_W, KP_H))
          hx_set_digit(name, (u32)i, 1);
      if (touch_in(tx, ty, ACT_X, ACT_Y(0), ACT_W, ACT_H))
        k |= BUTTON_START;
      if (touch_in(tx, ty, ACT_X, ACT_Y(1), ACT_W, ACT_H))
        k |= BUTTON_SELECT;
      if (touch_in(tx, ty, ACT_X, ACT_Y(2), ACT_W, ACT_H))
        close = 1;
    }

    if (hx_editing) {
      u8 v = hx_byte(hx_cur);
      u32 d = hx_nib ? (v & 0xFu) : (u32)(v >> 4);
      if (k & BUTTON_DUP)
        hx_set_digit(name, (d + 1u) & 0xFu, 0);
      if (k & BUTTON_DDOWN)
        hx_set_digit(name, (d + 15u) & 0xFu, 0);
      if (k & BUTTON_DLEFT) {
        if (hx_nib)
          hx_move(name, hx_cur, 0);
        else if (hx_cur > 0)
          hx_move(name, hx_cur - 1u, 1);
      }
      if (k & BUTTON_DRIGHT) {
        if (!hx_nib)
          hx_move(name, hx_cur, 1);
        else if (hx_cur + 1u < hx_size)
          hx_move(name, hx_cur + 1u, 0);
      }
      if (k & (BUTTON_A | BUTTON_B)) {
        hx_editing = 0;
        hx_draw_footer();
        hx_refresh(name, hx_cur, 0);
        k &= ~(u32)BUTTON_B; /* leaving edit mode is all B does here */
      }
    } else {
      if ((k & BUTTON_DLEFT) && hx_cur > 0)
        hx_move(name, hx_cur - 1u, 0);
      if ((k & BUTTON_DRIGHT) && hx_cur + 1u < hx_size)
        hx_move(name, hx_cur + 1u, 0);
      if ((k & BUTTON_DUP) && hx_cur >= HX_COLS)
        hx_move(name, hx_cur - HX_COLS, 0);
      if ((k & BUTTON_DDOWN) && hx_cur + HX_COLS < hx_size)
        hx_move(name, hx_cur + HX_COLS, 0);
      if (k & BUTTON_L)
        hx_page_by(name, -1);
      if (k & BUTTON_R)
        hx_page_by(name, 1);
      if (k & BUTTON_A) {
        hx_editing = 1;
        hx_draw_footer();
        hx_refresh(name, hx_cur, 0);
      }
    }

    if (k & BUTTON_START) {
      if (!hx_edits) {
        fv_message("No changes", "There is nothing to save");
      } else {
        hx_count_text(s, "Write ", " to the card");
        if (fv_confirm("Save changes?", s, "Save", "Cancel")) {
          int ok = hx_save(path);
          hx_draw_top(name);
          fv_message(ok ? "Saved" : "Could not save",
                     ok ? name : "The card may be locked or full");
        }
      }
      hx_draw_bottom();
    }

    if ((k & BUTTON_SELECT) && hx_edits) {
      hx_count_text(s, "Undo ", " of changes");
      if (fv_confirm("Revert changes?", s, "Revert", "Keep")) {
        hx_edits = 0;
        hx_draw_top(name);
      }
      hx_draw_bottom();
    }

    if ((k & BUTTON_B) || close) {
      if (hx_edits) {
        hx_count_text(s, "", " not saved");
        if (!fv_confirm("Discard changes?", s, "Discard", "Keep")) {
          hx_draw_bottom();
          ui_idle();
          continue;
        }
      }
      f_close(&hx_file);
      return;
    }
    ui_idle();
  }
}
