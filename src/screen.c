

#include "aurora.h"
#include "assets.h"
#include "aurora_logo.h"
#include "font.h"
#include "icons.h"
#include "gpu.h"

static int console_x = 0;
static int console_y = 0;

#define CONSOLE_COLS (BOT_SCREEN_WIDTH / FONT_WIDTH)   
#define CONSOLE_ROWS (BOT_SCREEN_HEIGHT / FONT_HEIGHT) 

static Color console_fg = {0xFF, 0xFF, 0xFF};
static Color console_bg = {0x10, 0x10, 0x20};

volatile u8 *g_fb_top = VRAM_TOP_PHYS;
volatile u8 *g_fb_bot = VRAM_BOT_PHYS;

int (*g_screen_blit)(u32 src, u32 dst, u32 len) = 0;
int (*g_screen_intercept)(volatile u8 *back) = 0;

volatile u8 *g_blit_src = 0;
void (*g_screen_wait)(void) = 0;

int screen_fb_width(volatile u8 *fb) {
  u32 a = (u32)fb;
  return (fb == VRAM_TOP_BACK || a == GPU_FB_TOP_A || a == GPU_FB_TOP_B ||
          a == GPU_FB_TOP_C)
             ? TOP_SCREEN_WIDTH
             : BOT_SCREEN_WIDTH;
}

static void copy_words(volatile u8 *dst, const volatile u8 *src, u32 size) {
  volatile u32 *d = (volatile u32 *)dst;
  const volatile u32 *s = (const volatile u32 *)src;
  for (u32 i = 0; i < (size >> 2); i++)
    d[i] = s[i];
}

void screen_use_backbuffer(int on) {
  if (on && g_fb_top != VRAM_TOP_BACK) {
    /* Seed the backbuffers from what is currently on the panels, so a present
     * that only redraws part of a screen cannot flash uninitialised memory. */
    copy_words(VRAM_TOP_BACK, VRAM_TOP_PHYS, TOP_FB_SIZE);
    copy_words(VRAM_BOT_BACK, VRAM_BOT_PHYS, BOT_FB_SIZE);
  }
  g_fb_top = on ? VRAM_TOP_BACK : VRAM_TOP_PHYS;
  g_fb_bot = on ? VRAM_BOT_BACK : VRAM_BOT_PHYS;
}

static void present(volatile u8 *back, volatile u8 *phys, u32 size) {
  if (back == phys)
    return;
  if (g_screen_intercept && g_screen_intercept(back))
    return;
  screen_touch(back); /* a previous copy may still be reading this buffer */
  if (g_screen_blit && g_screen_blit((u32)back, (u32)phys, size)) {
    if (g_screen_wait)
      g_blit_src = back; /* posted, not finished: guard it until collected */
    return;
  }
  /* No GPU: copy by word. */
  const u32 *s = (const u32 *)back;
  volatile u32 *d = (volatile u32 *)phys;
  for (u32 i = 0; i < (size >> 2); i++)
    d[i] = s[i];
}

void screen_init(void) {
  Color top_bg = {0x05, 0x0A, 0x15};
  clear_screen(VRAM_TOP_LA, TOP_FB_SIZE, top_bg);

  clear_screen(VRAM_BOT_A, BOT_FB_SIZE, COLOR_BG_DARK);
  screen_present_top();
  screen_present_bottom();
}

void screen_present_top(void) { present(g_fb_top, VRAM_TOP_PHYS, TOP_FB_SIZE); }

void screen_present_bottom(void) {
  present(g_fb_bot, VRAM_BOT_PHYS, BOT_FB_SIZE);
}

void clear_screen(volatile u8 *fb, u32 fb_size, Color color) {
  screen_touch(fb);
  u32 b = color.b, g = color.g, r = color.r;
  u32 w0 = b | (g << 8) | (r << 16) | (b << 24);
  u32 w1 = g | (r << 8) | (b << 16) | (g << 24);
  u32 w2 = r | (b << 8) | (g << 16) | (r << 24);
  volatile u32 *p = (volatile u32 *)fb;
  volatile u32 *end = p + (fb_size >> 2);
  while (p < end) {
    p[0] = w0;
    p[1] = w1;
    p[2] = w2;
    p += 3;
  }
}

