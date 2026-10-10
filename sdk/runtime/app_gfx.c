/* Drawing, into screen.c's backbuffers in cached FCRAM. A present copies a
 * backbuffer to its panel with the GPU when the ARM11 core is there (switching
 * at the vertical blank), else with the CPU. */
#include "app_internal.h"
#include "assets.h"
#include "font.h"
#include "gpu.h"
#include "image.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SH SCREEN_HEIGHT

static bool g_gpu;
static u32 g_dirty; /* bit 0 the top screen, bit 1 the bottom */

volatile u8 *app_fb(Screen s) {
  return s == SCREEN_BOTTOM ? VRAM_BOT_A : VRAM_TOP_LA;
}

void app_dirty(Screen s) { g_dirty |= s == SCREEN_BOTTOM ? 2u : 1u; }

void gfx_mark(Screen s) { app_dirty(s); }

int gfx_width(Screen s) {
  return s == SCREEN_BOTTOM ? SCREEN_BOTTOM_WIDTH : SCREEN_TOP_WIDTH;
}

/* The backbuffer, once a present still reading it has finished. */
static u8 *begin(Screen s) {
  volatile u8 *fb = app_fb(s);
  screen_touch(fb);
  app_dirty(s);
  return (u8 *)fb;
}

static inline void put(u8 *fb, int w, int x, int y, uint32_t c) {
  u8 *p;
  if ((unsigned)x >= (unsigned)w || (unsigned)y >= (unsigned)SH)
    return;
  p = fb + ((x * SH) + (SH - 1 - y)) * 3;
  p[0] = (u8)c;
  p[1] = (u8)(c >> 8);
  p[2] = (u8)(c >> 16);
}

/* Rows y0..y1 of column x. Within a column the rows run backwards in memory,
 * so the run starts at y1. */
static void vspan(u8 *fb, int w, int x, int y0, int y1, uint32_t c) {
  u8 *p;
  if ((unsigned)x >= (unsigned)w)
    return;
  if (y0 > y1) {
    int t = y0;
    y0 = y1;
    y1 = t;
  }
  if (y0 < 0)
    y0 = 0;
  if (y1 > SH - 1)
    y1 = SH - 1;
  if (y0 > y1)
    return;
  p = fb + ((x * SH) + (SH - 1 - y1)) * 3;
  for (int n = y1 - y0 + 1; n > 0; n--) {
    p[0] = (u8)c;
    p[1] = (u8)(c >> 8);
    p[2] = (u8)(c >> 16);
    p += 3;
  }
}

/* Clips a rectangle to the screen; false when none of it is left. */
static bool clip(Screen s, int *x, int *y, int *w, int *h) {
  int sw = gfx_width(s);
  if (*x < 0) {
    *w += *x;
    *x = 0;
  }
  if (*y < 0) {
    *h += *y;
    *y = 0;
  }
  if (*w > sw - *x)
    *w = sw - *x;
  if (*h > SH - *y)
    *h = SH - *y;
  return *w > 0 && *h > 0;
}

void app_gfx_init(void) {
  if (app_core() && gpu_init()) {
    g_screen_blit = gpu_present_async;
    g_screen_wait = gpu_wait_idle;
    g_gpu = true;
  }
  g_fb_top = VRAM_TOP_BACK;
  g_fb_bot = VRAM_BOT_BACK;
  clear_screen(VRAM_TOP_BACK, TOP_FB_SIZE, app_color(0));
  clear_screen(VRAM_BOT_BACK, BOT_FB_SIZE, app_color(0));
  g_dirty = 3;
  app_present();
}

/* While a network call keeps the core busy: the newest frame goes to
 * framebuffer A, A goes on show, and presents copy there with the CPU. */
static bool g_direct;
static int (*g_gpu_blit)(u32 src, u32 dst, u32 len);

void app_gfx_direct(bool on) {
  if (on == g_direct || !g_gpu)
    return;
  if (on) {
    gpu_wait_idle();
    g_gpu_blit = g_screen_blit;
    g_screen_blit = 0;
    screen_present_top();
    screen_present_bottom();
    gpu_show_a();
  } else {
    g_screen_blit = g_gpu_blit;
  }
  g_direct = on;
}

