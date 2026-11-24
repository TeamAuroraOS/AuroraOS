/* Resident ARM11 minimal Tesla-inspired Aurora Menu sidebar & /Aurora/amenu host.
 * Uses the OS's anti-aliased Figtree fonts from ASSETS_ARENA_ADDR (falling back
 * to the built-in 8x8 font if assets.pak is absent). */
#include "amenu.h"
#include "assets.h"
#include "audio.h"
#include "core11.h"
#include "font.h"
#include "gpu.h"
#include "power.h"
#include "system_menu.h"

#define HID_PAD_ADDR 0x10146000u
#define KEY_A        (1u << 0)
#define KEY_B        (1u << 1)
#define KEY_SELECT   (1u << 2)
#define KEY_START    (1u << 3)
#define KEY_RIGHT    (1u << 4)
#define KEY_LEFT     (1u << 5)
#define KEY_UP       (1u << 6)
#define KEY_DOWN     (1u << 7)
#define KEY_R        (1u << 8)
#define KEY_L        (1u << 9)
#define KEY_X        (1u << 10)
#define KEY_Y        (1u << 11)
#define KEY_CHORD    (KEY_L | KEY_DOWN | KEY_SELECT)

#define TOP_FB_ADDR_A 0x18300000u
#define TOP_FB_ADDR_B 0x18400000u
#define TOP_FB_ADDR_C 0x18500000u
#define BOT_FB_ADDR_A 0x18346500u
#define BOT_FB_ADDR_B 0x18446500u
#define BOT_FB_ADDR_C 0x18546500u

#define TOP_WIDTH     400
#define TOP_HEIGHT    240
#define BOT_WIDTH     320
#define BOT_HEIGHT    240

#define SIDEBAR_W     AMENU_SIDEBAR_W
#define HEADER_H      38
#define LIST_Y        44
#define ROW_H         22
#define VISIBLE_ROWS  7
#define FOOTER_Y      208

#define COL_BG        0x0E1015u
#define COL_SEL       0x1E2230u
#define COL_BORDER    0x242834u
#define COL_HAIRLINE  0x1D212Cu
#define COL_WHITE     0xF4F6FAu
#define COL_MUTED     0xA4ACB9u
#define COL_DIM       0x6C7382u
#define COL_GREEN     0x4CD964u
#define COL_RED       0xFF5A52u

static uint32_t menu_chord_latched;
static uint32_t hud_last_us;
static uint32_t hud_present_us;
static volatile uint8_t *g_menu_fb;
static volatile uint8_t *g_prev_fb;
static int g_hud_target_screen = -1;
static volatile uint8_t *g_hud_target_fb;
static int g_buf[2];
static int g_cur_buf;
static int g_slide_x;
static int g_request_close;

static uint32_t menu_keys(void) {
  core11_sleep_poll();
  return ~MMIO32(HID_PAD_ADDR) & 0xFFFu;
}

static volatile uint8_t *top_framebuffer(int frame) {
  uint32_t a = frame == 0 ? TOP_FB_ADDR_A
             : frame == 1 ? TOP_FB_ADDR_B
                          : TOP_FB_ADDR_C;
  return (volatile uint8_t *)a;
}

static volatile uint8_t *bot_framebuffer(int frame) {
  uint32_t a = frame == 0 ? BOT_FB_ADDR_A
             : frame == 1 ? BOT_FB_ADDR_B
                          : BOT_FB_ADDR_C;
  return (volatile uint8_t *)a;
}

/* Prepare the hidden off-screen buffer before drawing a frame. */
static void menu_begin_frame(void) {
  g_menu_fb = bot_framebuffer(g_buf[g_cur_buf]);
}

/* Atomically flip the completed off-screen buffer onto the bottom LCD and
 * switch to the other spare buffer for the next frame (zero flicker). */
static void menu_swap_frame(void) {
  gpu11_menu_show(g_buf[g_cur_buf]);
  g_cur_buf ^= 1;
}

static uint32_t get_accent(void) {
  volatile AuroraSystemMenuShared *s = aurora_system_menu_shared();
  uint32_t c = s->accent_rgb;
  return c ? (c & 0xFFFFFFu) : 0x64E8C8u;
}

static uint32_t get_theme_rgb(uint32_t val, uint32_t def) {
  return val ? (val & 0xFFFFFFu) : def;
}

static uint32_t rgb_lerp(uint32_t a, uint32_t b, int t, int max) {
  if (max <= 0) return a;
  int ar = (int)((a >> 16) & 0xFFu), ag = (int)((a >> 8) & 0xFFu), ab = (int)(a & 0xFFu);
  int br = (int)((b >> 16) & 0xFFu), bg = (int)((b >> 8) & 0xFFu), bb = (int)(b & 0xFFu);
  int r = ar + ((br - ar) * t) / max;
  int g = ag + ((bg - ag) * t) / max;
  int bl = ab + ((bb - ab) * t) / max;
  return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)bl;
}