void draw_pixel(volatile u8 *fb, int x, int y, int screen_height, Color color) {
  screen_touch(fb);
  if (x < 0 || y < 0 || y >= screen_height || x >= screen_fb_width(fb))
    return;

  u32 offset =
      ((x * screen_height) + (screen_height - 1 - y)) * BYTES_PER_PIXEL;
  fb[offset + 0] = color.b;
  fb[offset + 1] = color.g;
  fb[offset + 2] = color.r;
}

/* The framebuffer runs down a column, so a glyph column is one contiguous run
 * walked with a pointer. */
void draw_char(volatile u8 *fb, int x, int y, int screen_height, char c,
               Color fg, Color bg) {
  screen_touch(fb);
  if (c < 0x20 || c > 0x7E)
    return;

  const u8 *glyph = font_data[c - 0x20];

  int r0 = 0, r1 = FONT_HEIGHT; /* clip vertically once, not per pixel */
  if (y < 0)
    r0 = -y;
  if (y + r1 > screen_height)
    r1 = screen_height - y;
  if (r0 >= r1)
    return;

  int fbw = screen_fb_width(fb);
  for (int col = 0; col < FONT_WIDTH; col++) {
    int px = x + col;
    if (px < 0)
      continue;
    if (px >= fbw)
      break; /* past the right edge is the next buffer in memory */
    volatile u8 *p =
        fb + ((px * screen_height) + (screen_height - 1 - (y + r0))) * 3;
    u8 mask = (u8)(0x80 >> col);
    for (int row = r0; row < r1; row++) {
      Color pixel = (glyph[row] & mask) ? fg : bg;
      p[0] = pixel.b;
      p[1] = pixel.g;
      p[2] = pixel.r;
      p -= 3;
    }
  }
}

void draw_string(volatile u8 *fb, int x, int y, int screen_height,
                 const char *str, Color fg, Color bg) {
  int cx = x;
  while (*str) {
    if (*str == '\n') {
      cx = x;
      y += FONT_HEIGHT;
    } else if (((unsigned char)*str & 0xC0u) == 0x80u) {
      /* UTF-8 continuation byte: the lead byte already took the cell, so an
       * accented letter the 8x8 font lacks leaves one gap, not two. */
    } else {
      draw_char(fb, cx, y, screen_height, *str, fg, bg);
      cx += FONT_WIDTH;
    }
    str++;
  }
}

void draw_aurora_logo(volatile u8 *fb, int x0, int y0, int screen_height,
                      Color color) {
  screen_touch(fb);
  int r0 = 0, r1 = AURORA_LOGO_HEIGHT;
  if (y0 < 0)
    r0 = -y0;
  if (y0 + r1 > screen_height)
    r1 = screen_height - y0;
  if (r0 >= r1)
    return;

  u8 b = color.b, g = color.g, r = color.r;
  for (int col = 0; col < AURORA_LOGO_WIDTH; col++) {
    int px = x0 + col;
    if (px < 0)
      continue;
    volatile u8 *p =
        fb + ((px * screen_height) + (screen_height - 1 - (y0 + r0))) * 3;
    int byte_col = col >> 3;
    u8 mask = (u8)(0x80 >> (col & 7));
    for (int row = r0; row < r1; row++) {
      if (aurora_logo_bits[row * AURORA_LOGO_ROW_BYTES + byte_col] & mask) {
        p[0] = b;
        p[1] = g;
        p[2] = r;
      }
      p -= 3;
    }
  }
}

/* Copy a run, in words once both sides reach a common alignment. The
 * framebuffer is 3 bytes per pixel, so a column run is rarely word-aligned to
 * begin with. */
static void store_run(volatile u8 *d, const u8 *s, u32 n) {
  /* Both sides must be able to reach the same alignment. An unaligned LDR on
   * ARMv5 rotates the word instead of faulting, so a mismatch here does not
   * crash, it silently permutes the colour channels: solid fills come out as
   * horizontal stripes. */
  if ((((u32)d ^ (u32)s) & 3u) != 0u) {
    while (n--)
      *d++ = *s++;
    return;
  }
  while (n && ((u32)d & 3u)) {
    *d++ = *s++;
    n--;
  }
  volatile u32 *dw = (volatile u32 *)d;
  const u32 *sw = (const u32 *)s;
  u32 words = n >> 2;
  for (u32 i = 0; i < words; i++)
    dw[i] = sw[i];
  d += words << 2;
  s += words << 2;
  for (n &= 3u; n; n--)
    *d++ = *s++;
}