u32 app_present(void) {
  u32 d = g_dirty;

  g_dirty = 0;
  if (!d)
    return 0;
  if (g_gpu && !g_direct && d == 3u) {
    /* Both panels switch at the same blank. Waited for, since screen_touch()
     * guards only one buffer. */
    gpu_wait_idle();
    g_blit_src = 0;
    if (gpu_present2_async((u32)VRAM_TOP_BACK, (u32)VRAM_BOT_BACK)) {
      gpu_wait_idle();
      return d;
    }
  }
  if (d & 1u)
    screen_present_top();
  if (d & 2u)
    screen_present_bottom();
  return d;
}

void gfx_present(void) {
  app_flush_stdout();
  app_present();
}

uint8_t *gfx_framebuffer(Screen s) { return begin(s); }

void gfx_clear(Screen s, uint32_t color) {
  if (s == SCREEN_BOTTOM)
    clear_screen(VRAM_BOT_A, BOT_FB_SIZE, app_color(color));
  else
    clear_screen(VRAM_TOP_LA, TOP_FB_SIZE, app_color(color));
  app_dirty(s);
}

void gfx_pixel(Screen s, int x, int y, uint32_t color) {
  put(begin(s), gfx_width(s), x, y, color);
}

uint32_t gfx_get_pixel(Screen s, int x, int y) {
  volatile u8 *fb = app_fb(s);
  const u8 *p;
  if ((unsigned)x >= (unsigned)gfx_width(s) || (unsigned)y >= (unsigned)SH)
    return 0;
  screen_touch(fb);
  p = (const u8 *)fb + ((x * SH) + (SH - 1 - y)) * 3;
  return ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0];
}

void gfx_rect(Screen s, int x, int y, int w, int h, uint32_t color) {
  if (!clip(s, &x, &y, &w, &h))
    return;
  draw_filled_rect(app_fb(s), x, y, w, h, SH, app_color(color));
  app_dirty(s);
}

void gfx_rect_blend(Screen s, int x, int y, int w, int h, uint32_t color,
                    int alpha) {
  if (alpha <= 0 || !clip(s, &x, &y, &w, &h))
    return;
  if (alpha > 255)
    alpha = 255;
  draw_filled_rect_alpha(app_fb(s), x, y, w, h, SH, app_color(color),
                         alpha + (alpha >> 7));
  app_dirty(s);
}

void gfx_rect_outline(Screen s, int x, int y, int w, int h, int t,
                      uint32_t color) {
  if (t < 1)
    t = 1;
  if (2 * t >= w || 2 * t >= h) {
    gfx_rect(s, x, y, w, h, color);
    return;
  }
  gfx_rect(s, x, y, w, t, color);
  gfx_rect(s, x, y + h - t, w, t, color);
  gfx_rect(s, x, y + t, t, h - 2 * t, color);
  gfx_rect(s, x + w - t, y + t, t, h - 2 * t, color);
}

void gfx_round_rect(Screen s, int x, int y, int w, int h, int r,
                    uint32_t color) {
  if (w <= 0 || h <= 0 || x >= gfx_width(s) || y >= SH || x + w <= 0 ||
      y + h <= 0)
    return;
  draw_filled_round_rect(app_fb(s), x, y, w, h, r, SH, app_color(color));
  app_dirty(s);
}

void gfx_gradient(Screen s, int x, int y, int w, int h, uint32_t top,
                  uint32_t bottom) {
  int sw = gfx_width(s);
  /* Only across: draw_vgradient clips the rows itself, keeping the ramp. */
  if (x < 0) {
    w += x;
    x = 0;
  }
  if (w > sw - x)
    w = sw - x;
  if (w <= 0 || h <= 0 || y >= SH || y + h <= 0)
    return;
  draw_vgradient(app_fb(s), x, y, w, h, SH, app_color(top), app_color(bottom));
  app_dirty(s);
}

static int outcode(long long x, long long y, int w) {
  return (x < 0 ? 1 : x >= w ? 2 : 0) | (y < 0 ? 4 : y >= SH ? 8 : 0);
}

