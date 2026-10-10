/* The terminal: output on the top screen in the 8x8 font, so columns line up,
 * and a touch keyboard on the bottom one. Command lines are split here, with
 * quotes, escapes and wildcards, and run by TermCmds.c. */

#include "terminal.h"
#include "term.h"
#include "anim.h"
#include "container.h"
#include "files.h"
#include "font.h"
#include "model.h"
#include "timer.h"
#include "touch.h"
#include "ui.h"
#include <string.h>

#define T_ROWS 23
#define T_LH   10 /* the 8x8 glyphs and two rows of spacing */
#define T_X0   4
#define T_Y0   5
#define T_KEEP 320 /* lines of scrollback */
#define T_HIST 32
#define T_ARGS 64
#define T_LIVE (T_INPUT + 128) /* prompt and command line */
#define T_NAMES 49152

static const Color t_bg = {0x0C, 0x0C, 0x10};
static const Color t_pal[TC_COUNT] = {
    [TC_TEXT] = {0xD8, 0xD8, 0xD8},  [TC_DIM] = {0x84, 0x84, 0x8C},
    [TC_DIR] = {0x6C, 0xA8, 0xFF},   [TC_EXEC] = {0x7C, 0xE0, 0x7C},
    [TC_IMAGE] = {0xE0, 0x8C, 0xE0}, [TC_AUDIO] = {0x5C, 0xD8, 0xE0},
    [TC_ERR] = {0xFF, 0x6C, 0x6C},   [TC_WARN] = {0xF0, 0xC8, 0x50},
};

typedef struct {
  char ch[T_COLS];
  u8 co[T_COLS];
} TLine;

static TLine t_buf[T_KEEP];
static u32 t_done; /* finished lines; the one being written follows them */
static int t_x;
static int t_scroll; /* rows scrolled back from the newest */
static int t_running; /* a command has the screen, so no prompt */
static const char *t_status;
static int t_cursor_on = 1;
static u32 t_blink;
static int t_dirty;

static char lv_ch[T_LIVE];
static u8 lv_co[T_LIVE];
static int lv_n;

char t_cwd[T_PATH] = "0:/";
char t_oldcwd[T_PATH];
const char *t_user;
const char *t_host;
void (*t_launch)(const char *path);
int t_quit;

static char in_buf[T_INPUT];
static int in_len, in_pos;

static char t_hist[T_HIST][T_INPUT];
static int t_hist_n, t_hist_at;
static char t_draft[T_INPUT];

static FATFS t_fs;
static int t_sd;
static int t_started;

TEnt t_ents[T_ENTS];
int t_ents_cut;
static char t_names[T_NAMES];