/* One framebuffer column, reused across a fill. */
static u8 col_buf[TOP_SCREEN_HEIGHT * BYTES_PER_PIXEL + 4]
    __attribute__((aligned(4)));

/* Every column of a fill starts screen_height*3 bytes after the last, and that
 * stride is a multiple of four, so one skew serves them all. Building the
 * column at that offset lets store_run take its word path. */
static u32 col_skew(int x, int screen_height, int y, int rows) {
  return (((u32)x * (u32)screen_height + (u32)(screen_height - y - rows)) * 3u) &
         3u;
}

void draw_filled_rect(volatile u8 *fb, int x, int y, int w, int h,
                      int screen_height, Color color) {
  screen_touch(fb);
  if (y < 0) {
    h += y;
    y = 0;
  }
  if (y + h > screen_height)
    h = screen_height - y;
  if (w <= 0 || h <= 0)
    return;

  u32 skew = col_skew(x < 0 ? 0 : x, screen_height, y, h);
  for (int i = 0; i < h; i++) {
    col_buf[skew + i * 3 + 0] = color.b;
    col_buf[skew + i * 3 + 1] = color.g;
    col_buf[skew + i * 3 + 2] = color.r;
  }
  u32 run = (u32)h * 3u;
  int wmax = screen_fb_width(fb);
  for (int px = x; px < x + w && px < wmax; px++) {
    if (px < 0)
      continue;
    store_run(fb + ((u32)px * (u32)screen_height + (u32)(screen_height - y - h)) * 3u,
              col_buf + skew, run);
  }
}

void draw_filled_rect_alpha(volatile u8 *fb, int x, int y, int w, int h,
                            int screen_height, Color color, int alpha) {
  screen_touch(fb);
  if (alpha <= 0)
    return;
  if (alpha >= 256) {
    draw_filled_rect(fb, x, y, w, h, screen_height, color);
    return;
  }
  if (y < 0) {
    h += y;
    y = 0;
  }
  if (y + h > screen_height)
    h = screen_height - y;
  if (w <= 0 || h <= 0)
    return;

  int inv = 256 - alpha;
  int cb = color.b * alpha, cg = color.g * alpha, cr = color.r * alpha;
  for (int px = x; px < x + w; px++) {
    volatile u8 *p = fb + ((px * screen_height) + (screen_height - 1 - y)) * 3;
    for (int i = 0; i < h; i++) {
      p[0] = (u8)((cb + p[0] * inv) >> 8);
      p[1] = (u8)((cg + p[1] * inv) >> 8);
      p[2] = (u8)((cr + p[2] * inv) >> 8);
      p -= 3;
    }
  }
}

/* Edges are anti-aliased by coverage. Alpha runs 0..256 so a blend is a shift. */

static u32 isqrt32(u32 n) {
  u32 rem = 0, root = 0;
  for (int i = 0; i < 16; i++) {
    root <<= 1;
    rem = (rem << 2) | (n >> 30);
    n <<= 2;
    if (root < rem) {
      rem -= root + 1;
      root += 2;
    }
  }
  return root >> 1;
}

void draw_pixel_alpha(volatile u8 *fb, int x, int y, int screen_height,
                      Color color, int alpha) {
  screen_touch(fb);
  if (alpha <= 0 || x < 0 || y < 0 || y >= screen_height ||
      x >= screen_fb_width(fb))
    return;
  if (alpha >= 256) {
    draw_pixel(fb, x, y, screen_height, color);
    return;
  }
  u32 o = ((x * screen_height) + (screen_height - 1 - y)) * BYTES_PER_PIXEL;
  int inv = 256 - alpha;
  fb[o + 0] = (u8)((color.b * alpha + fb[o + 0] * inv) >> 8);
  fb[o + 1] = (u8)((color.g * alpha + fb[o + 1] * inv) >> 8);
  fb[o + 2] = (u8)((color.r * alpha + fb[o + 2] * inv) >> 8);
}

/* The framebuffer runs down a column, so the ramp is walked along that axis and
 * each shade computed once. */