/* Cohen-Sutherland, so a line far off the screen costs nothing. */
static bool clip_line(int w, int *x0, int *y0, int *x1, int *y1) {
  long long ax = *x0, ay = *y0, bx = *x1, by = *y1;
  for (;;) {
    int ca = outcode(ax, ay, w), cb = outcode(bx, by, w), c;
    long long x, y;
    if (!(ca | cb))
      break;
    if (ca & cb)
      return false;
    c = ca ? ca : cb;
    if (c & 8) {
      y = SH - 1;
      x = ax + (bx - ax) * (y - ay) / (by - ay);
    } else if (c & 4) {
      y = 0;
      x = ax + (bx - ax) * (y - ay) / (by - ay);
    } else if (c & 2) {
      x = w - 1;
      y = ay + (by - ay) * (x - ax) / (bx - ax);
    } else {
      x = 0;
      y = ay + (by - ay) * (x - ax) / (bx - ax);
    }
    if (c == ca) {
      ax = x;
      ay = y;
    } else {
      bx = x;
      by = y;
    }
  }
  *x0 = (int)ax;
  *y0 = (int)ay;
  *x1 = (int)bx;
  *y1 = (int)by;
  return true;
}

void gfx_line(Screen s, int x0, int y0, int x1, int y1, uint32_t color) {
  int w = gfx_width(s), dx, dy, sx, sy, err;
  u8 *fb;

  if (!clip_line(w, &x0, &y0, &x1, &y1))
    return;
  fb = begin(s);
  dx = x1 > x0 ? x1 - x0 : x0 - x1;
  dy = y1 > y0 ? y0 - y1 : y1 - y0;
  sx = x0 < x1 ? 1 : -1;
  sy = y0 < y1 ? 1 : -1;
  err = dx + dy;
  for (;;) {
    put(fb, w, x0, y0, color);
    if (x0 == x1 && y0 == y1)
      break;
    int e2 = 2 * err;
    if (e2 >= dy) {
      err += dy;
      x0 += sx;
    }
    if (e2 <= dx) {
      err += dx;
      y0 += sy;
    }
  }
}

void gfx_circle(Screen s, int cx, int cy, int r, uint32_t color) {
  int w = gfx_width(s), hy = r;
  long long lim = (long long)r * r + r; /* + r: rounder at small sizes */
  u8 *fb;

  if (r < 0 || cx + r < 0 || cx - r >= w || cy + r < 0 || cy - r >= SH)
    return;
  fb = begin(s);
  for (int dx = 0; dx <= r; dx++) {
    if (cx + dx >= w && cx - dx < 0)
      break;
    while (hy > 0 && (long long)dx * dx + (long long)hy * hy > lim)
      hy--;
    vspan(fb, w, cx + dx, cy - hy, cy + hy, color);
    if (dx)
      vspan(fb, w, cx - dx, cy - hy, cy + hy, color);
  }
}

void gfx_circle_outline(Screen s, int cx, int cy, int r, uint32_t color) {
  int w = gfx_width(s), x = r, y = 0, err = 1 - r;
  u8 *fb;

  if (r < 0 || cx + r < 0 || cx - r >= w || cy + r < 0 || cy - r >= SH)
    return;
  fb = begin(s);
  while (x >= y) {
    put(fb, w, cx + x, cy + y, color);
    put(fb, w, cx + y, cy + x, color);
    put(fb, w, cx - y, cy + x, color);
    put(fb, w, cx - x, cy + y, color);
    put(fb, w, cx - x, cy - y, color);
    put(fb, w, cx - y, cy - x, color);
    put(fb, w, cx + y, cy - x, color);
    put(fb, w, cx + x, cy - y, color);
    y++;
    if (err < 0) {
      err += 2 * y + 1;
    } else {
      x--;
      err += 2 * (y - x) + 1;
    }
  }
}

/* The y of the edge (ax, ay)-(bx, by) at column x, rounded. */
static int edge_y(int ax, int ay, int bx, int by, int x) {
  long long num = (long long)(by - ay) * (x - ax) * 2 + (long long)(bx - ax);
  return ay + (int)(num / (2LL * (bx - ax)));
}

