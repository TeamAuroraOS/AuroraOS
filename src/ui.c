#include "ui.h"
#include "assets.h"
#include "icons.h"
#include "font.h"
#include "gpu.h"
#include "timer.h"
#include <string.h>

Color g_accent = COLOR_AURORA;

extern void delay(volatile u32 cycles);
extern void os_dcache_flush(void);

/* Ends one input-loop pass: collects any GPU blit still in flight, then waits
 * out the frame. A caller must pass through here before drawing into a
 * presented buffer. */
#define UI_FRAME_US 1000u

void ui_idle(void) {
  static u32 frame_ticks;
  u32 t0;

  gpu_wait_idle();
  g_blit_src = 0; /* collected here, so the per-draw guard stays a bare compare */
  if (!timer_calibrated()) {
    delay(7000); /* no calibrated timer: roughly a millisecond of nops */
    return;
  }
  /* Converted once. timer_us_since() divides, and the ARM9 has no divider, so
   * doing that per iteration of a spin loop would cost more than the wait. */
  if (!frame_ticks) {
    frame_ticks =
        (u32)(((unsigned long long)UI_FRAME_US * timer_hz()) / 1000000u);
    if (!frame_ticks)
      frame_ticks = 1;
  }
  t0 = timer_ticks();
  while (timer_ticks() - t0 < frame_ticks)
    ;
}

#define UI_NO_ASSET 0xFFFFFFFFu

Font ui_small, ui_font, ui_bold, ui_title;
int ui_assets;

void ui_init(void) {
  ui_assets = assets_load();
  if (!ui_assets)
    return;
  asset_font(ASSET_FONT_SMALL, &ui_small);
  asset_font(ASSET_FONT_UI, &ui_font);
  asset_font(ASSET_FONT_BOLD, &ui_bold);
  asset_font(ASSET_FONT_TITLE, &ui_title);
}

int ui_have(const Font *f) { return ui_assets && f->head != 0; }

/* Characters, not bytes: the fallback font draws one cell per UTF-8 sequence. */
static int utf8_chars(const char *s) {
  int n = 0;
  for (; *s; s++)
    if (((unsigned char)*s & 0xC0u) != 0x80u)
      n++;
  return n;
}

int ui_tw(const Font *f, const char *s) {
  return ui_have(f) ? text_width(f, s) : utf8_chars(s) * FONT_WIDTH;
}

int ui_th(const Font *f) {
  return ui_have(f) ? text_line_height(f) : FONT_HEIGHT;
}

void ui_text(volatile u8 *fb, int x, int y, int sh, const char *s,
                    Color fg, Color bg, const Font *f) {
  if (ui_have(f))
    draw_text(fb, x, y + text_ascent(f), sh, s, fg, f);
  else
    draw_string(fb, x, y, sh, s, fg, bg);
}

void ui_text_mid(volatile u8 *fb, int cx, int y, int sh, const char *s,
                        Color fg, Color bg, const Font *f) {
  ui_text(fb, cx - ui_tw(f, s) / 2, y, sh, s, fg, bg, f);
}

void ui_icon(volatile u8 *fb, int bx, int by, int box, int sh, u32 asset,
                    const unsigned char *bits, Color tint) {
  if (asset != UI_NO_ASSET && draw_asset_boxed(fb, bx, by, box, box, sh, asset,
                                               tint))
    return;
  if (bits) {
    int s = box / ICON_SIZE;
    if (s < 1)
      s = 1;
    draw_icon_scaled(fb, bx + (box - ICON_SIZE * s) / 2,
                     by + (box - ICON_SIZE * s) / 2, sh, bits, tint, s);
  }
}

/* Somewhere clear of the app-return descriptor at 0x25008000 and the image
 * decoder buffers from 0x25100000. Live for as long as the OS runs. */
#define UI_BG_TOP_ADDR 0x25010000u /* 400x240x3 = 288,000 -> 0x25056500 */
#define UI_BG_BOT_ADDR 0x25060000u /* 320x240x3 = 230,400 -> 0x25098400 */

static u8 *const bg_top = (u8 *)UI_BG_TOP_ADDR;
static u8 *const bg_bot = (u8 *)UI_BG_BOT_ADDR;
static int bg_ready;

/* The theme gradient with the pack's pattern tinted over it, or diagonal facets
 * without the pack. Per-pixel and slow, so it runs once. */