void draw_vgradient(volatile u8 *fb, int x, int y, int w, int h,
                    int screen_height, Color top, Color bottom) {
  screen_touch(fb);
  int r0 = 0, r1 = h;

  if (y < 0)
    r0 = -y;
  if (y + r1 > screen_height)
    r1 = screen_height - y;
  if (r0 >= r1 || w <= 0)
    return;
  if (r1 > TOP_SCREEN_HEIGHT)
    r1 = TOP_SCREEN_HEIGHT;

  /* Build the column once. Addresses descend as the row number rises, so the
   * run starts at the bottom row and the buffer is filled in that order. */
  u32 skew = col_skew(x < 0 ? 0 : x, screen_height, y, r1);
  for (int row = r0; row < r1; row++) {
    int tt = (h > 1) ? (row * 255) / (h - 1) : 0;
    u32 i = skew + (u32)(r1 - 1 - row) * 3u;
    col_buf[i + 0] = (u8)((top.b * (255 - tt) + bottom.b * tt) / 255);
    col_buf[i + 1] = (u8)((top.g * (255 - tt) + bottom.g * tt) / 255);
    col_buf[i + 2] = (u8)((top.r * (255 - tt) + bottom.r * tt) / 255);
  }

  u32 run = (u32)(r1 - r0) * 3u;
  for (int px = x; px < x + w; px++) {
    if (px < 0)
      continue;
    store_run(fb + ((u32)px * (u32)screen_height +
                    (u32)(screen_height - y - r1)) * 3u,
              col_buf + skew, run);
  }
}

/* Rounded rectangles are drawn a column at a time, so each pixel is written
 * once and the body, sides and corners take their shade from one ramp. Every
 * arc is centred `r` pixels in from the edges, measured between pixels rather
 * than through their centres, so the curve meets the straight sides without a
 * step. A corner pixel's coverage comes from the distance between its centre
 * and the arc. */
#define ROUND_MAX_R 40
#define ROUND_SLOTS 12

/* For corner column i and row j, both counted in from the outside corner:
 * cov[i][j] (0..255) for j < full[i], opaque from full[i] on. */
typedef struct {
  int r;
  u8 full[ROUND_MAX_R];
  u8 cov[ROUND_MAX_R * ROUND_MAX_R];
} RoundTab;

static RoundTab round_tabs[ROUND_SLOTS];
static int round_next;

static const RoundTab *round_tab(int r) {
  RoundTab *t;
  int s, i, j;

  for (s = 0; s < ROUND_SLOTS; s++)
    if (round_tabs[s].r == r)
      return &round_tabs[s];
  t = &round_tabs[round_next];
  round_next = (round_next + 1) % ROUND_SLOTS;
  for (i = 0; i < r; i++) {
    for (j = 0; j < r; j++) {
      /* Twice the offsets from the arc's centre, so they are whole numbers;
       * the root comes back in 8.8 fixed point. */
      u32 dx = (u32)(2 * (r - i) - 1), dy = (u32)(2 * (r - j) - 1);
      int a = ((2 * r + 1) * 256 - (int)isqrt32((dx * dx + dy * dy) << 16)) >> 1;
      if (a >= 256)
        break;
      t->cov[i * ROUND_MAX_R + j] = (u8)(a < 0 ? 0 : a);
    }
    t->full[i] = (u8)j;
  }
  t->r = r;
  return t;
}