/* Filled by columns, the framebuffer's own direction. */
void gfx_triangle(Screen s, int x0, int y0, int x1, int y1, int x2, int y2,
                  uint32_t color) {
  int w = gfx_width(s), t, xa, xb;
  u8 *fb;

  if (x0 > x1) {
    t = x0; x0 = x1; x1 = t;
    t = y0; y0 = y1; y1 = t;
  }
  if (x1 > x2) {
    t = x1; x1 = x2; x2 = t;
    t = y1; y1 = y2; y2 = t;
  }
  if (x0 > x1) {
    t = x0; x0 = x1; x1 = t;
    t = y0; y0 = y1; y1 = t;
  }
  if (x2 < 0 || x0 >= w)
    return;
  fb = begin(s);
  if (x0 == x2) {
    int lo = y0 < y1 ? y0 : y1, hi = y0 > y1 ? y0 : y1;
    if (y2 < lo)
      lo = y2;
    if (y2 > hi)
      hi = y2;
    vspan(fb, w, x0, lo, hi, color);
    return;
  }
  xa = x0 < 0 ? 0 : x0;
  xb = x2 >= w ? w - 1 : x2;
  for (int x = xa; x <= xb; x++) {
    int ya = edge_y(x0, y0, x2, y2, x), yb;
    if (x < x1)
      yb = edge_y(x0, y0, x1, y1, x);
    else if (x2 != x1)
      yb = edge_y(x1, y1, x2, y2, x);
    else
      yb = y1;
    vspan(fb, w, x, ya, yb, color);
  }
}

static char g_text[1024];
static char g_line[1024];

static const char *vfmt(const char *fmt, va_list ap) {
  vsnprintf(g_text, sizeof(g_text), fmt, ap);
  return g_text;
}

/* One 8x8 glyph, every pixel scale x scale, background left alone. */
static void glyph(u8 *fb, int w, int x, int y, unsigned char c, uint32_t color,
                  int scale) {
  const u8 *g;
  if (c < 0x20 || c > 0x7E)
    return;
  if (x >= w || y >= SH || x + 8 * scale <= 0 || y + 8 * scale <= 0)
    return;
  g = font_data[c - 0x20];
  for (int row = 0; row < FONT_HEIGHT; row++) {
    u8 bits = g[row];
    for (int col = 0; bits && col < FONT_WIDTH; col++) {
      if (!(bits & (0x80 >> col)))
        continue;
      if (scale == 1) {
        put(fb, w, x + col, y + row, color);
        continue;
      }
      for (int i = 0; i < scale; i++)
        vspan(fb, w, x + col * scale + i, y + row * scale,
              y + row * scale + scale - 1, color);
    }
  }
}

static int mono_text(Screen s, int x, int y, int scale, uint32_t color,
                     const char *t) {
  u8 *fb = begin(s);
  int w = gfx_width(s), cx = x, widest = 0, cell = FONT_WIDTH * scale;

  for (; *t; t++) {
    unsigned char c = (unsigned char)*t;
    if (c == '\n') {
      if (cx - x > widest)
        widest = cx - x;
      cx = x;
      y += FONT_HEIGHT * scale;
    } else if (c == '\t') {
      cx = x + ((cx - x) / (4 * cell) + 1) * 4 * cell;
    } else if ((c & 0xC0u) != 0x80u) {
      /* A UTF-8 continuation byte takes no cell of its own. */
      glyph(fb, w, cx, y, c, color, scale);
      cx += cell;
    }
  }
  return cx - x > widest ? cx - x : widest;
}

static Font g_fonts[4];
static int g_fonts_state; /* 0 not tried, 1 loaded, -1 not there */

static const Font *font_of(GfxFont f) {
  static const u32 ids[4] = {ASSET_FONT_SMALL, ASSET_FONT_UI, ASSET_FONT_BOLD,
                             ASSET_FONT_TITLE};
  if (f <= FONT_MONO || f > FONT_TITLE)
    return NULL;
  if (!g_fonts_state) {
    g_fonts_state = -1;
    if (app_fs_mount() && assets_read()) {
      int ok = 1;
      for (int i = 0; i < 4; i++)
        ok &= asset_font(ids[i], &g_fonts[i]);
      if (ok)
        g_fonts_state = 1;
    }
  }
  return g_fonts_state > 0 ? &g_fonts[f - 1] : NULL;
}

bool gfx_fonts_loaded(void) { return font_of(FONT_REGULAR) != NULL; }

/* Copies the line `t` starts with into g_line; returns the next one. */
static const char *next_line(const char *t) {
  size_t n = 0;
  while (t[n] && t[n] != '\n' && n < sizeof(g_line) - 1)
    n++;
  memcpy(g_line, t, n);
  g_line[n] = '\0';
  t += n;
  return *t == '\n' ? t + 1 : t;
}