static uint32_t rgb_darken(uint32_t c, int num, int den) {
  int r = (int)((c >> 16) & 0xFFu) * num / den;
  int g = (int)((c >> 8) & 0xFFu) * num / den;
  int b = (int)(c & 0xFFu) * num / den;
  return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static void fb_pixel(volatile uint8_t *fb, int w, int h, int x, int y,
                     uint32_t rgb) {
  if ((uint32_t)x >= (uint32_t)w || (uint32_t)y >= (uint32_t)h)
    return;
  uint32_t off = ((uint32_t)x * (uint32_t)h + ((uint32_t)h - 1u - (uint32_t)y)) * 3u;
  fb[off]     = (uint8_t)(rgb & 0xFFu);
  fb[off + 1] = (uint8_t)((rgb >> 8) & 0xFFu);
  fb[off + 2] = (uint8_t)((rgb >> 16) & 0xFFu);
}

static void fb_pixel_alpha(volatile uint8_t *fb, int w, int h, int x, int y,
                           uint32_t rgb, int alpha) {
  if ((uint32_t)x >= (uint32_t)w || (uint32_t)y >= (uint32_t)h || alpha <= 0)
    return;
  if (alpha >= 255) {
    fb_pixel(fb, w, h, x, y, rgb);
    return;
  }
  uint32_t off = ((uint32_t)x * (uint32_t)h + ((uint32_t)h - 1u - (uint32_t)y)) * 3u;
  int inv = 256 - alpha;
  uint32_t sb = rgb & 0xFFu;
  uint32_t sg = (rgb >> 8) & 0xFFu;
  uint32_t sr = (rgb >> 16) & 0xFFu;
  fb[off]     = (uint8_t)((sb * (uint32_t)alpha + fb[off] * (uint32_t)inv) >> 8);
  fb[off + 1] = (uint8_t)((sg * (uint32_t)alpha + fb[off + 1] * (uint32_t)inv) >> 8);
  fb[off + 2] = (uint8_t)((sr * (uint32_t)alpha + fb[off + 2] * (uint32_t)inv) >> 8);
}

static void fb_rect(volatile uint8_t *fb, int fw, int fh, int x, int y,
                    int w, int h, uint32_t rgb) {
  for (int yy = y; yy < y + h; yy++)
    for (int xx = x; xx < x + w; xx++)
      fb_pixel(fb, fw, fh, xx, yy, rgb);
}

static void fb_rect_alpha(volatile uint8_t *fb, int fw, int fh, int x, int y,
                          int w, int h, uint32_t rgb, int alpha) {
  for (int yy = y; yy < y + h; yy++)
    for (int xx = x; xx < x + w; xx++)
      fb_pixel_alpha(fb, fw, fh, xx, yy, rgb, alpha);
}

/* Built-in 8x8 fallback renderer */
static void fb_char8(volatile uint8_t *fb, int fw, int fh, int x, int y,
                     char c, uint32_t rgb) {
  if (c < 0x20 || c > 0x7E)
    return;
  const uint8_t *glyph = font_data[(uint8_t)c - 0x20u];
  for (int row = 0; row < FONT_HEIGHT; row++)
    for (int col = 0; col < FONT_WIDTH; col++)
      if (glyph[row] & (uint8_t)(0x80u >> col))
        fb_pixel(fb, fw, fh, x + col, y + row, rgb);
}

/* Anti-aliased Figtree font renderer reading directly from ASSETS_ARENA_ADDR */
static const AssetFontHead *ft_head(int kind) {
  volatile AuroraSystemMenuShared *s = aurora_system_menu_shared();
  uint32_t addr = (kind == 2) ? s->font_bold_addr
                : (kind == 1) ? s->font_reg_addr
                              : s->font_small_addr;
  if (addr < ASSETS_ARENA_ADDR || addr >= ASSETS_ARENA_ADDR + ASSETS_ARENA_MAX)
    return 0;
  return (const AssetFontHead *)addr;
}

static int ft_width(int kind, const char *s) {
  if (!s) return 0;
  const AssetFontHead *h = ft_head(kind);
  if (!h) {
    int n = 0;
    while (s[n]) n++;
    return n * FONT_WIDTH;
  }
  const AssetGlyph *glyphs = (const AssetGlyph *)((const uint8_t *)h + sizeof(AssetFontHead));
  int w = 0;
  while (*s) {
    uint8_t c = (uint8_t)*s++;
    if (c >= h->first && c < h->first + h->count)
      w += glyphs[c - h->first].adv;
  }
  return w;
}

static int ft_height(int kind) {
  const AssetFontHead *h = ft_head(kind);
  return h ? (int)h->line_height : FONT_HEIGHT;
}

static void ft_draw(volatile uint8_t *fb, int fw, int fh, int x, int y,
                    int max_w, int kind, const char *s, uint32_t rgb) {
  if (!s) return;
  const AssetFontHead *h = ft_head(kind);
  if (!h) {
    int px = x;
    while (*s && (max_w <= 0 || px + FONT_WIDTH <= x + max_w)) {
      fb_char8(fb, fw, fh, px, y, *s++, rgb);
      px += FONT_WIDTH;
    }
    return;
  }
  const AssetGlyph *glyphs = (const AssetGlyph *)((const uint8_t *)h + sizeof(AssetFontHead));
  const uint8_t *atlas = (const uint8_t *)h + sizeof(AssetFontHead) +
                         (uint32_t)h->count * sizeof(AssetGlyph);
  int pen = x;
  int base = y + h->ascent;
  while (*s) {
    uint8_t c = (uint8_t)*s++;
    if (c < h->first || c >= h->first + h->count)
      continue;
    const AssetGlyph *g = &glyphs[c - h->first];
    if (max_w > 0 && pen + g->adv > x + max_w)
      break;
    int gx = pen + g->left;
    int gy = base - g->top;
    for (int r = 0; r < g->h; r++) {
      const uint8_t *row = atlas + (uint32_t)(g->ay + r) * h->atlas_w + g->ax;
      for (int col = 0; col < g->w; col++) {
        int a = row[col];
        if (a)
          fb_pixel_alpha(fb, fw, fh, gx + col, gy + r, rgb, a + (a >> 7));
      }
    }
    pen += g->adv;
  }
}

static void fb_grad_round_rect(volatile uint8_t *fb, int fw, int fh, int x, int y,
                               int w, int h, int r, uint32_t top_c,
                               uint32_t bot_c) {
  for (int yy = 0; yy < h; yy++) {
    int dy = yy < r ? (r - 1 - yy) : (yy >= h - r ? (yy - (h - r)) : 0);
    int inset = 0;
    if (dy > 0) {
      while (inset < r && (r - 1 - inset) * (r - 1 - inset) + dy * dy >= r * r)
        inset++;
    }
    uint32_t c = rgb_lerp(top_c, bot_c, yy, h > 1 ? h - 1 : 1);
    for (int xx = inset; xx < w - inset; xx++)
      fb_pixel(fb, fw, fh, x + xx, y + yy, c);
  }
}

/* Home-Menu-styled gradient sidebar panel over smoothly dimmed live screen */
static void backdrop_and_sidebar_base(void) {
  volatile AuroraSystemMenuShared *s = aurora_system_menu_shared();
  volatile uint8_t *dst = g_menu_fb;
  volatile uint8_t *src = g_prev_fb;
  int sx = g_slide_x;
  int right_edge = sx + SIDEBAR_W;
  int vis = right_edge > 0 ? (right_edge * 256) / SIDEBAR_W : 0;
  if (vis > 256) vis = 256;
  uint32_t dim = (uint32_t)(256 - (124 * vis) / 256);

  uint32_t pt = get_theme_rgb(s->panel_top_rgb, 0x262830u);
  uint32_t pb = get_theme_rgb(s->panel_bot_rgb, 0x181A20u);
  /* Slightly deeper panel tone matching Home Menu cards */
  pt = rgb_darken(pt, 7, 10);
  pb = rgb_darken(pb, 6, 10);

  if (!dst)
    return;

  if (src && src != dst) {
    for (int x = 0; x < BOT_WIDTH; x++) {
      uint32_t col_off = (uint32_t)x * BOT_HEIGHT * 3u;
      for (int i = 0; i < BOT_HEIGHT * 3; i++)
        dst[col_off + i] = (uint8_t)((src[col_off + i] * dim) >> 8);
    }
  } else {
    fb_rect(dst, BOT_WIDTH, BOT_HEIGHT, 0, 0, BOT_WIDTH, BOT_HEIGHT, 0x0C0D12u);
  }

  if (right_edge > 0) {
    fb_grad_round_rect(dst, BOT_WIDTH, BOT_HEIGHT, sx + 6, 8,
                       SIDEBAR_W - 10, BOT_HEIGHT - 16, 10, pt, pb);
  }
}

/* ---- AmenuApi implementation (ARM11) ---- */

static void api_sidebar_clear(void) {
  backdrop_and_sidebar_base();
}

static void api_sidebar_header(const char *title, const char *subtitle) {
  int sx = g_slide_x;
  int ty = 10 + (26 - ft_height(2)) / 2;
  ft_draw(g_menu_fb, BOT_WIDTH, BOT_HEIGHT, sx + 16, ty, SIDEBAR_W - 76, 2,
          title, COL_WHITE);
  if (subtitle && subtitle[0]) {
    uint32_t sc = COL_DIM;
    if (subtitle[0] == 'M' && subtitle[1] == 'O' && subtitle[2] == 'D')
      sc = (subtitle[5] == 'O' && subtitle[6] == 'N') ? COL_GREEN : COL_RED;
    int sw = ft_width(0, subtitle);
    int sy = 10 + (26 - ft_height(0)) / 2 + 1;
    ft_draw(g_menu_fb, BOT_WIDTH, BOT_HEIGHT, sx + SIDEBAR_W - 14 - sw, sy,
            sw + 2, 0, subtitle, sc);
  }
  fb_rect(g_menu_fb, BOT_WIDTH, BOT_HEIGHT, sx + 14, HEADER_H - 1,
          SIDEBAR_W - 26, 1, COL_HAIRLINE);
}

static void api_sidebar_item(int row, const char *label, const char *value,
                             int selected) {
  if (row < 0 || row >= VISIBLE_ROWS)
    return;
  volatile AuroraSystemMenuShared *s = aurora_system_menu_shared();
  int sx = g_slide_x;
  int y = LIST_Y + row * ROW_H;
  uint32_t accent = get_accent();
  uint32_t st = get_theme_rgb(s->slot_top_rgb, 0x36363Cu);
  uint32_t sb = get_theme_rgb(s->slot_bot_rgb, 0x1E1E22u);
  uint32_t fg = get_theme_rgb(s->text_rgb, COL_WHITE);
  uint32_t dm = get_theme_rgb(s->text_dim_rgb, COL_DIM);

  if (selected) {
    fb_grad_round_rect(g_menu_fb, BOT_WIDTH, BOT_HEIGHT, sx + 12, y,
                       SIDEBAR_W - 22, ROW_H - 3, 7, accent,
                       rgb_darken(accent, 7, 10));
  } else {
    fb_grad_round_rect(g_menu_fb, BOT_WIDTH, BOT_HEIGHT, sx + 12, y,
                       SIDEBAR_W - 22, ROW_H - 3, 7, st, sb);
  }

  int vw = (value && value[0]) ? ft_width(0, value) : 0;
  int max_lbl = SIDEBAR_W - 36 - (vw ? vw + 8 : 0);
  if (max_lbl < 32) max_lbl = 32;
  int ly = y + (ROW_H - 3 - ft_height(1)) / 2;
  ft_draw(g_menu_fb, BOT_WIDTH, BOT_HEIGHT, sx + 18, ly, max_lbl, 1,
          label, selected ? 0xFFFFFFu : fg);

  if (vw > 0) {
    int vx = sx + SIDEBAR_W - 16 - vw;
    int vy = y + (ROW_H - 3 - ft_height(0)) / 2;
    uint32_t vc = selected ? 0xFFFFFFu : dm;
    if (!selected && value[0] == 'O' && value[1] == 'N' && value[2] == '\0')
      vc = COL_GREEN;
    else if (!selected && ((value[0] == 'O' && value[1] == 'F' && value[2] == 'F') ||
                           (value[0] == 'L' && value[1] == 'O' && value[2] == 'C')))
      vc = COL_RED;
    ft_draw(g_menu_fb, BOT_WIDTH, BOT_HEIGHT, vx, vy, vw + 2, 0, value, vc);
  }
}

static void api_sidebar_separator(int row, const char *label) {
  if (row < 0 || row >= VISIBLE_ROWS)
    return;
  int sx = g_slide_x;
  int y = LIST_Y + row * ROW_H;
  fb_rect(g_menu_fb, BOT_WIDTH, BOT_HEIGHT, sx + 12, y + ROW_H / 2,
          SIDEBAR_W - 24, 1, COL_HAIRLINE);
  if (label && label[0])
    ft_draw(g_menu_fb, BOT_WIDTH, BOT_HEIGHT, sx + 14, y + 4, SIDEBAR_W - 28,
            0, label, COL_DIM);
}

static void api_sidebar_footer(const char *hint) {
  int sx = g_slide_x;
  fb_rect(g_menu_fb, BOT_WIDTH, BOT_HEIGHT, sx + 14, FOOTER_Y,
          SIDEBAR_W - 26, 1, COL_HAIRLINE);
  int fy = FOOTER_Y + (BOT_HEIGHT - 8 - FOOTER_Y - ft_height(0)) / 2;
  ft_draw(g_menu_fb, BOT_WIDTH, BOT_HEIGHT, sx + 14, fy, SIDEBAR_W - 26,
          0, hint, COL_DIM);
}

static void api_draw_pixel(int x, int y, uint32_t rgb) {
  fb_pixel(g_menu_fb, BOT_WIDTH, BOT_HEIGHT, x + g_slide_x, y, rgb);
}

static void api_draw_rect(int x, int y, int w, int h, uint32_t rgb) {
  fb_rect(g_menu_fb, BOT_WIDTH, BOT_HEIGHT, x + g_slide_x, y, w, h, rgb);
}

static void api_draw_rect_alpha(int x, int y, int w, int h, uint32_t rgb,
                                int alpha) {
  fb_rect_alpha(g_menu_fb, BOT_WIDTH, BOT_HEIGHT, x + g_slide_x, y, w, h, rgb, alpha);
}

static void api_draw_text(int x, int y, const char *text, uint32_t rgb) {
  ft_draw(g_menu_fb, BOT_WIDTH, BOT_HEIGHT, x + g_slide_x, y, 0, 1, text, rgb);
}

static void api_hud_rect(int screen, int x, int y, int w, int h, uint32_t rgb,
                         int alpha) {
  if (g_hud_target_screen >= 0) {
    if (screen != g_hud_target_screen || !g_hud_target_fb)
      return;
    int fw = screen == 0 ? TOP_WIDTH : BOT_WIDTH;
    int fh = screen == 0 ? TOP_HEIGHT : BOT_HEIGHT;
    fb_rect_alpha(g_hud_target_fb, fw, fh, x, y, w, h, rgb, alpha);
    return;
  }
  if (screen == 0) {
    volatile uint8_t *fb = top_framebuffer(gpu11_front_frame(0));
    fb_rect_alpha(fb, TOP_WIDTH, TOP_HEIGHT, x, y, w, h, rgb, alpha);
  } else {
    volatile uint8_t *fb = bot_framebuffer(gpu11_front_frame(1));
    fb_rect_alpha(fb, BOT_WIDTH, BOT_HEIGHT, x, y, w, h, rgb, alpha);
  }
}

static void api_hud_text(int screen, int x, int y, const char *text,
                         uint32_t rgb) {
  if (g_hud_target_screen >= 0) {
    if (screen != g_hud_target_screen || !g_hud_target_fb)
      return;
    int fw = screen == 0 ? TOP_WIDTH : BOT_WIDTH;
    int fh = screen == 0 ? TOP_HEIGHT : BOT_HEIGHT;
    ft_draw(g_hud_target_fb, fw, fh, x, y, 0, 0, text, rgb);
    return;
  }
  if (screen == 0) {
    volatile uint8_t *fb = top_framebuffer(gpu11_front_frame(0));
    ft_draw(fb, TOP_WIDTH, TOP_HEIGHT, x, y, 0, 0, text, rgb);
  } else {
    volatile uint8_t *fb = bot_framebuffer(gpu11_front_frame(1));
    ft_draw(fb, BOT_WIDTH, BOT_HEIGHT, x, y, 0, 0, text, rgb);
  }
}

static uint32_t api_accent_color(void) { return get_accent(); }
static uint32_t api_now_us(void) { return core11_now_us(); }
static uint32_t api_keys_held(void) { return menu_keys(); }

static int api_get_brightness(void) {
  volatile AuroraSystemMenuShared *s = aurora_system_menu_shared();
  uint32_t b = s->brightness;
  return b <= 4u ? (int)b : 3;
}

static void api_set_brightness(int level) {
  volatile AuroraSystemMenuShared *s = aurora_system_menu_shared();
  AudioCtrl *ct = (AudioCtrl *)AUDIO_CTRL_ADDR;
  if (level < 0) level = 0;
  if (level > 4) level = 4;
  s->brightness = (uint32_t)level;
  dcache_clean_line((const void *)&s->brightness);
  backlight11_set(ct, (uint32_t)level);
}

static void api_request_home(void) {
  volatile AuroraSystemMenuShared *s = aurora_system_menu_shared();
  if (s->app_active && (s->app_caps & AURORA_SYSTEM_MENU_CAP_HOME)) {
    s->action = AURORA_SYSTEM_MENU_HOME;
    dcache_clean_line((const void *)&s->action);
    g_request_close = 1;
  }
}

static int api_app_active(void) {
  volatile AuroraSystemMenuShared *s = aurora_system_menu_shared();
  return s->app_active != 0u;
}

static int api_app_modding_allowed(void) { return 1; }

static const char *api_app_path(void) {
  volatile AuroraSystemMenuShared *s = aurora_system_menu_shared();
  return s->app_active ? (const char *)s->app_path : "/AURORAOS.BIN";
}

static int app_addr_valid(uint32_t addr, uint32_t len) {
  if (len == 0u || addr + len < addr)
    return 0;
  if (addr >= 0x22000000u && addr + len <= 0x23000000u)
    return 1;
  if (addr >= 0x24000000u && addr + len <= 0x25000000u)
    return 1;
  return 0;
}

static int api_app_mem_read(uint32_t addr, void *dst, uint32_t len) {
  if (!api_app_modding_allowed() || !dst || !app_addr_valid(addr, len))
    return 0;
  const volatile uint8_t *src = (const volatile uint8_t *)addr;
  uint8_t *out = (uint8_t *)dst;
  for (uint32_t i = 0; i < len; i++)
    out[i] = src[i];
  return 1;
}

static int api_app_mem_write(uint32_t addr, const void *src, uint32_t len) {
  if (!api_app_modding_allowed() || !src || !app_addr_valid(addr, len))
    return 0;
  volatile uint8_t *dst = (volatile uint8_t *)addr;
  const uint8_t *in = (const uint8_t *)src;
  for (uint32_t i = 0; i < len; i++)
    dst[i] = in[i];
  dsb();
  volatile AuroraSystemMenuShared *s = aurora_system_menu_shared();
  s->mod_flush_seq = s->mod_flush_seq + 1u;
  dcache_clean_line((const void *)&s->mod_flush_seq);
  return 1;
}

static uint32_t api_app_mem_find(uint32_t start, uint32_t len,
                                 const void *pattern, uint32_t pat_len) {
  if (!api_app_modding_allowed() || !pattern || pat_len == 0u ||
      pat_len > len || !app_addr_valid(start, len))
    return 0u;
  const volatile uint8_t *mem = (const volatile uint8_t *)start;
  const uint8_t *pat = (const uint8_t *)pattern;
  uint32_t limit = len - pat_len;
  for (uint32_t i = 0; i <= limit; i++) {
    uint32_t j = 0;
    while (j < pat_len && mem[i + j] == pat[j])
      j++;
    if (j == pat_len)
      return start + i;
  }
  return 0u;
}

static int api_app_set_mod_keys(uint32_t key_mask) {
  if (!api_app_modding_allowed())
    return 0;
  volatile AuroraSystemMenuShared *s = aurora_system_menu_shared();
  s->mod_keys = key_mask & 0xFFFu;
  dcache_clean_line((const void *)&s->mod_keys);
  return 1;
}

static void api_set_accent(uint32_t rgb) {
  volatile AuroraSystemMenuShared *s = aurora_system_menu_shared();
  s->accent_rgb = rgb & 0xFFFFFFu;
  dcache_clean_line((const void *)&s->accent_rgb);
}

static void api_set_led_color(uint8_t r, uint8_t g, uint8_t b) {
  power_set_led(r, g, b);
}

static void api_play_tone(uint32_t freq_hz) {
  AudioCtrl *ct = (AudioCtrl *)AUDIO_CTRL_ADDR;
  if (freq_hz)
    audio11_command(ct, AUDIO_CMD_TONE, freq_hz);
  else
    audio11_command(ct, AUDIO_CMD_STOP, 0);
}

static int api_get_battery(int *charging, int *mv, int *temp_c) {
  if (charging) *charging = battery_charging();
  if (mv)       *mv = battery_millivolts();
  if (temp_c)   *temp_c = battery_temperature();
  return battery_percent();
}

static void api_get_sys_info(uint32_t *cpu_mhz, uint32_t *is_n3ds,
                             uint32_t *core_ver, uint32_t *gpu_ops) {
  const AudioCtrl *ct = (const AudioCtrl *)AUDIO_CTRL_ADDR;
  const GpuShared *g  = (const GpuShared *)GPU_SHARED_ADDR;
  uint32_t div = core11_clock_div();
  if (cpu_mhz)  *cpu_mhz  = div * 268u;
  if (is_n3ds)  *is_n3ds  = (ct->socinfo & (1u << 2)) ? 1u : 0u;
  if (core_ver) *core_ver = ct->version;
  if (gpu_ops)  *gpu_ops  = g->ops;
}

static void api_reboot(void) { power_reboot(); }

static const AmenuApi g_api11 = {
  AMENU_ABI_VERSION,
  SIDEBAR_W,
  BOT_WIDTH,
  BOT_HEIGHT,
  api_sidebar_clear,
  api_sidebar_header,
  api_sidebar_item,
  api_sidebar_separator,
  api_sidebar_footer,
  api_draw_pixel,
  api_draw_rect,
  api_draw_rect_alpha,
  api_draw_text,
  api_hud_rect,
  api_hud_text,
  api_accent_color,
  api_now_us,
  api_keys_held,
  api_get_brightness,
  api_set_brightness,
  api_request_home,
  api_app_active,
  api_app_modding_allowed,
  api_app_path,
  api_app_mem_read,
  api_app_mem_write,
  api_app_mem_find,
  api_app_set_mod_keys,
  api_set_accent,
  api_set_led_color,
  api_play_tone,
  api_get_battery,
  api_get_sys_info,
  api_reboot,
};

static const AmenuPlugin *plugin_get(AmenuSlot *slot) {
  if (!slot || !slot->loaded || !slot->entry_addr)
    return 0;
  if (!slot->initialized) {
    AmenuEntryFn entry = (AmenuEntryFn)slot->entry_addr;
    const AmenuPlugin *p = entry(&g_api11);
    slot->initialized = 1;
    if (p && p->abi_version == AMENU_ABI_VERSION) {
      slot->plugin_ptr = (uint32_t)p;
      if (p->on_load)
        p->on_load(&g_api11);
    } else {
      slot->plugin_ptr = 0;
    }
  }
  return (const AmenuPlugin *)slot->plugin_ptr;
}

static int app_gone(void) {
  volatile AuroraSystemMenuShared *shared = aurora_system_menu_shared();
  dcache_inval_line((const void *)&shared->app_active);
  return !shared->app_active;
}

enum {
  ITEM_PLUGIN = 0,
  ITEM_HOME,
  ITEM_BRIGHTNESS,
  ITEM_POWER,
};

typedef struct {
  int kind;
  int slot_idx;
} MenuItem;

static int build_root_items(MenuItem *items) {
  int n = 0;
  AmenuTable *tbl = amenu_table();
  if (tbl->magic == AMENU_TABLE_MAGIC && tbl->version == AMENU_ABI_VERSION) {
    uint32_t cnt = tbl->count > AMENU_MAX_PLUGINS ? AMENU_MAX_PLUGINS : tbl->count;
    for (uint32_t i = 0; i < cnt; i++) {
      if (tbl->slots[i].loaded && plugin_get(&tbl->slots[i])) {
        items[n].kind = ITEM_PLUGIN;
        items[n].slot_idx = (int)i;
        n++;
      }
    }
  }
  items[n].kind = ITEM_HOME;       items[n].slot_idx = -1; n++;
  items[n].kind = ITEM_BRIGHTNESS; items[n].slot_idx = -1; n++;
  items[n].kind = ITEM_POWER;      items[n].slot_idx = -1; n++;
  return n;
}

static int in_round_rect(int lx, int ly, int w, int h, int r) {
  int cx = (lx < r) ? r - 1 - lx : (lx >= w - r) ? lx - (w - r) : 0;
  int cy = (ly < r) ? r - 1 - ly : (ly >= h - r) ? ly - (h - r) : 0;
  return cx * cx + cy * cy <= r * r;
}

static void draw_dock_thumb(int x, int y, int w, int h, int r,
                            const volatile uint16_t *thumb, int sel,
                            uint32_t accent) {
  uint32_t ar = (accent >> 16) & 0xFFu, ag = (accent >> 8) & 0xFFu, ab = accent & 0xFFu;
  for (int ly = 0; ly < h; ly++) {
    int py = y + ly;
    if (py < 0 || py >= BOT_HEIGHT) continue;
    int ty = (ly * (int)AURORA_DOCK_THUMB_H) / h;
    for (int lx = 0; lx < w; lx++) {
      int px = x + lx;
      if (px < 0 || px >= BOT_WIDTH) continue;
      if (!in_round_rect(lx, ly, w, h, r)) continue;
      int border = sel && (lx < 2 || lx >= w - 2 || ly < 2 || ly >= h - 2 ||
                           !in_round_rect(lx, ly, w, h, r + 2));
      if (border) {
        fb_pixel(g_menu_fb, BOT_WIDTH, BOT_HEIGHT, px, py, accent);
        continue;
      }
      int tx = (lx * (int)AURORA_DOCK_THUMB_W) / w;
      uint16_t c565 = thumb[ty * (int)AURORA_DOCK_THUMB_W + tx];
      uint32_t cr = ((c565 >> 11) & 31u) * 255u / 31u;
      uint32_t cg = ((c565 >> 5) & 63u) * 255u / 63u;
      uint32_t cb = (c565 & 31u) * 255u / 31u;
      if (sel) {
        cr = (cr * 170u + ar * 85u) >> 8;
        cg = (cg * 170u + ag * 85u) >> 8;
        cb = (cb * 170u + ab * 85u) >> 8;
      } else {
        cr = (cr * 140u) >> 8;
        cg = (cg * 140u) >> 8;
        cb = (cb * 140u) >> 8;
      }
      fb_pixel(g_menu_fb, BOT_WIDTH, BOT_HEIGHT, px, py, (cr << 16) | (cg << 8) | cb);
    }
  }
}

static void draw_dock_icon(int x, int y, const volatile uint16_t *icon) {
  for (int ly = 0; ly < (int)AURORA_DOCK_ICON_H; ly++) {
    int py = y + ly;
    if (py < 0 || py >= BOT_HEIGHT) continue;
    for (int lx = 0; lx < (int)AURORA_DOCK_ICON_W; lx++) {
      int px = x + lx;
      if (px < 0 || px >= BOT_WIDTH) continue;
      if (!in_round_rect(lx, ly, (int)AURORA_DOCK_ICON_W, (int)AURORA_DOCK_ICON_H, 6))
        continue;
      uint16_t c565 = icon[ly * (int)AURORA_DOCK_ICON_W + lx];
      uint32_t cr = ((c565 >> 11) & 31u) * 255u / 31u;
      uint32_t cg = ((c565 >> 5) & 63u) * 255u / 63u;
      uint32_t cb = (c565 & 31u) * 255u / 31u;
      fb_pixel(g_menu_fb, BOT_WIDTH, BOT_HEIGHT, px, py, (cr << 16) | (cg << 8) | cb);
    }
  }
}

static void draw_dock(int dock_focus, int dock_sel) {
  volatile AuroraSystemMenuShared *s = aurora_system_menu_shared();
  int slide_r = (-g_slide_x * 124) / SIDEBAR_W;
  int dx = SIDEBAR_W + 4 + slide_r;
  int dy = 8, dw = BOT_WIDTH - SIDEBAR_W - 10, dh = BOT_HEIGHT - 16;
  if (dx >= BOT_WIDTH)
    return;

  uint32_t pt = rgb_darken(get_theme_rgb(s->panel_top_rgb, 0x262830u), 7, 10);
  uint32_t pb = rgb_darken(get_theme_rgb(s->panel_bot_rgb, 0x181A20u), 6, 10);
  uint32_t st = get_theme_rgb(s->slot_top_rgb, 0x303440u);
  uint32_t sb = get_theme_rgb(s->slot_bot_rgb, 0x242730u);
  uint32_t txt = get_theme_rgb(s->text_rgb, COL_WHITE);
  uint32_t dim = get_theme_rgb(s->text_dim_rgb, COL_MUTED);
  uint32_t accent = get_accent();

  fb_grad_round_rect(g_menu_fb, BOT_WIDTH, BOT_HEIGHT, dx, dy, dw, dh, 10, pt, pb);
  ft_draw(g_menu_fb, BOT_WIDTH, BOT_HEIGHT, dx + 10, dy + 8, dw - 20, 2, "Dock", accent);

  uint32_t dcnt = s->dock_count;
  if (dcnt > AURORA_DOCK_MAX) dcnt = AURORA_DOCK_MAX;
  for (uint32_t i = 0; i < AURORA_DOCK_MAX; i++) {
    int sy = dy + 32 + (int)i * 60;
    int sh = 52;
    int cx = dx + 6, cw = dw - 12;
    int sel = dock_focus && (int)i == dock_sel && i < dcnt;
    if (i < dcnt && s->dock[i].has_thumb) {
      draw_dock_thumb(cx, sy, cw, sh, 8, s->dock[i].thumb, sel, accent);
    } else if (sel) {
      fb_grad_round_rect(g_menu_fb, BOT_WIDTH, BOT_HEIGHT, cx, sy, cw,
                         sh, 8, accent, rgb_darken(accent, 7, 10));
    } else {
      fb_grad_round_rect(g_menu_fb, BOT_WIDTH, BOT_HEIGHT, cx, sy, cw,
                         sh, 8, st, sb);
    }
    if (i < dcnt && s->dock[i].name[0]) {
      if (s->dock[i].has_icon) {
        draw_dock_icon(cx + 6, sy + 10, s->dock[i].icon);
      } else {
        uint32_t tc = s->dock[i].tint_rgb ? s->dock[i].tint_rgb : accent;
        char init[2] = { s->dock[i].name[0], '\0' };
        fb_grad_round_rect(g_menu_fb, BOT_WIDTH, BOT_HEIGHT, cx + 6, sy + 10,
                           32, 32, 6, tc, rgb_darken(tc, 7, 10));
        ft_draw(g_menu_fb, BOT_WIDTH, BOT_HEIGHT, cx + 17, sy + 18, 16, 2,
                init, COL_WHITE);
      }
      ft_draw(g_menu_fb, BOT_WIDTH, BOT_HEIGHT, cx + 44, sy + 18, cw - 48, 1,
              (const char *)s->dock[i].name, (sel || s->dock[i].has_thumb) ? COL_WHITE : txt);
    } else {
      ft_draw(g_menu_fb, BOT_WIDTH, BOT_HEIGHT, cx + 10, sy + 18, cw - 20, 0,
              "Empty", dim);
    }
  }
}

static void draw_root(const MenuItem *items, int count, int selected,
                      int scroll, int dock_focus, int dock_sel) {
  int mod_ok = api_app_modding_allowed();
  AmenuTable *tbl = amenu_table();
  char br_buf[8];
  int br = api_get_brightness();
  if (!dock_focus && selected >= 0 && selected < count &&
      items[selected].kind == ITEM_BRIGHTNESS) {
    br_buf[0] = '<'; br_buf[1] = ' ';
    br_buf[2] = (char)('1' + br);
    br_buf[3] = '/'; br_buf[4] = '5';
    br_buf[5] = ' '; br_buf[6] = '>'; br_buf[7] = '\0';
  } else {
    br_buf[0] = (char)('1' + br);
    br_buf[1] = '/'; br_buf[2] = '5'; br_buf[3] = '\0';
  }

  api_sidebar_clear();
  api_sidebar_header("Aurora", mod_ok ? "MOD: ON" : "MOD: OFF");

  for (int r = 0; r < VISIBLE_ROWS && scroll + r < count; r++) {
    int idx = scroll + r;
    int sel = (!dock_focus) && (idx == selected);
    const MenuItem *it = &items[idx];
    if (it->kind == ITEM_PLUGIN) {
      AmenuSlot *slot = &tbl->slots[it->slot_idx];
      const AmenuPlugin *p = (const AmenuPlugin *)slot->plugin_ptr;
      const char *name = (p && p->name[0]) ? p->name : slot->filename;
      const char *val = ">";
      if (p && !(p->flags & AMENU_FLAG_HAS_MENU) && (p->flags & AMENU_FLAG_HAS_HUD))
        val = slot->hud_enabled ? "ON" : "OFF";
      else if (p && (p->flags & AMENU_FLAG_HAS_HUD) && slot->hud_enabled)
        val = "HUD";
      api_sidebar_item(r, name, val, sel);
    } else if (it->kind == ITEM_HOME) {
      api_sidebar_item(r, "Home", "", sel);
    } else if (it->kind == ITEM_BRIGHTNESS) {
      api_sidebar_item(r, "Brightness", br_buf, sel);
    } else if (it->kind == ITEM_POWER) {
      api_sidebar_item(r, "Power Off", "", sel);
    }
  }

  if (dock_focus)
    api_sidebar_footer("A: Switch  <: Menu  B: Close");
  else if (selected >= 0 && selected < count && items[selected].kind == ITEM_BRIGHTNESS)
    api_sidebar_footer("</>: Adjust  B: Close");
  else if (selected >= 0 && selected < count && items[selected].kind == ITEM_PLUGIN)
    api_sidebar_footer("A: Open  X: HUD  >: Dock");
  else
    api_sidebar_footer("A: Select  >: Dock  B: Close");

  draw_dock(dock_focus, dock_sel);
}

static void menu_notice(void) {
  static const char *const lines[] = {
      "Built before Beta 7.",
      "Press HOME after closing,",
      "or rebuild with the SDK."};
  uint32_t previous;

  menu_begin_frame();
  api_sidebar_clear();
  api_sidebar_header("Home", "Older App");
  for (unsigned i = 0; i < sizeof(lines) / sizeof(lines[0]); i++)
    ft_draw(g_menu_fb, BOT_WIDTH, BOT_HEIGHT, 12, 52 + (int)i * 18,
            SIDEBAR_W - 24, 1, lines[i], COL_MUTED);
  api_sidebar_footer("A or B: Back");
  menu_swap_frame();
  while ((menu_keys() & (KEY_A | KEY_B)) && !app_gone())
    sleep_ms(16);
  previous = menu_keys();
  while (!app_gone()) {
    uint32_t current = menu_keys();
    uint32_t down = current & ~previous;
    previous = current;
    if (down & (KEY_A | KEY_B))
      return;
    stereo11_tick();
    sleep_ms(16);
  }
}

static int menu_confirm_power(void) {
  uint32_t previous;
  int selected = 1;
  while (menu_keys() & (KEY_A | KEY_B))
    sleep_ms(16);

  for (;;) {
    menu_begin_frame();
    api_sidebar_clear();
    api_sidebar_header("Power Off", "Confirm");
    api_sidebar_item(0, "Power Off", "", selected == 0);
    api_sidebar_item(1, "Cancel", "", selected == 1);
    api_sidebar_footer("A: Confirm  B: Back");
    menu_swap_frame();

    previous = menu_keys();
    for (;;) {
      uint32_t current = menu_keys();
      uint32_t down = current & ~previous;
      if (app_gone())
        return 0;
      previous = current;
      if (down & (KEY_UP | KEY_DOWN)) {
        selected ^= 1;
        break;
      }
      if (down & KEY_B)
        return 0;
      if (down & KEY_A)
        return selected == 0;
      stereo11_tick();
      sleep_ms(16);
    }
  }
}

static void run_plugin_menu(AmenuSlot *slot) {
  const AmenuPlugin *p = plugin_get(slot);
  if (!p || !p->on_menu_frame)
    return;
  if (p->on_menu_open)
    p->on_menu_open(&g_api11);
  while ((menu_keys() & KEY_A) && !app_gone())
    sleep_ms(16);
  uint32_t previous = menu_keys();
  uint32_t last_draw = 0;
  int first = 1;
  while (!app_gone() && !g_request_close) {
    uint32_t current = menu_keys();
    uint32_t down = current & ~previous;
    uint32_t now = core11_now_us();
    previous = current;
    if (first || down || (now - last_draw >= 250000u)) {
      first = 0;
      last_draw = now;
      menu_begin_frame();
      int keep = p->on_menu_frame(&g_api11, current, down);
      menu_swap_frame();
      if (!keep)
        break;
    }
    stereo11_tick();
    sleep_ms(16);
  }
  while ((menu_keys() & (KEY_A | KEY_B)) && !app_gone())
    sleep_ms(16);
}

static void menu_run(void) {
  volatile AuroraSystemMenuShared *shared = aurora_system_menu_shared();
  int frame = gpu11_menu_frame();
  if (frame < 0)
    return;

  int prev = gpu11_menu_prev_frame();
  g_buf[0] = (prev + 1) % 3;
  g_buf[1] = (prev + 2) % 3;
  g_cur_buf = 0;
  g_prev_fb = bot_framebuffer(prev);
  g_request_close = 0;

  shared->menu_open = 1;
  dcache_clean_line((const void *)&shared->menu_open);

  MenuItem items[AMENU_MAX_PLUGINS + 4];
  int count = build_root_items(items);
  int selected = 0;
  int scroll = 0;
  int dock_focus = 0;
  int dock_sel = 0;

  /* Smooth cubic ease-out slide-in */
  for (int step = 1; step <= 8; step++) {
    int inv = 8 - step;
    g_slide_x = -(SIDEBAR_W * inv * inv * inv) / 512;
    menu_begin_frame();
    draw_root(items, count, selected, scroll, dock_focus, dock_sel);
    menu_swap_frame();
    sleep_ms(12);
  }
  g_slide_x = 0;

  while ((menu_keys() & KEY_CHORD) && !app_gone())
    sleep_ms(16);
  uint32_t previous = menu_keys();

  while (!app_gone() && !g_request_close) {
    uint32_t current = menu_keys();
    uint32_t down = current & ~previous;
    int redraw = 0;
    uint32_t dcnt = shared->dock_count;
    if (dcnt > AURORA_DOCK_MAX) dcnt = AURORA_DOCK_MAX;
    previous = current;

    if (down & KEY_B)
      break;

    if (dock_focus) {
      if (down & KEY_LEFT) {
        dock_focus = 0;
        redraw = 1;
      } else if (down & KEY_UP) {
        if (dcnt > 0)
          dock_sel = dock_sel > 0 ? dock_sel - 1 : (int)dcnt - 1;
        redraw = 1;
      } else if (down & KEY_DOWN) {
        if (dcnt > 0)
          dock_sel = dock_sel + 1 < (int)dcnt ? dock_sel + 1 : 0;
        redraw = 1;
      } else if ((down & KEY_A) && (uint32_t)dock_sel < dcnt) {
        if (shared->app_caps & AURORA_SYSTEM_MENU_CAP_HOME) {
          shared->dock_launch_idx = (uint32_t)dock_sel;
          shared->action = AURORA_SYSTEM_MENU_HOME;
          dcache_clean_line((const void *)&shared->action);
          break;
        }
        menu_notice();
        previous = menu_keys();
        redraw = 1;
      }
    } else {
      if (down & KEY_UP) {
        selected = selected > 0 ? selected - 1 : count - 1;
        redraw = 1;
      }
      if (down & KEY_DOWN) {
        selected = selected + 1 < count ? selected + 1 : 0;
        redraw = 1;
      }
      if (selected < scroll)
        scroll = selected;
      if (selected >= scroll + VISIBLE_ROWS)
        scroll = selected - VISIBLE_ROWS + 1;

      if (items[selected].kind == ITEM_BRIGHTNESS) {
        if (down & KEY_LEFT) {
          api_set_brightness(api_get_brightness() - 1);
          redraw = 1;
        } else if (down & KEY_RIGHT) {
          api_set_brightness(api_get_brightness() + 1);
          redraw = 1;
        }
      } else if ((down & KEY_RIGHT) && dcnt > 0) {
        dock_focus = 1;
        if ((uint32_t)dock_sel >= dcnt) dock_sel = 0;
        redraw = 1;
      }

      if ((down & KEY_X) && items[selected].kind == ITEM_PLUGIN) {
        AmenuTable *tbl = amenu_table();
        int sidx = items[selected].slot_idx;
        AmenuSlot *slot = &tbl->slots[sidx];
        const AmenuPlugin *p = (const AmenuPlugin *)slot->plugin_ptr;
        if (p && (p->flags & AMENU_FLAG_HAS_HUD)) {
          slot->hud_enabled = !slot->hud_enabled;
          if (slot->hud_enabled) tbl->hud_mask |= (1u << sidx);
          else                   tbl->hud_mask &= ~(1u << sidx);
          if (p->on_toggle)
            p->on_toggle(&g_api11, (int)slot->hud_enabled);
          redraw = 1;
        }
      }

      if (down & KEY_A) {
        int k = items[selected].kind;
        if (k == ITEM_PLUGIN) {
          AmenuTable *tbl = amenu_table();
          int sidx = items[selected].slot_idx;
          AmenuSlot *slot = &tbl->slots[sidx];
          const AmenuPlugin *p = (const AmenuPlugin *)slot->plugin_ptr;
          if (p && p->on_menu_frame) {
            run_plugin_menu(slot);
            if (p->flags & AMENU_FLAG_HAS_HUD) {
              slot->hud_enabled = 1;
              tbl->hud_mask |= (1u << sidx);
            }
            previous = menu_keys();
            redraw = 1;
          } else if (p && (p->flags & AMENU_FLAG_HAS_HUD)) {
            slot->hud_enabled = !slot->hud_enabled;
            if (slot->hud_enabled) tbl->hud_mask |= (1u << sidx);
            else                   tbl->hud_mask &= ~(1u << sidx);
            if (p->on_toggle)
              p->on_toggle(&g_api11, (int)slot->hud_enabled);
            redraw = 1;
          }
        } else if (k == ITEM_HOME) {
          if (shared->app_caps & AURORA_SYSTEM_MENU_CAP_HOME) {
            shared->dock_launch_idx = AURORA_DOCK_LAUNCH_NONE;
            shared->action = AURORA_SYSTEM_MENU_HOME;
            dcache_clean_line((const void *)&shared->action);
            break;
          }
          menu_notice();
          previous = menu_keys();
          redraw = 1;
        } else if (k == ITEM_POWER) {
          if (menu_confirm_power())
            power_shutdown();
          previous = menu_keys();
          redraw = 1;
        }
      }
    }

    if (redraw && !g_request_close) {
      menu_begin_frame();
      draw_root(items, count, selected, scroll, dock_focus, dock_sel);
      menu_swap_frame();
    }
    stereo11_tick();
    sleep_ms(16);
  }

  /* Smooth cubic ease-in slide-out unless leaving to Home or app exited */
  if (!app_gone() && shared->action == AURORA_SYSTEM_MENU_NONE) {
    for (int step = 1; step <= 8 && !app_gone(); step++) {
      g_slide_x = -(SIDEBAR_W * step * step * step) / 512;
      menu_begin_frame();
      draw_root(items, count, selected, scroll, dock_focus, dock_sel);
      menu_swap_frame();
      sleep_ms(12);
    }
  }
  g_slide_x = 0;

  while ((menu_keys() & 0xFFFu) && !app_gone())
    sleep_ms(16);
  shared->menu_open = 0;
  dcache_clean_line((const void *)&shared->menu_open);
  gpu11_menu_restore();
  g_menu_fb = 0;
  g_prev_fb = 0;
}

/* Called by Gpu11 right after ppf_copy() and right before show() so HUDs are
 * composited onto the hidden off-screen buffer with zero flicker. */
void system_menu11_draw_hud(int panel, uint32_t fb_addr) {
  volatile AuroraSystemMenuShared *shared = aurora_system_menu_shared();
  if (!shared->os_ready || shared->menu_open)
    return;
  AmenuTable *tbl = amenu_table();
  if (tbl->magic != AMENU_TABLE_MAGIC || tbl->version != AMENU_ABI_VERSION)
    return;
  uint32_t mask = tbl->hud_mask;
  if (!mask)
    return;

  hud_present_us = core11_now_us();
  uint32_t cnt = tbl->count;
  if (cnt > AMENU_MAX_PLUGINS)
    cnt = AMENU_MAX_PLUGINS;

  int active = api_app_active();
  int mod_ok = api_app_modding_allowed();
  g_hud_target_screen = panel;
  g_hud_target_fb = (volatile uint8_t *)fb_addr;
  for (uint32_t i = 0; i < cnt; i++) {
    if (!(mask & (1u << i)))
      continue;
    AmenuSlot *slot = &tbl->slots[i];
    if (!slot->loaded || !slot->hud_enabled)
      continue;
    const AmenuPlugin *p = plugin_get(slot);
    if (p && p->on_hud_tick)
      p->on_hud_tick(&g_api11, active, mod_ok);
  }
  g_hud_target_screen = -1;
  g_hud_target_fb = 0;
}

static void hud_plugins_tick(void) {
  AmenuTable *tbl = amenu_table();
  if (tbl->magic != AMENU_TABLE_MAGIC || tbl->version != AMENU_ABI_VERSION)
    return;
  uint32_t mask = tbl->hud_mask;
  if (!mask)
    return;

  uint32_t now = core11_now_us();
  /* When Gpu11 is actively presenting frames, system_menu11_draw_hud() already
   * composites the HUD off-screen on every frame; only tick here when idle. */
  if (now - hud_present_us < 120000u)
    return;
  if (now - hud_last_us < 100000u)
    return;
  hud_last_us = now;

  uint32_t cnt = tbl->count;
  if (cnt > AMENU_MAX_PLUGINS)
    cnt = AMENU_MAX_PLUGINS;

  int active = api_app_active();
  int mod_ok = api_app_modding_allowed();
  for (uint32_t i = 0; i < cnt; i++) {
    if (!(mask & (1u << i)))
      continue;
    AmenuSlot *slot = &tbl->slots[i];
    if (!slot->loaded || !slot->hud_enabled)
      continue;
    const AmenuPlugin *p = plugin_get(slot);
    if (p && p->on_hud_tick)
      p->on_hud_tick(&g_api11, active, mod_ok);
  }
}

void system_menu11_poll(void) {
  volatile AuroraSystemMenuShared *shared = aurora_system_menu_shared();
  uint32_t held = menu_keys() & KEY_CHORD;
  dcache_inval_line((const void *)&shared->app_active);
  /* Only run HUD ticks once the OS has finished booting and a HUD is active. */
  if (shared->os_ready && !shared->menu_open)
    hud_plugins_tick();
  if (!shared->app_active) {
    menu_chord_latched = held == KEY_CHORD;
    return;
  }
  if (held == KEY_CHORD && !menu_chord_latched) {
    menu_chord_latched = 1;
    menu_run();
  } else if (held != KEY_CHORD) {
    menu_chord_latched = 0;
  }
}