static void round_rect(volatile u8 *fb, int x, int y, int w, int h, int r,
                       int sh, Color top, Color bot) {
  const RoundTab *t = 0;
  int wmax = screen_fb_width(fb), r0 = 0, r1 = h, k;
  u32 skew;

  screen_touch(fb);
  if (r > w / 2)
    r = w / 2;
  if (r > h / 2)
    r = h / 2;
  if (r > ROUND_MAX_R)
    r = ROUND_MAX_R;
  if (y < 0)
    r0 = -y;
  if (y + h > sh)
    r1 = sh - y;
  if (w <= 0 || r0 >= r1)
    return;
  if (r > 0)
    t = round_tab(r);

  /* The visible rows' shades, laid out as a framebuffer column: the bottom row
   * first. */
  skew = col_skew(0, sh, y, r1);
  for (k = r0; k < r1; k++) {
    u8 *c = col_buf + skew + (u32)(r1 - 1 - k) * 3u;
    if (top.r == bot.r && top.g == bot.g && top.b == bot.b) {
      c[0] = top.b;
      c[1] = top.g;
      c[2] = top.r;
    } else {
      int tt = (h > 1) ? (k * 255) / (h - 1) : 0;
      c[0] = (u8)((top.b * (255 - tt) + bot.b * tt) / 255);
      c[1] = (u8)((top.g * (255 - tt) + bot.g * tt) / 255);
      c[2] = (u8)((top.r * (255 - tt) + bot.r * tt) / 255);
    }
  }

  for (int col = 0; col < w; col++) {
    int px = x + col, i = -1, n = 0, f0, f1;
    volatile u8 *base;
    if (px < 0)
      continue;
    if (px >= wmax)
      break;
    if (col < r)
      i = col;
    else if (col >= w - r)
      i = w - 1 - col;
    if (i >= 0)
      n = t->full[i];

    base = fb + (u32)px * (u32)sh * 3u;
    f0 = n > r0 ? n : r0;
    f1 = h - n < r1 ? h - n : r1;
    if (f1 > f0)
      store_run(base + (u32)(sh - y - f1) * 3u,
                col_buf + skew + (u32)(r1 - f1) * 3u, (u32)(f1 - f0) * 3u);

    for (int j = 0; j < n; j++) {
      int a = t->cov[i * ROUND_MAX_R + j], inv = 256 - a;
      if (!a)
        continue;
      for (int e = 0; e < 2; e++) {
        int row = e ? h - 1 - j : j;
        volatile u8 *p;
        const u8 *c;
        if (row < r0 || row >= r1)
          continue;
        p = base + (u32)(sh - 1 - (y + row)) * 3u;
        c = col_buf + skew + (u32)(r1 - 1 - row) * 3u;
        p[0] = (u8)((c[0] * a + p[0] * inv) >> 8);
        p[1] = (u8)((c[1] * a + p[1] * inv) >> 8);
        p[2] = (u8)((c[2] * a + p[2] * inv) >> 8);
      }
    }
  }
}

void draw_filled_round_rect(volatile u8 *fb, int x, int y, int w, int h,
                            int radius, int screen_height, Color color) {
  round_rect(fb, x, y, w, h, radius < 0 ? 0 : radius, screen_height, color,
             color);
}

void draw_gradient_round_rect(volatile u8 *fb, int x, int y, int w, int h,
                              int radius, int screen_height, Color top,
                              Color bottom) {
  round_rect(fb, x, y, w, h, radius < 0 ? 0 : radius, screen_height, top,
             bottom);
}

/* The inner edge is the same shape inset by `t`, with radius r - t, so a
 * corner pixel's coverage is the outer shape's less the inner one's. */
void draw_round_ring(volatile u8 *fb, int x, int y, int w, int h, int r, int t,
                     int screen_height, Color color) {
  const RoundTab *to, *ti = 0;

  if (r > w / 2)
    r = w / 2;
  if (r > h / 2)
    r = h / 2;
  if (r > ROUND_MAX_R)
    r = ROUND_MAX_R;
  if (t > r)
    t = r;
  if (t <= 0 || 2 * t >= w || 2 * t >= h) {
    round_rect(fb, x, y, w, h, r < 0 ? 0 : r, screen_height, color, color);
    return;
  }
  draw_filled_rect(fb, x + r, y, w - 2 * r, t, screen_height, color);
  draw_filled_rect(fb, x + r, y + h - t, w - 2 * r, t, screen_height, color);
  draw_filled_rect(fb, x, y + r, t, h - 2 * r, screen_height, color);
  draw_filled_rect(fb, x + w - t, y + r, t, h - 2 * r, screen_height, color);

  to = round_tab(r);
  if (r > t)
    ti = round_tab(r - t);
  for (int i = 0; i < r; i++) {
    for (int j = 0; j < r; j++) {
      int a = j >= to->full[i] ? 256 : to->cov[i * ROUND_MAX_R + j];
      int ii = i - t, jj = j - t;
      if (ii >= 0 && jj >= 0)
        a -= !ti || jj >= ti->full[ii] ? 256 : ti->cov[ii * ROUND_MAX_R + jj];
      if (a <= 0)
        continue;
      draw_pixel_alpha(fb, x + i, y + j, screen_height, color, a);
      draw_pixel_alpha(fb, x + w - 1 - i, y + j, screen_height, color, a);
      draw_pixel_alpha(fb, x + i, y + h - 1 - j, screen_height, color, a);
      draw_pixel_alpha(fb, x + w - 1 - i, y + h - 1 - j, screen_height, color,
                       a);
    }
  }
}

