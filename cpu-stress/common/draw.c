#include "draw.h"
#include "font8x8.h"

#include <stdarg.h>
#include <stdio.h>

void d_rect(Fb *f, int x, int y, int w, int h, uint32_t c) {
  uint8_t b = (uint8_t)c, g = (uint8_t)(c >> 8), r = (uint8_t)(c >> 16);
  if (x < 0) {
    w += x;
    x = 0;
  }
  if (y < 0) {
    h += y;
    y = 0;
  }
  if (w > f->w - x)
    w = f->w - x;
  if (h > SCR_H - y)
    h = SCR_H - y;
  if (w <= 0 || h <= 0)
    return;
  /* A column runs from the bottom row up, so the rect's rows are one run. */
  for (int i = x; i < x + w; i++) {
    uint8_t *p = f->p + ((i * SCR_H) + (SCR_H - y - h)) * 3;
    for (int k = 0; k < h; k++, p += 3) {
      p[0] = b;
      p[1] = g;
      p[2] = r;
    }
  }
}

void d_clear(Fb *f, uint32_t c) { d_rect(f, 0, 0, f->w, SCR_H, c); }

static void put(Fb *f, int x, int y, uint32_t c) {
  uint8_t *p;
  if ((unsigned)x >= (unsigned)f->w || (unsigned)y >= SCR_H)
    return;
  p = f->p + ((x * SCR_H) + (SCR_H - 1 - y)) * 3;
  p[0] = (uint8_t)c;
  p[1] = (uint8_t)(c >> 8);
  p[2] = (uint8_t)(c >> 16);
}

void d_line(Fb *f, int x0, int y0, int x1, int y1, uint32_t c) {
  int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
  int dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1;
  int e = dx + dy;
  for (;;) {
    int e2 = 2 * e; /* both steps decide on the error before either */
    put(f, x0, y0, c);
    if (x0 == x1 && y0 == y1)
      return;
    if (e2 >= dy) {
      e += dy;
      x0 += sx;
    }
    if (e2 <= dx) {
      e += dx;
      y0 += sy;
    }
  }
}

int d_text(Fb *f, int x, int y, uint32_t c, const char *s) {
  int x0 = x;
  for (; *s; s++, x += 8) {
    unsigned ch = (unsigned char)*s;
    if (ch < 0x20 || ch > 0x7E)
      ch = '?';
    for (int row = 0; row < 8; row++) {
      uint8_t bits = font8x8[ch - 0x20][row];
      for (int col = 0; col < 8; col++)
        if (bits & (0x80u >> col))
          put(f, x + col, y + row, c);
    }
  }
  return x - x0;
}

int d_textf(Fb *f, int x, int y, uint32_t c, const char *fmt, ...) {
  char buf[96];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  return d_text(f, x, y, c, buf);
}
