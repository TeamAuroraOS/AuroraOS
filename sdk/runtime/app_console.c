/* The console stdout and stderr write to: 8x8 cells drawn into a screen's
 * backbuffer, wrapping and scrolling. It understands the ANSI escapes most
 * console programs use: ESC[2J, ESC[K, ESC[row;colH and ESC[...m colours. */
#include "app_internal.h"
#include "font.h"

#include <string.h>

#define SH SCREEN_HEIGHT

static bool g_on;
static Screen g_scr;
static int g_cols, g_rows = SH / FONT_HEIGHT, g_cx, g_cy;
static uint32_t g_base_fg = 0xFFFFFFu, g_base_bg = 0x000000u;
static uint32_t g_fg = 0xFFFFFFu, g_bg = 0x000000u;

/* xterm's palette: 30..37 and 40..47, then the bright 90..97 and 100..107. */
static const uint32_t ansi[16] = {
    0x000000, 0xCD0000, 0x00CD00, 0xCDCD00, 0x0000EE, 0xCD00CD, 0x00CDCD,
    0xE5E5E5, 0x7F7F7F, 0xFF0000, 0x00FF00, 0xFFFF00, 0x5C5CFF, 0xFF00FF,
    0x00FFFF, 0xFFFFFF,
};

static enum { ESC_NONE, ESC_START, ESC_CSI } g_esc;
static int g_par[4], g_npar;

void con_init(Screen s) {
  g_on = true;
  g_scr = s == SCREEN_BOTTOM ? SCREEN_BOTTOM : SCREEN_TOP;
  g_cols = gfx_width(g_scr) / FONT_WIDTH;
  con_clear();
}

void con_clear(void) {
  if (!g_on) {
    con_init(SCREEN_TOP);
    return;
  }
  gfx_clear(g_scr, g_bg);
  g_cx = g_cy = 0;
}

void con_color(uint32_t fg, uint32_t bg) {
  g_base_fg = g_fg = fg & 0xFFFFFFu;
  g_base_bg = g_bg = bg & 0xFFFFFFu;
}

void con_move(int col, int row) {
  if (!g_on)
    con_init(SCREEN_TOP);
  g_cx = col < 0 ? 0 : col >= g_cols ? g_cols - 1 : col;
  g_cy = row < 0 ? 0 : row >= g_rows ? g_rows - 1 : row;
}

int con_cols(void) { return g_on ? g_cols : SCREEN_TOP_WIDTH / FONT_WIDTH; }
int con_rows(void) { return g_rows; }

/* Rows run backwards in memory within a column, so moving everything up a
 * line moves each column's bytes 24 further on. */
static void scroll(void) {
  u8 *fb = gfx_framebuffer(g_scr);
  u8 b = (u8)g_bg, g = (u8)(g_bg >> 8), r = (u8)(g_bg >> 16);
  for (int x = 0; x < gfx_width(g_scr); x++) {
    u8 *col = fb + x * SH * 3;
    memmove(col + FONT_HEIGHT * 3, col, (SH - FONT_HEIGHT) * 3);
    for (int i = 0; i < FONT_HEIGHT; i++) {
      col[i * 3 + 0] = b;
      col[i * 3 + 1] = g;
      col[i * 3 + 2] = r;
    }
  }
}

static void newline(void) {
  g_cx = 0;
  if (++g_cy >= g_rows) {
    scroll();
    g_cy = g_rows - 1;
  }
}

static void cell(unsigned char c) {
  if (g_cx >= g_cols)
    newline();
  draw_char(app_fb(g_scr), g_cx * FONT_WIDTH, g_cy * FONT_HEIGHT, SH,
            (char)(c < 0x80 ? c : ' '), app_color(g_fg), app_color(g_bg));
  g_cx++;
}

static void sgr(int n) {
  if (n == 0) {
    g_fg = g_base_fg;
    g_bg = g_base_bg;
  } else if (n >= 30 && n <= 37) {
    g_fg = ansi[n - 30];
  } else if (n >= 90 && n <= 97) {
    g_fg = ansi[n - 90 + 8];
  } else if (n >= 40 && n <= 47) {
    g_bg = ansi[n - 40];
  } else if (n >= 100 && n <= 107) {
    g_bg = ansi[n - 100 + 8];
  } else if (n == 39) {
    g_fg = g_base_fg;
  } else if (n == 49) {
    g_bg = g_base_bg;
  }
}

static void csi(char op) {
  int a = g_npar > 0 ? g_par[0] : 0, b = g_npar > 1 ? g_par[1] : 0;
  switch (op) {
  case 'J':
    if (a == 2)
      con_clear();
    break;
  case 'K':
    gfx_rect(g_scr, g_cx * FONT_WIDTH, g_cy * FONT_HEIGHT,
             (g_cols - g_cx) * FONT_WIDTH, FONT_HEIGHT, g_bg);
    break;
  case 'H':
  case 'f':
    con_move((b ? b : 1) - 1, (a ? a : 1) - 1);
    break;
  case 'm':
    if (!g_npar)
      sgr(0);
    for (int i = 0; i < g_npar; i++)
      sgr(g_par[i]);
    break;
  default:
    break;
  }
}

/* Returns true while the byte belongs to an escape sequence. */
static bool escape(unsigned char c) {
  switch (g_esc) {
  case ESC_NONE:
    if (c != 0x1B)
      return false;
    g_esc = ESC_START;
    return true;
  case ESC_START:
    if (c == '[') {
      g_esc = ESC_CSI;
      g_npar = 0;
      g_par[0] = 0;
    } else {
      g_esc = ESC_NONE;
    }
    return true;
  case ESC_CSI:
    if (c >= '0' && c <= '9') {
      if (!g_npar)
        g_npar = 1;
      if (g_npar <= 4)
        g_par[g_npar - 1] = g_par[g_npar - 1] * 10 + (c - '0');
    } else if (c == ';') {
      if (!g_npar)
        g_npar = 1;
      if (g_npar < 4)
        g_par[g_npar] = 0;
      g_npar++;
    } else {
      if (g_npar > 4)
        g_npar = 4;
      g_esc = ESC_NONE;
      csi((char)c);
    }
    return true;
  }
  return false;
}

void app_con_write(const char *p, size_t n) {
  if (!g_on)
    con_init(SCREEN_TOP);
  app_dirty(g_scr);
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)p[i];
    if (escape(c))
      continue;
    switch (c) {
    case '\n':
      newline();
      break;
    case '\r':
      g_cx = 0;
      break;
    case '\t':
      do
        cell(' ');
      while (g_cx % 4 && g_cx < g_cols);
      break;
    case '\b':
      if (g_cx > 0)
        g_cx--;
      break;
    case '\f':
      con_clear();
      break;
    default:
      /* A UTF-8 sequence takes one cell, shown blank: the font is ASCII. */
      if (c >= 0x20 && (c & 0xC0u) != 0x80u)
        cell(c);
      break;
    }
  }
}