void draw_icon_32(volatile u8 *fb, int x, int y, int screen_height,
                  const unsigned char *icon_bits, Color color) {
  screen_touch(fb);
  int r0 = 0, r1 = ICON_SIZE;
  if (y < 0)
    r0 = -y;
  if (y + r1 > screen_height)
    r1 = screen_height - y;
  if (r0 >= r1)
    return;

  u8 b = color.b, g = color.g, r = color.r;
  for (int col = 0; col < ICON_SIZE; col++) {
    int px = x + col;
    if (px < 0)
      continue;
    volatile u8 *p =
        fb + ((px * screen_height) + (screen_height - 1 - (y + r0))) * 3;
    int byte_col = col >> 3;
    u8 mask = (u8)(0x80 >> (col & 7));
    for (int row = r0; row < r1; row++) {
      if (icon_bits[row * ICON_ROW_BYTES + byte_col] & mask) {
        p[0] = b;
        p[1] = g;
        p[2] = r;
      }
      p -= 3;
    }
  }
}

/* Upscaled 1bpp art, smoothed by bilinear filtering: at an integer scale,
 * supersampling would only ever give zero or full coverage. */

/* One source pixel as 0 or 256; anything off the edge reads as empty. */
static int bitmap_texel(const unsigned char *bits, int row_bytes, int size,
                        int col, int row) {
  if (col < 0 || row < 0 || col >= size || row >= size)
    return 0;
  return (bits[row * row_bytes + (col >> 3)] & (0x80 >> (col & 7))) ? 256 : 0;
}

static void draw_bitmap_smooth(volatile u8 *fb, int x, int y, int screen_height,
                               const unsigned char *bits, int size,
                               int row_bytes, Color color, int scale) {
  int dim = size * scale;

  for (int dy = 0; dy < dim; dy++) {
    int py = y + dy;
    /* Source coordinate of this pixel's centre, 8.8 fixed point. */
    int fy = ((dy * 2 + 1) * 128) / scale - 128;
    int sy0 = fy >> 8, ty = fy & 255;
    if (py < 0 || py >= screen_height)
      continue;
    for (int dx = 0; dx < dim; dx++) {
      int px = x + dx;
      int fx, sx0, tx, top, bot, a;
      if (px < 0)
        continue;
      fx = ((dx * 2 + 1) * 128) / scale - 128;
      sx0 = fx >> 8;
      tx = fx & 255;
      top = (bitmap_texel(bits, row_bytes, size, sx0, sy0) * (256 - tx) +
             bitmap_texel(bits, row_bytes, size, sx0 + 1, sy0) * tx) >> 8;
      bot = (bitmap_texel(bits, row_bytes, size, sx0, sy0 + 1) * (256 - tx) +
             bitmap_texel(bits, row_bytes, size, sx0 + 1, sy0 + 1) * tx) >> 8;
      a = (top * (256 - ty) + bot * ty) >> 8;
      if (a > 0)
        draw_pixel_alpha(fb, px, py, screen_height, color, a);
    }
  }
}

void draw_icon_scaled(volatile u8 *fb, int x, int y, int screen_height,
                      const unsigned char *icon_bits, Color color, int scale) {
  if (scale <= 1) { /* 1:1 has nothing to smooth, and stays crisp */
    draw_icon_32(fb, x, y, screen_height, icon_bits, color);
    return;
  }
  draw_bitmap_smooth(fb, x, y, screen_height, icon_bits, ICON_SIZE,
                     ICON_ROW_BYTES, color, scale);
}