static void bg_compose(u8 *dst, int w, int h) {
  const Asset *a = asset_get(ASSET_WALLPAPER);
  Color ramp[TOP_SCREEN_HEIGHT];
  u8 ar = g_accent.r, ag = g_accent.g, ab = g_accent.b;

  for (int i = 0; i < h; i++) {
    int tt = (h > 1) ? (i * 255) / (h - 1) : 0;
    ramp[i].r = (u8)((COLOR_HM_BG_TOP.r * (255 - tt) + COLOR_HM_BG_BOT.r * tt) / 255);
    ramp[i].g = (u8)((COLOR_HM_BG_TOP.g * (255 - tt) + COLOR_HM_BG_BOT.g * tt) / 255);
    ramp[i].b = (u8)((COLOR_HM_BG_TOP.b * (255 - tt) + COLOR_HM_BG_BOT.b * tt) / 255);
  }

  for (int px = 0; px < w; px++) {
    u8 *p = dst + ((u32)px * (u32)h + (u32)(h - 1)) * 3u;
    const u8 *s = (a && px < (int)a->w) ? a->data + px : 0;
    for (int py = 0; py < h; py++) {
      Color c = ramp[py];
      int al = (s && py < (int)a->h) ? s[(u32)py * a->w] : 0;
      if (!al) {
        if (!a) {
          int d1 = px - py, d2 = px + py - h;
          if ((d1 % 46 == 0 && d1 >= -h && d1 < TOP_SCREEN_WIDTH) ||
              (d2 % 46 == 0 && d2 >= -h && d2 < TOP_SCREEN_WIDTH))
            c = COLOR_HM_FACET;
        }
        p[0] = c.b;
        p[1] = c.g;
        p[2] = c.r;
      } else {
        int a8 = al + (al >> 7), inv = 256 - a8;
        p[0] = (u8)((ab * a8 + c.b * inv) >> 8);
        p[1] = (u8)((ag * a8 + c.g * inv) >> 8);
        p[2] = (u8)((ar * a8 + c.r * inv) >> 8);
      }
      p -= 3;
    }
  }
}

void ui_bg_build(void) {
  bg_compose(bg_top, TOP_SCREEN_WIDTH, TOP_SCREEN_HEIGHT);
  bg_compose(bg_bot, BOT_SCREEN_WIDTH, BOT_SCREEN_HEIGHT);
  bg_ready = 1;
}

void ui_bg_invalidate(void) { bg_ready = 0; }

static const u8 *bg_for(int w) {
  if (!bg_ready)
    ui_bg_build();
  return (w >= TOP_SCREEN_WIDTH) ? bg_top : bg_bot;
}

/* Copy a run, in words once the two sides reach a common alignment. Both
 * buffers share the same layout, so they share the same alignment too. */
static void copy_run(volatile u8 *d, const u8 *s, u32 n) {
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

void ui_wallpaper(volatile u8 *fb, int w, int h, int sh) {
  const u8 *bg = bg_for(w);
  u32 size = (u32)w * (u32)h * 3u;

  screen_touch(fb);
  /* Flush by index before the GPU writes, so blended drawing cannot read the
   * previous frame back from the cache and nothing dirty can overwrite the
   * GPU's output. */
  os_dcache_flush();
  if (gpu_alive() && gpu_texcopy((u32)bg, (u32)fb, size))
    return;
  copy_run(fb, bg, size);
  (void)sh;
}

/* In this column-major framebuffer a rect is w runs of h*3 bytes at the same
 * offset in both buffers, so it is a straight copy. */
void ui_wallpaper_rect(volatile u8 *fb, int x, int y, int w, int h, int sh) {
  const u8 *bg;

  if (x < 0) {
    w += x;
    x = 0;
  }
  if (y < 0) {
    h += y;
    y = 0;
  }
  if (y + h > sh)
    h = sh - y;
  if (w <= 0 || h <= 0)
    return;

  screen_touch(fb);
  bg = bg_for((fb == VRAM_TOP_PHYS || fb == VRAM_TOP_BACK) ? TOP_SCREEN_WIDTH
                                                           : BOT_SCREEN_WIDTH);
  for (int col = 0; col < w; col++) {
    u32 base = ((u32)(x + col) * (u32)sh + (u32)(sh - y - h)) * 3u;
    copy_run(fb + base, bg + base, (u32)h * 3u);
  }
}

/* Erases a `border`-wide frame and the blended corners of a rounded rect about
 * to be redrawn in place; blending corners over themselves would harden them. */
void ui_patch_round_rect(volatile u8 *fb, int x, int y, int w, int h, int r,
                         int border, int sh) {
  if (border > 0) {
    ui_wallpaper_rect(fb, x - border, y - border, w + 2 * border, border, sh);
    ui_wallpaper_rect(fb, x - border, y + h, w + 2 * border, border, sh);
    ui_wallpaper_rect(fb, x - border, y, border, h, sh);
    ui_wallpaper_rect(fb, x + w, y, border, h, sh);
  }
  if (r > 0) {
    ui_wallpaper_rect(fb, x, y, r, r, sh);
    ui_wallpaper_rect(fb, x + w - r, y, r, r, sh);
    ui_wallpaper_rect(fb, x, y + h - r, r, r, sh);
    ui_wallpaper_rect(fb, x + w - r, y + h - r, r, r, sh);
  }
}

/* Drawn rather than blitted, so it works at any size. */
void ui_dialog(volatile u8 *fb, int w, int h, int sh, const char *title,
                      const char *subtitle, Color title_color, int dim) {
  static const Color scrim = {0x00, 0x00, 0x00};
  static const Color card_top = COLOR_PANEL_TOP;
  static const Color card_bot = COLOR_PANEL_BOT;
  int dw = w - 56, dh = 92;
  int dx = (w - dw) / 2, dy = (h - dh) / 2;

  if (dim)
    draw_filled_rect_alpha(fb, 0, 0, w, h, sh, scrim, 150);
  draw_gradient_round_rect(fb, dx, dy, dw, dh, 14, sh, card_top, card_bot);
  ui_text_mid(fb, w / 2, dy + 24, sh, title, title_color, card_top, &ui_title);
  if (subtitle && subtitle[0])
    ui_text_mid(fb, w / 2, dy + 54, sh, subtitle, COLOR_HM_TEXT2, card_bot,
                &ui_font);
}