static int font_text(Screen s, int x, int y, const Font *f, uint32_t color,
                     const char *t) {
  volatile u8 *fb = app_fb(s);
  int asc = text_ascent(f), lh = text_line_height(f), widest = 0;

  app_dirty(s);
  while (*t) {
    int w;
    t = next_line(t);
    if (y < SH && y + lh > 0)
      draw_text(fb, x, y + asc, SH, g_line, app_color(color), f);
    w = text_width(f, g_line);
    if (w > widest)
      widest = w;
    y += lh;
  }
  return widest;
}

int gfx_text(Screen s, int x, int y, uint32_t color, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  const char *t = vfmt(fmt, ap);
  va_end(ap);
  return mono_text(s, x, y, 1, color, t);
}

int gfx_text_scaled(Screen s, int x, int y, int scale, uint32_t color,
                    const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  const char *t = vfmt(fmt, ap);
  va_end(ap);
  return mono_text(s, x, y, scale < 1 ? 1 : scale, color, t);
}

int gfx_print(Screen s, int x, int y, GfxFont font, uint32_t color,
              const char *fmt, ...) {
  const Font *f = font_of(font);
  va_list ap;
  va_start(ap, fmt);
  const char *t = vfmt(fmt, ap);
  va_end(ap);
  return f ? font_text(s, x, y, f, color, t) : mono_text(s, x, y, 1, color, t);
}

int gfx_text_width(GfxFont font, const char *text) {
  const Font *f = font_of(font);
  int widest = 0;
  while (*text) {
    int w = 0;
    text = next_line(text);
    if (f) {
      w = text_width(f, g_line);
    } else {
      for (const char *p = g_line; *p; p++)
        if (((unsigned char)*p & 0xC0u) != 0x80u)
          w += FONT_WIDTH;
    }
    if (w > widest)
      widest = w;
  }
  return widest;
}

int gfx_line_height(GfxFont font) {
  const Font *f = font_of(font);
  return f ? text_line_height(f) : FONT_HEIGHT;
}

static const char *g_img_err = "";

const char *gfx_image_error(void) { return g_img_err; }

GfxImage *gfx_image_new(int w, int h) {
  GfxImage *img;
  if (w <= 0 || h <= 0 || w > IMAGE_MAX_DIM || h > IMAGE_MAX_DIM) {
    g_img_err = "bad image size";
    return NULL;
  }
  img = malloc(sizeof(*img));
  if (img)
    img->pixels = calloc((size_t)w * (size_t)h, sizeof(uint32_t));
  if (!img || !img->pixels) {
    free(img);
    g_img_err = "out of memory";
    return NULL;
  }
  img->w = w;
  img->h = h;
  return img;
}

void gfx_image_free(GfxImage *img) {
  if (img) {
    free(img->pixels);
    free(img);
  }
}

/* image_load() reads BMP itself, from IMAGE_FILE_ADDR; its PNG and JPEG
 * output has no alpha, so those go through image_decode_fit() at full size. */
static GfxImage *load_bmp(const char *path) {
  char fpath[FS_NAME_MAX + 8];
  Image src;
  ImageResult r;
  GfxImage *img;

  if (!app_fs_path(path, fpath, sizeof(fpath))) {
    g_img_err = "bad path";
    return NULL;
  }
  r = image_load(fpath, &src);
  if (r != IMG_OK) {
    g_img_err = image_error(r);
    return NULL;
  }
  img = gfx_image_new(src.w, src.h);
  if (!img)
    return NULL;
  for (int i = 0; i < src.w * src.h; i++) {
    const u8 *p = src.rgb + i * 3;
    img->pixels[i] = 0xFF000000u | ((uint32_t)p[0] << 16) |
                     ((uint32_t)p[1] << 8) | p[2];
  }
  return img;
}