/* Text at a larger size, smoothed the same way. */
void draw_string_scaled(volatile u8 *fb, int x, int y, int screen_height,
                        const char *str, Color color, int scale) {
  int cx = x;
  if (scale <= 1) {
    return; /* callers wanting 1:1 use draw_string, which sets a background */
  }
  while (*str) {
    if (*str == '\n') {
      cx = x;
      y += FONT_HEIGHT * scale;
    } else {
      if (*str >= 0x20 && *str <= 0x7E)
        draw_bitmap_smooth(fb, cx, y, screen_height, font_data[*str - 0x20],
                           FONT_HEIGHT, 1, color, scale);
      cx += FONT_WIDTH * scale;
    }
    str++;
  }
}

/* Pack art is stored at the size it is drawn, so these are straight blits.
 * Coverage 0..255 is widened to 0..256 so an opaque pixel is a plain store. */

/* One tinted coverage rectangle, walked down framebuffer columns. */
static void blit_cov(volatile u8 *fb, int x, int y, int screen_height,
                     const u8 *src, int stride, int w, int h, Color c) {
  int r0 = 0, r1 = h, col, wmax = screen_fb_width(fb);
  if (y < 0)
    r0 = -y;
  if (y + r1 > screen_height)
    r1 = screen_height - y;
  if (r0 >= r1)
    return;

  for (col = 0; col < w; col++) {
    int px = x + col, row;
    const u8 *s;
    volatile u8 *p;
    if (px < 0)
      continue;
    if (px >= wmax)
      break;
    s = src + r0 * stride + col;
    p = fb + ((px * screen_height) + (screen_height - 1 - (y + r0))) * 3;
    for (row = r0; row < r1; row++) {
      int a = *s;
      if (a) {
        if (a >= 255) {
          p[0] = c.b;
          p[1] = c.g;
          p[2] = c.r;
        } else {
          int a8 = a + (a >> 7), inv = 256 - a8;
          p[0] = (u8)((c.b * a8 + p[0] * inv) >> 8);
          p[1] = (u8)((c.g * a8 + p[1] * inv) >> 8);
          p[2] = (u8)((c.r * a8 + p[2] * inv) >> 8);
        }
      }
      s += stride;
      p -= 3;
    }
  }
}

static void blit_rgba(volatile u8 *fb, int x, int y, int screen_height,
                      const u8 *src, int w, int h) {
  int r0 = 0, r1 = h, col, wmax = screen_fb_width(fb);
  if (y < 0)
    r0 = -y;
  if (y + r1 > screen_height)
    r1 = screen_height - y;
  if (r0 >= r1)
    return;

  for (col = 0; col < w; col++) {
    int px = x + col, row;
    const u8 *s;
    volatile u8 *p;
    if (px < 0)
      continue;
    if (px >= wmax)
      break;
    s = src + (r0 * w + col) * 4;
    p = fb + ((px * screen_height) + (screen_height - 1 - (y + r0))) * 3;
    for (row = r0; row < r1; row++) {
      int a = s[3];
      if (a) {
        if (a >= 255) {
          p[0] = s[2];
          p[1] = s[1];
          p[2] = s[0];
        } else {
          int a8 = a + (a >> 7), inv = 256 - a8;
          p[0] = (u8)((s[2] * a8 + p[0] * inv) >> 8);
          p[1] = (u8)((s[1] * a8 + p[1] * inv) >> 8);
          p[2] = (u8)((s[0] * a8 + p[2] * inv) >> 8);
        }
      }
      s += w * 4;
      p -= 3;
    }
  }
}

void draw_asset(volatile u8 *fb, int x, int y, int screen_height,
                const Asset *a, Color tint) {
  screen_touch(fb);
  if (!a || !a->data)
    return;
  if (a->type == ASSET_TYPE_RGBA)
    blit_rgba(fb, x, y, screen_height, a->data, a->w, a->h);
  else
    blit_cov(fb, x, y, screen_height, a->data, a->w, a->w, a->h, tint);
}

/* Glyphs are coverage rendered from Figtree, drawn with the same blit. `y` is
 * the baseline. */

/* A malformed sequence is taken a byte at a time; the checks stop at a NUL. */
static unsigned utf8_next(const char **s) {
  const unsigned char *p = (const unsigned char *)*s;
  unsigned c = p[0];
  if (c >= 0xC0u && c < 0xE0u && (p[1] & 0xC0u) == 0x80u) {
    *s += 2;
    return ((c & 0x1Fu) << 6) | (p[1] & 0x3Fu);
  }
  if (c >= 0xE0u && c < 0xF0u && (p[1] & 0xC0u) == 0x80u &&
      (p[2] & 0xC0u) == 0x80u) {
    *s += 3;
    return ((c & 0x0Fu) << 12) | ((p[1] & 0x3Fu) << 6) | (p[2] & 0x3Fu);
  }
  *s += 1;
  return c;
}