char t_fold(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

int t_streq(const char *a, const char *b) {
  while (*a && *a == *b) {
    a++;
    b++;
  }
  return *a == *b;
}

int t_icmp(const char *a, const char *b) {
  while (*a && t_fold(*a) == t_fold(*b)) {
    a++;
    b++;
  }
  return (int)(u8)t_fold(*a) - (int)(u8)t_fold(*b);
}

char *t_cpy(char *dst, const char *src) {
  while (*src)
    *dst++ = *src++;
  *dst = 0;
  return dst;
}

static int cont(char c) { return ((u8)c & 0xC0u) == 0x80u; }

int t_width(const char *s) {
  int n = 0;
  for (; *s; s++)
    if (!cont(*s))
      n++;
  return n;
}

static Color t_rgb(int c) { return c == TC_USER ? g_accent : t_pal[c]; }

static void t_newline(void) {
  t_done++;
  memset(&t_buf[t_done % T_KEEP], 0, sizeof(TLine));
  t_x = 0;
}

/* A UTF-8 character takes one cell, shown as '?': the 8x8 font is ASCII. */
void t_outc(char c, int color) {
  u8 b = (u8)c;
  if (c == '\n') {
    t_newline();
    return;
  }
  if (c == '\t') {
    do
      t_outc(' ', color);
    while (t_x % 8);
    return;
  }
  if (cont(c))
    return;
  if (b < 0x20u || b > 0x7Eu)
    c = '?';
  if (t_x >= T_COLS)
    t_newline();
  TLine *l = &t_buf[t_done % T_KEEP];
  l->ch[t_x] = c;
  l->co[t_x] = (u8)color;
  t_x++;
}

void t_out(const char *s, int color) {
  while (*s)
    t_outc(*s++, color);
}

void t_line(const char *s, int color) {
  t_out(s, color);
  t_outc('\n', color);
}

void t_u32(u32 v, int color) {
  char tmp[12];
  int n = 0;
  do {
    tmp[n++] = (char)('0' + v % 10u);
    v /= 10u;
  } while (v);
  while (n--)
    t_outc(tmp[n], color);
}

void t_pad(const char *s, int width, int color) {
  t_out(s, color);
  for (int w = t_width(s); w < width; w++)
    t_outc(' ', color);
}

void t_wrap(const char *s, int indent, int color) {
  while (*s) {
    const char *e = s;
    int w = 0;
    while (*e && *e != ' ') {
      if (!cont(*e))
        w++;
      e++;
    }
    if (t_x > indent && t_x + w > T_COLS) {
      t_newline();
      while (t_x < indent)
        t_outc(' ', color);
    }
    while (s < e)
      t_outc(*s++, color);
    if (*s == ' ') {
      if (t_x < T_COLS)
        t_outc(' ', color);
      s++;
    }
  }
  t_outc('\n', color);
}

void t_columns(const char *const *names, const u8 *colors, int n) {
  int w = 0, cols, rows;
  for (int i = 0; i < n; i++) {
    int k = t_width(names[i]);
    if (k > w)
      w = k;
  }
  cols = (T_COLS + 2) / (w + 2);
  if (cols < 1)
    cols = 1;
  rows = (n + cols - 1) / cols;
  for (int r = 0; r < rows; r++) {
    for (int c = 0; c < cols; c++) {
      int i = c * rows + r;
      if (i >= n)
        break;
      if (i + rows < n)
        t_pad(names[i], w + 2, colors[i]);
      else
        t_out(names[i], colors[i]);
    }
    t_outc('\n', TC_TEXT);
  }
}

void t_err(const char *cmd, const char *pre, const char *what,
           const char *msg) {
  t_out(cmd, TC_ERR);
  t_out(": ", TC_ERR);
  if (pre) {
    t_out(pre, TC_ERR);
    if (what)
      t_outc(' ', TC_ERR);
  }
  if (what) {
    t_outc('\'', TC_ERR);
    t_out(what, TC_ERR);
    t_outc('\'', TC_ERR);
  }
  if (msg) {
    if (pre || what)
      t_out(": ", TC_ERR);
    t_out(msg, TC_ERR);
  }
  t_outc('\n', TC_ERR);
}

void t_clear(void) {
  memset(t_buf, 0, sizeof(t_buf));
  t_done = 0;
  t_x = 0;
  t_scroll = 0;
}

/* "user@host:/path$ ", through `put` so it can go to the scrollback or the
 * live line. A deep path keeps only its last folder. */
static void t_prompt(void (*put)(const char *, int)) {
  const char *p = t_show(t_cwd);
  put(t_user, TC_USER);
  put("@", TC_USER);
  put(t_host, TC_USER);
  put(":", TC_TEXT);
  if (t_width(p) > 24) {
    const char *last = p;
    for (const char *s = p; *s; s++)
      if (*s == '/')
        last = s;
    put("...", TC_DIR);
    p = last;
  }
  put(p, TC_DIR);
  put("$ ", TC_TEXT);
}

static void lv_put(const char *s, int color) {
  for (; *s && lv_n < T_LIVE; s++) {
    u8 b = (u8)*s;
    if (cont(*s))
      continue;
    lv_ch[lv_n] = (b < 0x20u || b > 0x7Eu) ? '?' : *s;
    lv_co[lv_n++] = (u8)color;
  }
}

static void t_cell(volatile u8 *fb, int row, int col, char c, Color fg) {
  if (c && c != ' ')
    draw_char(fb, T_X0 + col * FONT_WIDTH, T_Y0 + row * T_LH + 1,
              TOP_SCREEN_HEIGHT, c, fg, t_bg);
}

/* The scrollback, then the prompt and command line (or a job's status), the
 * newest rows at the bottom unless scrolled back. */
static void t_render(void) {
  volatile u8 *fb = VRAM_TOP_LA;
  const int sh = TOP_SCREEN_HEIGHT;
  u32 keep = t_done < T_KEEP - 1 ? t_done : T_KEEP - 1;
  u32 lo = t_done - keep, total, first;
  int partial = t_x > 0, cur = -1, rows = 0, most;

  lv_n = 0;
  if (t_status) {
    lv_put(t_status, TC_DIM);
  } else if (!t_running) {
    t_prompt(lv_put);
    cur = lv_n;
    for (int i = 0; i < in_pos; i++)
      if (!cont(in_buf[i]))
        cur++;
    lv_put(in_buf, TC_TEXT);
  }
  if (lv_n || cur >= 0) {
    int cells = cur >= lv_n ? cur + 1 : lv_n;
    rows = (cells + T_COLS - 1) / T_COLS;
  }

  total = keep + (u32)partial + (u32)rows;
  most = total > T_ROWS ? (int)(total - T_ROWS) : 0;
  if (t_scroll > most)
    t_scroll = most;
  first = (u32)(most - t_scroll);

  clear_screen(fb, TOP_FB_SIZE, t_bg);
  for (int r = 0; r < T_ROWS && first + (u32)r < total; r++) {
    u32 v = first + (u32)r;
    if (v < keep + (u32)partial) {
      const TLine *l = &t_buf[(lo + v) % T_KEEP];
      for (int c = 0; c < T_COLS; c++)
        t_cell(fb, r, c, l->ch[c], t_rgb(l->co[c]));
      continue;
    }
    int k = (int)(v - keep - (u32)partial) * T_COLS;
    for (int c = 0; c < T_COLS; c++) {
      int i = k + c;
      if (i == cur && t_cursor_on) {
        int x = T_X0 + c * FONT_WIDTH, y = T_Y0 + r * T_LH;
        draw_filled_rect(fb, x, y, FONT_WIDTH, T_LH, sh, t_pal[TC_TEXT]);
        if (i < lv_n && lv_ch[i] != ' ')
          draw_char(fb, x, y + 1, sh, lv_ch[i], t_bg, t_pal[TC_TEXT]);
      } else if (i < lv_n) {
        t_cell(fb, r, c, lv_ch[i], t_rgb(lv_co[i]));
      }
    }
  }

  if (t_scroll) {
    int th = (int)((u32)T_ROWS * (u32)sh / total);
    if (th < 8)
      th = 8;
    draw_filled_rect(fb, TOP_SCREEN_WIDTH - 3,
                     (int)first * (sh - th) / most, 2, th, sh,
                     t_pal[TC_DIM]);
  }
  screen_present_top();
  t_dirty = 0;
}

int t_busy(const char *s) {
  static u32 mark;
  if (get_keys() & BUTTON_SELECT)
    return 0;
  if (timer_calibrated() && timer_us_since(mark) < 150000u)
    return 1;
  mark = timer_ticks();
  t_status = s;
  t_render();
  t_status = 0;
  return 1;
}

void t_flush(void) {
  t_render();
  ui_idle();
}

int t_need_sd(const char *cmd) {
  if (!t_sd) {
    t_sd = f_mount(&t_fs, "", 1) == FR_OK;
    if (!t_sd)
      f_mount(NULL, "", 0);
  }
  if (!t_sd && cmd)
    t_err(cmd, 0, 0, "no SD card");
  return t_sd;
}

void t_unmount(void) {
  if (t_sd)
    f_mount(NULL, "", 0);
  t_sd = 0;
}

const char *t_show(const char *path) { return path + 2; }

int t_is_root(const char *path) { return path[2] == '/' && !path[3]; }

int t_resolve(const char *arg, char *out) {
  const char *s = arg;
  int n;

  if (s[0] == '~' && (!s[1] || s[1] == '/')) {
    s++;
    n = 3;
  } else if (s[0] == '/') {
    n = 3;
  } else {
    n = (int)strlen(t_cwd);
    memcpy(out, t_cwd, (u32)n);
  }
  memcpy(out, "0:/", 3);
  out[n] = 0;

  while (*s) {
    const char *e;
    int len;
    while (*s == '/')
      s++;
    if (!*s)
      break;
    for (e = s; *e && *e != '/'; e++)
      ;
    len = (int)(e - s);
    if (len == 2 && s[0] == '.' && s[1] == '.') {
      while (n > 3 && out[n - 1] != '/')
        n--;
      if (n > 3)
        n--;
      out[n] = 0;
    } else if (len != 1 || s[0] != '.') {
      if (n + 1 + len >= T_PATH)
        return 0;
      if (n > 3)
        out[n++] = '/';
      memcpy(out + n, s, (u32)len);
      n += len;
      out[n] = 0;
    }
    s = e;
  }
  return 1;
}

int t_join(char *out, const char *dir, const char *name) {
  int n = 0;
  for (; dir[n]; n++) {
    if (n >= T_PATH - 1)
      return 0;
    out[n] = dir[n];
  }
  if (n && out[n - 1] != '/') {
    if (n >= T_PATH - 1)
      return 0;
    out[n++] = '/';
  }
  for (int i = 0; name[i]; i++) {
    if (n >= T_PATH - 1)
      return 0;
    out[n++] = name[i];
  }
  out[n] = 0;
  return 1;
}

FRESULT t_stat(const char *path, FILINFO *fno) {
  FRESULT r;
  if (t_is_root(path)) {
    memset(fno, 0, sizeof(*fno));
    fno->fattrib = AM_DIR;
    return FR_OK;
  }
  r = f_stat(path, fno);
  /* A wildcard that matched nothing reaches here as itself, and FAT has no
   * names with * or ? in them. */
  if (r == FR_INVALID_NAME && (strchr(path, '*') || strchr(path, '?')))
    r = FR_NO_FILE;
  return r;
}

const char *t_frerr(FRESULT r) {
  switch (r) {
    case FR_NO_FILE:
    case FR_NO_PATH:         return "No such file or directory";
    case FR_EXIST:           return "File exists";
    case FR_DENIED:          return "Permission denied";
    case FR_WRITE_PROTECTED: return "Read-only file system";
    case FR_INVALID_NAME:    return "Invalid argument";
    case FR_NOT_ENABLED:
    case FR_NO_FILESYSTEM:   return "No medium found";
    case FR_LOCKED:          return "Device or resource busy";
    default:                 return "Input/output error";
  }
}

const char *t_foerr(FoResult r) {
  switch (r) {
    case FO_EXISTS:    return "File exists";
    case FO_INTO_SELF: return "Invalid argument";
    case FO_TOO_DEEP:  return "File name too long";
    case FO_FULL:      return "No space left on device";
    case FO_GONE:      return "No such file or directory";
    case FO_BAD_NAME:  return "Invalid argument";
    case FO_DENIED:    return "Permission denied";
    case FO_CANCELLED: return "Interrupted";
    default:           return "Input/output error";
  }
}

int t_container(const char *path) {
  static FIL f;
  char m[4];
  UINT br = 0;
  int ok;

  if (f_open(&f, path, FA_READ) != FR_OK)
    return 0;
  ok = f_read(&f, m, 4, &br) == FR_OK && br == 4;
  f_close(&f);
  return ok ? aurora_magic_kind(m) : 0;
}

int t_readdir(const char *dir, int all, int kinds, FRESULT *fr) {
  static DIR d;
  static FILINFO fno;
  static char path[T_PATH];
  u32 used = 0;
  int n = 0;
  FRESULT r = f_opendir(&d, dir);

  t_ents_cut = 0;
  if (r != FR_OK) {
    if (fr)
      *fr = r;
    return -1;
  }
  while (f_readdir(&d, &fno) == FR_OK && fno.fname[0]) {
    u32 len = (u32)strlen(fno.fname) + 1u;
    if (!all && ((fno.fattrib & (AM_HID | AM_SYS)) || fno.fname[0] == '.'))
      continue;
    if (n >= T_ENTS || used + len > T_NAMES) {
      t_ents_cut = 1;
      break;
    }
    TEnt *e = &t_ents[n++];
    memcpy(t_names + used, fno.fname, len);
    e->name = t_names + used;
    used += len;
    e->size = (u32)fno.fsize;
    e->date = fno.fdate;
    e->time = fno.ftime;
    e->attr = fno.fattrib;
    e->kind = (u8)files_kind(e->name, (fno.fattrib & AM_DIR) != 0);
  }
  f_closedir(&d);

  for (int i = 0; kinds && i < n; i++)
    if (t_ents[i].kind == FKIND_BIN && t_join(path, dir, t_ents[i].name) &&
        t_container(path))
      t_ents[i].kind = FKIND_AURORA;

  for (int gap = n / 2; gap > 0; gap /= 2)
    for (int i = gap; i < n; i++) {
      TEnt e = t_ents[i];
      int j = i;
      while (j >= gap && t_icmp(t_ents[j - gap].name, e.name) > 0) {
        t_ents[j] = t_ents[j - gap];
        j -= gap;
      }
      t_ents[j] = e;
    }
  return n;
}

#define KB_Y0  32
#define KB_X0  6
#define KB_KW  28
#define KB_KH  36
#define KB_GX  3
#define KB_GY  4
#define KB_BTN 48 /* PgUp and PgDn in the bar */

/* 0..39 are the four rows of ten; the rest are named. */
enum {
  K_SHIFT = 40,
  K_SYM,
  K_TAB,
  K_SPACE,
  K_DEL,
  K_ENTER,
  K_PGUP,
  K_PGDN,
  K_COUNT
};

static const char *const kb_abc[4] = {"1234567890", "qwertyuiop",
                                      "asdfghjkl-", "zxcvbnm,./"};
static const char *const kb_abc_up[4] = {"!@#$%^&*()", "QWERTYUIOP",
                                         "ASDFGHJKL_", "ZXCVBNM<>?"};
static const char *const kb_sym[4] = {"1234567890", "!@#$%^&*()",
                                      "~`|\\<>{}[]", "+=_:;\"',?-"};
static const int kb_wide[6] = {40, 44, 36, 86, 40, 46}; /* the bottom row */

static const Color kb_bg = {0x16, 0x16, 0x16};
static const Color kb_key_face = {0x2E, 0x2E, 0x33};
static const Color kb_mod_face = {0x22, 0x22, 0x26};
static const Color kb_down_face = {0x5A, 0x5A, 0x62};
static const Color kb_ink = {0x12, 0x12, 0x16};

static int kb_shift; /* 0 off, 1 the next key, 2 locked */
static int kb_layer; /* 0 letters, 1 symbols */
static int kb_held = -1;

static void kb_rect(int k, int *x, int *y, int *w, int *h) {
  if (k < K_SHIFT) {
    *x = KB_X0 + (k % 10) * (KB_KW + KB_GX);
    *y = KB_Y0 + (k / 10) * (KB_KH + KB_GY);
    *w = KB_KW;
    *h = KB_KH;
  } else if (k <= K_ENTER) {
    *x = KB_X0;
    for (int i = K_SHIFT; i < k; i++)
      *x += kb_wide[i - K_SHIFT] + KB_GX;
    *y = KB_Y0 + 4 * (KB_KH + KB_GY);
    *w = kb_wide[k - K_SHIFT];
    *h = KB_KH;
  } else {
    *x = BOT_SCREEN_WIDTH - 8 - KB_BTN - (k == K_PGUP ? KB_BTN + 6 : 0);
    *y = 3;
    *w = KB_BTN;
    *h = 20;
  }
}

/* Caps lock shifts letters only; a one-key shift shifts anything. */
static char kb_char(int k) {
  int r = k / 10, c = k % 10;
  char ch = kb_abc[r][c];
  if (kb_layer)
    return kb_sym[r][c];
  if (ch >= 'a' && ch <= 'z')
    return kb_shift ? kb_abc_up[r][c] : ch;
  return kb_shift == 1 ? kb_abc_up[r][c] : ch;
}

static void kb_key(int k, int down) {
  static const char *const names[] = {"Shift", "?123", "Tab", "space",
                                      "Del",   "Enter", "PgUp", "PgDn"};
  volatile u8 *fb = VRAM_BOT_A;
  const int sh = BOT_SCREEN_HEIGHT;
  int x, y, w, h, on;
  char one[2];
  const char *label;
  const Font *f = &ui_small;
  Color face = kb_mod_face, ink = COLOR_WHITE;

  kb_rect(k, &x, &y, &w, &h);
  if (k < K_SHIFT) {
    one[0] = kb_char(k);
    one[1] = 0;
    label = one;
    f = &ui_font;
    face = kb_key_face;
  } else {
    label = names[k - K_SHIFT];
    if (k == K_SHIFT && kb_shift == 2)
      label = "Caps";
    if (k == K_SYM && kb_layer)
      label = "abc";
    if (k == K_SPACE)
      ink = t_pal[TC_DIM];
  }
  on = (k == K_SHIFT && kb_shift && !kb_layer) || (k == K_SYM && kb_layer) ||
       k == K_ENTER;
  if (on) {
    face = g_accent;
    ink = kb_ink;
  }
  if (down) {
    face = kb_down_face;
    ink = COLOR_WHITE;
  }
  draw_filled_rect(fb, x, y, w, h, sh, k >= K_PGUP ? COLOR_HM_BAR : kb_bg);
  draw_filled_round_rect(fb, x, y, w, h, 6, sh, face);
  ui_text(fb, x + (w - ui_tw(f, label)) / 2, y + (h - ui_th(f)) / 2, sh,
          label, ink, face, f);
}

static void kb_keys(void) {
  for (int k = 0; k < K_COUNT; k++)
    kb_key(k, k == kb_held);
  screen_present_bottom();
}

void t_kb_redraw(void) {
  volatile u8 *fb = VRAM_BOT_A;
  const int sh = BOT_SCREEN_HEIGHT;
  int x;

  clear_screen(fb, BOT_FB_SIZE, kb_bg);
  draw_filled_rect(fb, 0, 0, BOT_SCREEN_WIDTH, 26, sh, COLOR_HM_BAR);
  ui_text(fb, 10, (26 - ui_th(&ui_bold)) / 2, sh, "Terminal", COLOR_WHITE,
          COLOR_HM_BAR, &ui_bold);
  x = 10 + ui_tw(&ui_bold, "Terminal") + 10;
  ui_text(fb, x, (26 - ui_th(&ui_small)) / 2, sh, "START: close",
          t_pal[TC_DIM], COLOR_HM_BAR, &ui_small);
  kb_keys();
}

static int kb_hit(int tx, int ty) {
  for (int k = 0; k < K_COUNT; k++) {
    int x, y, w, h;
    kb_rect(k, &x, &y, &w, &h);
    /* The gaps belong to the key on their left or above, so no tap is lost. */
    if (touch_in(tx, ty, x, y, w + KB_GX, h + KB_GY))
      return k;
  }
  return -1;
}

static int kb_repeats(int k) {
  return k < K_SHIFT || k == K_SPACE || k == K_DEL || k == K_PGUP ||
         k == K_PGDN;
}

static void ed_changed(void) {
  t_scroll = 0;
  t_cursor_on = 1;
  t_blink = timer_ticks();
  t_dirty = 1;
}

static void ed_set(const char *s) {
  int n = 0;
  while (s[n] && n < T_INPUT - 1) {
    in_buf[n] = s[n];
    n++;
  }
  in_buf[n] = 0;
  in_len = in_pos = n;
  ed_changed();
}

static void ed_insert(char c) {
  if (in_len >= T_INPUT - 1)
    return;
  memmove(in_buf + in_pos + 1, in_buf + in_pos, (u32)(in_len - in_pos + 1));
  in_buf[in_pos++] = c;
  in_len++;
  ed_changed();
}

static void ed_backspace(void) {
  int at = in_pos - 1;
  if (!in_pos)
    return;
  while (at > 0 && cont(in_buf[at]))
    at--;
  memmove(in_buf + at, in_buf + in_pos, (u32)(in_len - in_pos + 1));
  in_len -= in_pos - at;
  in_pos = at;
  ed_changed();
}

static void ed_left(void) {
  if (!in_pos)
    return;
  in_pos--;
  while (in_pos && cont(in_buf[in_pos]))
    in_pos--;
  ed_changed();
}

static void ed_right(void) {
  if (in_pos >= in_len)
    return;
  in_pos++;
  while (in_pos < in_len && cont(in_buf[in_pos]))
    in_pos++;
  ed_changed();
}

static int t_special(char c) {
  return c == ' ' || c == '\'' || c == '"' || c == '\\' || c == '*' ||
         c == '?' || c == '|' || c == '<' || c == '>' || c == ';' || c == '&';
}

/* Puts `word`, escaped, and then `tail` in place of in_buf[start..in_pos). */
static void ed_replace(int start, const char *word, const char *tail) {
  static char rep[T_INPUT * 2];
  int n = 0, cut = in_pos - start;
  for (const char *s = word; *s && n < T_INPUT; s++) {
    if (t_special(*s))
      rep[n++] = '\\';
    rep[n++] = *s;
  }
  for (const char *s = tail; *s && n < T_INPUT; s++)
    rep[n++] = *s;
  if (in_len - cut + n >= T_INPUT)
    return;
  memmove(in_buf + start + n, in_buf + in_pos, (u32)(in_len - in_pos + 1));
  memcpy(in_buf + start, rep, (u32)n);
  in_len += n - cut;
  in_pos = start + n;
  ed_changed();
}

static void hist_add(const char *s) {
  if (!s[0] || (t_hist_n && t_streq(t_hist[(t_hist_n - 1) % T_HIST], s)))
    return;
  t_cpy(t_hist[t_hist_n % T_HIST], s);
  t_hist_n++;
}

/* +1 steps back to an older command, -1 forward to the line being typed. */
static void hist_move(int dir) {
  int held = t_hist_n < T_HIST ? t_hist_n : T_HIST;
  int at = t_hist_at + dir;
  if (at < 0 || at > held)
    return;
  if (!t_hist_at)
    t_cpy(t_draft, in_buf);
  t_hist_at = at;
  ed_set(at ? t_hist[(t_hist_n - at) % T_HIST] : t_draft);
}

static int t_prefix(const char *s, const char *p, int n, int icase) {
  for (int i = 0; i < n; i++)
    if (icase ? t_fold(s[i]) != t_fold(p[i]) : s[i] != p[i])
      return 0;
  return 1;
}

/* Tab: completes the word under the cursor, a command name first and a path
 * after that. When several fit and nothing more is shared, lists them. */
static void t_complete(void) {
  static char word[T_INPUT], dir[T_PATH], nw[T_INPUT];
  static const char *cand[T_ENTS];
  static u8 cand_co[T_ENTS];
  int start = 0, lead = 1, wn = 0, nc = 0, slash = -1, plen, common;
  const char *pre;
  char q = 0;

  for (int i = 0; i < in_pos; i++) {
    char c = in_buf[i];
    if (q) {
      if (c == q)
        q = 0;
      else if (q == '"' && c == '\\')
        i++;
    } else if (c == '\\') {
      i++;
    } else if (c == '\'' || c == '"') {
      q = c;
    } else if (c == ' ') {
      start = i + 1;
    }
  }
  if (q)
    return;
  for (int i = 0; i < start; i++)
    if (in_buf[i] != ' ')
      lead = 0;
  for (int i = start; i < in_pos; i++) {
    char c = in_buf[i];
    if (q) {
      if (c == q) {
        q = 0;
        continue;
      }
      if (q == '"' && c == '\\' && i + 1 < in_pos)
        c = in_buf[++i];
    } else if (c == '\'' || c == '"') {
      q = c;
      continue;
    } else if (c == '\\' && i + 1 < in_pos) {
      c = in_buf[++i];
    }
    word[wn++] = c;
  }
  word[wn] = 0;
  for (int i = 0; i < wn; i++)
    if (word[i] == '/')
      slash = i;
  pre = word + slash + 1;
  plen = (int)strlen(pre);

  if (lead && slash < 0) {
    for (int i = 0; i < t_ncmds; i++)
      if ((int)strlen(t_cmds[i].name) >= plen &&
          t_prefix(t_cmds[i].name, pre, plen, 0)) {
        cand[nc] = t_cmds[i].name;
        cand_co[nc++] = TC_TEXT;
      }
  } else {
    char keep = word[slash + 1];
    int ok, n;
    if (!t_need_sd(0))
      return;
    word[slash + 1] = 0;
    ok = t_resolve(slash >= 0 ? word : ".", dir);
    word[slash + 1] = keep;
    n = ok ? t_readdir(dir, pre[0] == '.', 0, 0) : -1;
    for (int i = 0; i < n; i++)
      if ((int)strlen(t_ents[i].name) >= plen &&
          t_prefix(t_ents[i].name, pre, plen, 1)) {
        cand[nc] = t_ents[i].name;
        cand_co[nc++] = (t_ents[i].attr & AM_DIR) ? TC_DIR : TC_TEXT;
      }
  }
  if (!nc)
    return;

  common = (int)strlen(cand[0]);
  for (int i = 1; i < nc; i++) {
    int k = 0;
    while (k < common && t_fold(cand[0][k]) == t_fold(cand[i][k]))
      k++;
    common = k;
  }
  while (common > 0 && cont(cand[0][common]))
    common--;

  if (nc == 1 || common > plen) {
    int n = slash + 1;
    if (n + common >= T_INPUT)
      return;
    memcpy(nw, word, (u32)n);
    memcpy(nw + n, cand[0], (u32)common);
    nw[n + common] = 0;
    ed_replace(start, nw,
               nc > 1 ? "" : (cand_co[0] == TC_DIR ? "/" : " "));
    return;
  }
  t_prompt(t_out);
  t_line(in_buf, TC_TEXT);
  t_columns(cand, cand_co, nc);
  ed_changed();
}

static int t_match(const char *p, const char *s) {
  const char *star = 0, *back = 0;
  while (*s) {
    if (*p == '*') {
      star = p++;
      back = s;
    } else if (*p && (*p == '?' || t_fold(*p) == t_fold(*s))) {
      p++;
      s++;
    } else if (star) {
      p = star + 1;
      s = ++back;
    } else {
      return 0;
    }
  }
  while (*p == '*')
    p++;
  return !*p;
}

/* Splits a line into words: quotes group, a backslash takes the next
 * character as it is. `wild` marks words with an unquoted * or ?. */
static int t_split(const char *s, char *o, char **words, u8 *wild) {
  int n = 0;
  for (;;) {
    char q = 0;
    while (*s == ' ')
      s++;
    if (!*s)
      return n;
    if (n >= T_ARGS) {
      t_err("aurora", 0, 0, "too many arguments");
      return -1;
    }
    words[n] = o;
    wild[n] = 0;
    while (*s && (q || *s != ' ')) {
      char c = *s++;
      if (q) {
        if (c == q) {
          q = 0;
          continue;
        }
        if (q == '"' && c == '\\' && (*s == '"' || *s == '\\'))
          c = *s++;
      } else if (c == '\'' || c == '"') {
        q = c;
        continue;
      } else if (c == '\\') {
        if (!*s)
          continue;
        c = *s++;
      } else if (c == '|' || c == '<' || c == '>' || c == ';' || c == '&') {
        t_err("aurora", 0, 0, "pipes, redirection and ';' are not supported");
        return -1;
      } else if (c == '*' || c == '?') {
        wild[n] = 1;
      }
      *o++ = c;
    }
    if (q) {
      t_err("aurora", 0, 0, "unexpected end of line: a quote is not closed");
      return -1;
    }
    *o++ = 0;
    n++;
  }
}

/* The names a pattern matches, in place of it, sorted; only the last part of
 * the path may hold wildcards. Returns the count (0 leaves the word alone) or
 * -1 when there are too many. */
static int t_glob(const char *pat, char **argv, int room, char **pool,
                  char *end) {
  static char dir[T_PATH], lead[T_PATH];
  int slash = -1, n, got = 0;

  for (int i = 0; pat[i]; i++)
    if (pat[i] == '/')
      slash = i;
  for (int i = 0; i < slash; i++)
    if (pat[i] == '*' || pat[i] == '?')
      return 0;
  memcpy(lead, pat, (u32)(slash + 1));
  lead[slash + 1] = 0;
  if (!t_resolve(slash >= 0 ? lead : ".", dir))
    return 0;
  n = t_readdir(dir, pat[slash + 1] == '.', 0, 0);
  for (int i = 0; i < n; i++) {
    u32 ll = (u32)(slash + 1), nl;
    if (!t_match(pat + slash + 1, t_ents[i].name))
      continue;
    nl = (u32)strlen(t_ents[i].name) + 1u;
    if (got >= room || *pool + ll + nl > end)
      return -1;
    argv[got++] = *pool;
    memcpy(*pool, lead, ll);
    memcpy(*pool + ll, t_ents[i].name, nl);
    *pool += ll + nl;
  }
  return got;
}

static void t_run(const char *line) {
  static char words_buf[T_INPUT * 2], pool[8192];
  static char *words[T_ARGS], *argv[T_ARGS + 1];
  static u8 wild[T_ARGS];
  char *at = pool;
  int nw = t_split(line, words_buf, words, wild), argc = 0;

  if (nw <= 0)
    return;
  for (int i = 0; i < nw; i++) {
    if (wild[i] && t_need_sd(0)) {
      int m = t_glob(words[i], argv + argc, T_ARGS - argc, &at,
                     pool + sizeof(pool));
      if (m < 0) {
        t_err("aurora", 0, words[i], "argument list too long");
        return;
      }
      if (m) {
        argc += m;
        continue;
      }
    }
    if (argc >= T_ARGS) {
      t_err("aurora", 0, 0, "too many arguments");
      return;
    }
    argv[argc++] = words[i];
  }
  argv[argc] = 0;

  for (int i = 0; i < t_ncmds; i++) {
    if (!t_streq(argv[0], t_cmds[i].name))
      continue;
    if (argc > 1 && t_streq(argv[1], "--help")) {
      t_help(&t_cmds[i]);
    } else {
      t_cmds[i].run(argc, argv);
    }
    return;
  }
  if (!t_exec(argc, argv)) {
    t_out("aurora: ", TC_ERR);
    t_out(argv[0], TC_ERR);
    t_line(": command not found", TC_ERR);
  }
}

static void t_submit(void) {
  static char line[T_INPUT];
  t_prompt(t_out);
  t_line(in_buf, TC_TEXT);
  hist_add(in_buf);
  t_hist_at = 0;
  t_cpy(line, in_buf);
  ed_set("");

  t_running = 1;
  t_render(); /* the line stays up while the command runs */
  t_run(line);
  t_running = 0;
  if (t_x)
    t_outc('\n', TC_TEXT);
  ed_changed();
}

/* SELECT, like Ctrl+C: drops the line being typed. */
static void t_cancel(void) {
  t_prompt(t_out);
  t_out(in_buf, TC_TEXT);
  t_line("^C", TC_TEXT);
  t_hist_at = 0;
  ed_set("");
}

static void t_key(int k) {
  int shift = kb_shift, layer = kb_layer;
  switch (k) {
    case K_SHIFT:
      if (!kb_layer)
        kb_shift = (kb_shift + 1) % 3;
      break;
    case K_SYM:
      kb_layer = !kb_layer;
      break;
    case K_TAB:
      t_complete();
      break;
    case K_SPACE:
      ed_insert(' ');
      break;
    case K_DEL:
      ed_backspace();
      break;
    case K_ENTER:
      t_submit();
      break;
    case K_PGUP:
      t_scroll += T_ROWS / 2;
      t_dirty = 1;
      break;
    case K_PGDN:
      t_scroll = t_scroll > T_ROWS / 2 ? t_scroll - T_ROWS / 2 : 0;
      t_dirty = 1;
      break;
    default:
      ed_insert(kb_char(k));
      if (kb_shift == 1 && !kb_layer)
        kb_shift = 0;
      break;
  }
  if (kb_shift != shift || kb_layer != layer)
    kb_keys();
}

static void terminal_run(const char *user, void (*launch)(const char *path)) {
  static FILINFO fno;
  u32 repeat_mark = 0, repeat_wait = 0;

  t_user = (user && user[0]) ? user : "user";
  t_host = aurora_is_new3ds() ? "n3ds" : "o3ds";
  t_launch = launch;
  t_quit = 0;
  t_running = 0;
  t_scroll = 0;
  kb_held = -1;
  kb_shift = 0;
  kb_layer = 0;

  if (t_need_sd(0) &&
      (t_stat(t_cwd, &fno) != FR_OK || !(fno.fattrib & AM_DIR)))
    t_cpy(t_cwd, "0:/");
  if (!t_started) {
    t_started = 1;
    t_out("AuroraOS ", TC_TEXT);
    t_out(AURORA_VERSION, TC_TEXT);
    t_out(" (", TC_TEXT);
    t_out(t_host, TC_TEXT);
    t_line(")", TC_TEXT);
    t_line("Type 'help' for the commands. START closes.", TC_DIM);
    if (!t_sd)
      t_line("No SD card: the file commands will not work.", TC_WARN);
  }

  t_kb_redraw();
  ed_changed();
  t_render();

  while (!t_quit) {
    u32 k = get_keys_down();
    int tx, ty, hx, hy, ev = -1;

    if (k & BUTTON_START)
      break;

    if (touch_tap(&tx, &ty)) {
      int hit = kb_hit(tx, ty);
      if (kb_held >= 0 && kb_held != hit)
        kb_key(kb_held, 0);
      kb_held = hit;
      if (hit >= 0) {
        kb_key(hit, 1);
        ev = hit;
        repeat_mark = timer_ticks();
        repeat_wait = 450000u;
      }
      screen_present_bottom();
    } else if (kb_held >= 0) {
      if (!touch_read(&hx, &hy, 0, 0)) {
        int was = kb_held;
        kb_held = -1;
        kb_key(was, 0);
        screen_present_bottom();
      } else if (kb_repeats(kb_held) && timer_calibrated() &&
                 timer_us_since(repeat_mark) >= repeat_wait) {
        ev = kb_held;
        repeat_mark = timer_ticks();
        repeat_wait = 60000u;
      }
    }
    if (ev >= 0)
      t_key(ev);

    if (k & BUTTON_A)
      t_key(K_ENTER);
    if (k & BUTTON_B)
      t_key(K_DEL);
    if (k & BUTTON_Y)
      t_key(K_TAB);
    if (k & BUTTON_L)
      t_key(K_SHIFT);
    if (k & BUTTON_R)
      t_key(K_SYM);
    if (k & BUTTON_SELECT)
      t_cancel();
    if (k & BUTTON_DLEFT)
      ed_left();
    if (k & BUTTON_DRIGHT)
      ed_right();
    if (k & BUTTON_DUP)
      hist_move(1);
    if (k & BUTTON_DDOWN)
      hist_move(-1);

    if (timer_calibrated() && timer_us_since(t_blink) >= 530000u) {
      t_blink = timer_ticks();
      t_cursor_on = !t_cursor_on;
      t_dirty = 1;
    }
    if (t_dirty && !t_quit)
      t_render();
    ui_idle();
  }
  t_unmount();
}

void terminal_screen(const char *user, void (*launch)(const char *path)) {
  anim_transition(ANIM_PUSH, ANIM_BOTH);
  terminal_run(user, launch);
  anim_transition(ANIM_POP, ANIM_BOTH);
}