GfxImage *gfx_image_load(const char *path) {
  size_t len = 0;
  u8 *data = fs_read_file(path, &len), *scratch;
  GfxImage *img;
  ImageResult r;
  int w = 0, h = 0;
  u32 scratch_len;

  if (!data) {
    g_img_err = "cannot read the file";
    return NULL;
  }
  if (len >= 2 && data[0] == 'B' && data[1] == 'M') {
    free(data);
    return load_bmp(path);
  }
  r = image_probe(data, (u32)len, &w, &h);
  if (r != IMG_OK) {
    free(data);
    g_img_err = image_error(r);
    return NULL;
  }
  img = gfx_image_new(w, h);
  /* Column map, accumulators, the inflate window and a few rows. */
  scratch_len = 65536u + 48u * (u32)w;
  scratch = img ? malloc(scratch_len) : NULL;
  if (!scratch) {
    free(data);
    gfx_image_free(img);
    g_img_err = "out of memory";
    return NULL;
  }
  r = image_decode_fit(data, (u32)len, (u8 *)img->pixels, w, h, scratch,
                       scratch_len);
  free(scratch);
  free(data);
  if (r != IMG_OK) {
    gfx_image_free(img);
    g_img_err = image_error(r);
    return NULL;
  }
  for (int i = 0; i < w * h; i++) {
    const u8 *p = (const u8 *)&img->pixels[i];
    img->pixels[i] = ((uint32_t)p[3] << 24) | ((uint32_t)p[0] << 16) |
                     ((uint32_t)p[1] << 8) | p[2];
  }
  g_img_err = "";
  return img;
}

static inline void blend(u8 *p, uint32_t v) {
  uint32_t a = v >> 24;
  if (a >= 255) {
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
    p[2] = (u8)(v >> 16);
  } else if (a) {
    int a8 = (int)(a + (a >> 7)), inv = 256 - a8;
    p[0] = (u8)(((int)(v & 0xFF) * a8 + p[0] * inv) >> 8);
    p[1] = (u8)(((int)((v >> 8) & 0xFF) * a8 + p[1] * inv) >> 8);
    p[2] = (u8)(((int)((v >> 16) & 0xFF) * a8 + p[2] * inv) >> 8);
  }
}

void gfx_image_part(Screen s, const GfxImage *img, int sx, int sy, int sw,
                    int sh, int x, int y) {
  int w = gfx_width(s);
  u8 *fb;

  if (!img || !img->pixels)
    return;
  if (sx < 0) {
    sw += sx;
    x -= sx;
    sx = 0;
  }
  if (sy < 0) {
    sh += sy;
    y -= sy;
    sy = 0;
  }
  if (sw > img->w - sx)
    sw = img->w - sx;
  if (sh > img->h - sy)
    sh = img->h - sy;
  if (x < 0) {
    sw += x;
    sx -= x;
    x = 0;
  }
  if (y < 0) {
    sh += y;
    sy -= y;
    y = 0;
  }
  if (sw > w - x)
    sw = w - x;
  if (sh > SH - y)
    sh = SH - y;
  if (sw <= 0 || sh <= 0)
    return;
  fb = begin(s);
  for (int c = 0; c < sw; c++) {
    const uint32_t *src = img->pixels + (size_t)sy * img->w + sx + c;
    u8 *p = fb + (((x + c) * SH) + (SH - 1 - y)) * 3;
    for (int r = 0; r < sh; r++) {
      blend(p, *src);
      src += img->w;
      p -= 3;
    }
  }
}

void gfx_image(Screen s, const GfxImage *img, int x, int y) {
  if (img)
    gfx_image_part(s, img, 0, 0, img->w, img->h, x, y);
}

void gfx_image_scaled(Screen s, const GfxImage *img, int x, int y, int w,
                      int h) {
  int sw = gfx_width(s), c0, c1, r0, r1;
  u32 fx, fy;
  u8 *fb;

  if (!img || !img->pixels || w <= 0 || h <= 0)
    return;
  c0 = x < 0 ? -x : 0;
  c1 = w < sw - x ? w : sw - x;
  r0 = y < 0 ? -y : 0;
  r1 = h < SH - y ? h : SH - y;
  if (c0 >= c1 || r0 >= r1)
    return;
  /* Source steps in 16.16 fixed point; the ARM9 has no divider. */
  fx = ((u32)img->w << 16) / (u32)w;
  fy = ((u32)img->h << 16) / (u32)h;
  fb = begin(s);
  for (int c = c0; c < c1; c++) {
    const uint32_t *col = img->pixels + (((u32)c * fx) >> 16);
    u8 *p = fb + (((x + c) * SH) + (SH - 1 - (y + r0))) * 3;
    for (int r = r0; r < r1; r++) {
      blend(p, col[(size_t)(((u32)r * fy) >> 16) * (size_t)img->w]);
      p -= 3;
    }
  }
}