static const AssetGlyph *glyph_of(const Font *f, unsigned cp) {
  unsigned i;
  if (!f || !f->head)
    return 0;
  i = cp - f->head->first; /* wraps high below the first code point */
  return i < f->head->count ? &f->glyphs[i] : 0;
}

int text_width(const Font *f, const char *s) {
  int w = 0;
  while (s && *s) {
    const AssetGlyph *g = glyph_of(f, utf8_next(&s));
    if (g)
      w += g->adv;
  }
  return w;
}

int text_line_height(const Font *f) {
  return (f && f->head) ? f->head->line_height : FONT_HEIGHT;
}

int text_ascent(const Font *f) {
  return (f && f->head) ? f->head->ascent : FONT_HEIGHT;
}

void draw_text(volatile u8 *fb, int x, int baseline, int screen_height,
               const char *s, Color color, const Font *f) {
  screen_touch(fb);
  if (!f || !f->head)
    return;
  while (s && *s) {
    const AssetGlyph *g = glyph_of(f, utf8_next(&s));
    if (!g)
      continue;
    if (g->w && g->h)
      blit_cov(fb, x + g->left, baseline - g->top, screen_height,
               f->atlas + (u32)g->ay * f->head->atlas_w + g->ax,
               f->head->atlas_w, g->w, g->h, color);
    x += g->adv;
  }
}

void draw_text_centered(volatile u8 *fb, int cx, int baseline,
                        int screen_height, const char *s, Color color,
                        const Font *f) {
  draw_text(fb, cx - text_width(f, s) / 2, baseline, screen_height, s, color, f);
}

static void console_scroll(void) {
  volatile u8 *fb = VRAM_BOT_A;

  
  for (int x = 0; x < BOT_SCREEN_WIDTH; x++) {
    for (int y = 0; y < BOT_SCREEN_HEIGHT - FONT_HEIGHT; y++) {
      u32 dst_off = ((x * BOT_SCREEN_HEIGHT) + (BOT_SCREEN_HEIGHT - 1 - y)) * 3;
      u32 src_off = ((x * BOT_SCREEN_HEIGHT) +
                     (BOT_SCREEN_HEIGHT - 1 - (y + FONT_HEIGHT))) *
                    3;
      fb[dst_off + 0] = fb[src_off + 0];
      fb[dst_off + 1] = fb[src_off + 1];
      fb[dst_off + 2] = fb[src_off + 2];
    }
    
    for (int y = BOT_SCREEN_HEIGHT - FONT_HEIGHT; y < BOT_SCREEN_HEIGHT; y++) {
      u32 off = ((x * BOT_SCREEN_HEIGHT) + (BOT_SCREEN_HEIGHT - 1 - y)) * 3;
      fb[off + 0] = console_bg.b;
      fb[off + 1] = console_bg.g;
      fb[off + 2] = console_bg.r;
    }
  }
}

void console_init(void) {
  console_x = 0;
  console_y = 0;
  console_fg = COLOR_WHITE;
  console_bg = (Color){0x10, 0x10, 0x20};

  
  clear_screen(VRAM_BOT_A, BOT_FB_SIZE, console_bg);
}

void console_print(const char *str) { console_print_color(str, console_fg); }

void console_print_color(const char *str, Color color) {
  while (*str) {
    if (*str == '\n') {
      console_x = 0;
      console_y++;
    } else {
      
      int px = console_x * FONT_WIDTH;
      int py = console_y * FONT_HEIGHT;
      draw_char(VRAM_BOT_A, px, py, BOT_SCREEN_HEIGHT, *str, color, console_bg);
      console_x++;

      
      if (console_x >= CONSOLE_COLS) {
        console_x = 0;
        console_y++;
      }
    }

    
    if (console_y >= CONSOLE_ROWS) {
      console_scroll();
      console_y = CONSOLE_ROWS - 1;
      console_x = 0;
    }

    str++;
  }
}
