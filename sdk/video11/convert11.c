/* YUV 4:2:0 to the top screen's BGR. The framebuffer stands on its side: its bytes
 * run up each column, left to right across the screen, so writing in that order
 * is one straight run through memory, which is what uncached memory wants. Reading
 * a column at a time is harder on the cache; the source fits L1 at 240p, and the
 * larger ones have the New 3DS's L2 behind it. */
#include <stdint.h>

#include "libavutil/frame.h"
#include "libavutil/pixfmt.h"

#include "engine.h"
#include "video.h"

#define W 400
#define H 240

/* Fixed point with 8 fractional bits. */
typedef struct {
  int16_t y[256];
  int16_t vr[256], ug[256], vg[256], ub[256];
} Tables;

static Tables tab;
static int tab_for = -1;
static uint8_t clip_store[1280];
#define CLIP (clip_store + 512)

/* Limited range only. BT.709 from 720 lines, or when the stream says so; BT.601 for
 * the rest. */
static void tables_build(int bt709) {
  /* 298/256 scales 16..235 to 0..255. */
  const int vr = bt709 ? 459 : 409, ug = bt709 ? 55 : 100;
  const int vg = bt709 ? 136 : 208, ub = bt709 ? 541 : 516;
  for (int i = 0; i < 256; i++) {
    int c = i - 128;
    tab.y[i] = (int16_t)(((i - 16) * 298 + 128) >> 8);
    tab.vr[i] = (int16_t)((vr * c + 128) >> 8);
    tab.ug[i] = (int16_t)((-ug * c + 128) >> 8);
    tab.vg[i] = (int16_t)((-vg * c + 128) >> 8);
    tab.ub[i] = (int16_t)((ub * c + 128) >> 8);
  }
  for (int i = -512; i < 768; i++)
    CLIP[i] = (uint8_t)(i < 0 ? 0 : i > 255 ? 255 : i);
  tab_for = bt709;
}

/* Which source pixel each screen pixel takes. */
static int16_t col_x[W];
static const uint8_t *row_y[H], *row_u[H], *row_v[H];
static int fit_w, fit_h, fit_for_w, fit_for_h, fit_x0, fit_y0;

static void fit(int sw, int sh) {
  fit_w = W;
  fit_h = sw ? sh * W / sw : H;
  if (fit_h > H) {
    fit_h = H;
    fit_w = sh ? sw * H / sh : W;
  }
  fit_x0 = (W - fit_w) / 2;
  fit_y0 = (H - fit_h) / 2;
  for (int x = 0; x < fit_w; x++)
    col_x[x] = (int16_t)(x * sw / fit_w);
  fit_for_w = sw;
  fit_for_h = sh;
}

void video11_convert(const AVFrame *f, uint8_t *fb) {
  const int sw = f->width, sh = f->height;
  int bt709 = f->colorspace == AVCOL_SPC_BT709 ||
              (f->colorspace == AVCOL_SPC_UNSPECIFIED && sh >= 720);
  const int16_t *ty = tab.y, *tvr = tab.vr, *tug = tab.ug, *tvg = tab.vg, *tub = tab.ub;
  const uint8_t *clip = CLIP;
  uint8_t *d = fb;

  if (tab_for != bt709)
    tables_build(bt709);
  if (sw != fit_for_w || sh != fit_for_h)
    fit(sw, sh);
  for (int y = 0; y < fit_h; y++) {
    int sy = y * sh / fit_h, cy = sy >> 1;
    /* The screen's y runs bottom to top down a column. */
    row_y[y] = f->data[0] + sy * f->linesize[0];
    row_u[y] = f->data[1] + cy * f->linesize[1];
    row_v[y] = f->data[2] + cy * f->linesize[2];
  }

  for (int x = 0; x < fit_x0; x++)
    for (int b = 0; b < H * 3; b++)
      *d++ = 0;
  for (int x = 0; x < fit_w; x++) {
    const int sx = col_x[x], cx = sx >> 1;
    int y;
    for (y = 0; y < H - fit_h - fit_y0; y++) {
      d[0] = d[1] = d[2] = 0;
      d += 3;
    }
    for (y = fit_h - 1; y >= 0; y--) {
      const int u = row_u[y][cx], v = row_v[y][cx];
      const int yy = ty[row_y[y][sx]];
      d[0] = clip[yy + tub[u]];
      d[1] = clip[yy + tug[u] + tvg[v]];
      d[2] = clip[yy + tvr[v]];
      d += 3;
    }
    for (y = 0; y < fit_y0; y++) {
      d[0] = d[1] = d[2] = 0;
      d += 3;
    }
  }
  for (int x = fit_x0 + fit_w; x < W; x++)
    for (int b = 0; b < H * 3; b++)
      *d++ = 0;
}
