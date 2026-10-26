/* Aurora launcher: the menu you get with START held at power-on (Boot,
 * Power Off, Settings). It is drawn with the Home Menu's look: the same
 * wallpaper, gradient tiles, accent selection ring, bar and preview card.
 *
 * The launcher is built into the FIRM and cannot link src/ui.c (that file
 * needs the GPU, timer and animation code of the OS), so it draws with the
 * primitives of src/screen.c and loads SD:/Aurora/assets.pak itself. Without
 * the pack it falls back to the built-in 1bpp icons and the 8x8 font.
 *
 * Speed: the wallpaper, bar and logo are drawn once into snapshots in FCRAM.
 * A frame then restores only the rectangle that changes from a snapshot and
 * copies only that rectangle to the panel. */

#include "aurora.h"
#include "aurora_logo.h"
#include "assets.h"
#include "font.h"
#include "ff.h"
#include "i2c.h"
#include "icons.h"
#include "loader.h"
#include "sdmmc.h"

#define BSH BOT_SCREEN_HEIGHT
#define TSH TOP_SCREEN_HEIGHT

void delay(volatile u32 cycles) {
  while (cycles--) {
    __asm__ volatile("nop");
  }
}

static inline void arm9_drain_write_buffer(void) {
  __asm__ volatile("mcr p15, 0, %0, c7, c10, 4" ::"r"(0) : "memory");
}

void power_off(void) {
  I2C_init();

  /* Power the LCDs off before cutting system power. */
  I2C_writeReg(I2C_DEV_MCU, 0x22, 1 << 0);
  arm9_drain_write_buffer(); /* Match the ARM DSB sequence used by GodMode9. */

  /* Cut power to the console; bit 2 would trigger a reboot. */
  I2C_writeReg(I2C_DEV_MCU, 0x20, 1 << 0);

  while (1) {
    __asm__ volatile("mcr p15, 0, r0, c7, c0, 4");
  }
}

static u32 str_len(const char *s) {
  u32 len = 0;
  while (*s++)
    len++;
  return len;
}

static u32 prev_keys = 0;

u32 get_keys(void) {
  return ~REG_HID_PAD & 0x3FF;
}

u32 get_keys_down(void) {
  u32 cur = get_keys();
  u32 down = cur & ~prev_keys;
  prev_keys = cur;
  return down;
}

#define BRIGHTNESS_LEVELS 5
static int current_brightness = 3; 

void set_brightness(u32 level) {
  if (level >= BRIGHTNESS_LEVELS)
    level = BRIGHTNESS_LEVELS - 1;
  current_brightness = (int)level;
}

int sd_is_inserted(void) {
  return !(sdmmc_read16(REG_SDSTATUS0) & TMIO_STAT0_SIGSTATE);
}

int sd_init(void) {
  return sd_is_inserted();
}

int check_files_directory(void) {
  return sd_init();
}

/* ------------------------------------------------------------------------ *
 * Assets, text and rectangles
 * ------------------------------------------------------------------------ */

/* The accent the OS starts with; the launcher does not read USER.dat. */
static Color lm_accent = COLOR_AURORA;

static Font lm_small, lm_font, lm_bold, lm_title;
static int lm_fonts;

/* The few pack entries the launcher draws, embedded in the FIRM by
 * tools/mklauncherassets.py and decoded here, so no SD read is needed before
 * the menu shows. */

#include "launcher_assets.h"

#define LM_ARENA     0x23400000u /* decoded entries */
#define LM_ARENA_MAX 0x00100000u

typedef struct {
  u32 id;
  u16 type, w, h;
  u32 csize, usize;
  const u8 *data;
} LmEmbedded;

static const LmEmbedded lm_emb[] = {LAUNCHER_ASSET_LIST};

static Asset lm_assets[ASSET_COUNT];

/* 0x01..0x7F: run of n copies of the next byte. 0x81..0xFF: n literals. */
static int lm_rle(const u8 *src, u32 csize, u8 *dst, u32 usize) {
  const u8 *send = src + csize;
  u8 *out = dst, *oend = dst + usize;
  while (src < send) {
    u8 c = *src++;
    if (c & 0x80) {
      u32 n = c & 0x7Fu;
      if (!n || n > (u32)(oend - out) || n > (u32)(send - src))
        return 0;
      while (n--)
        *out++ = *src++;
    } else {
      u32 n = c;
      u8 v;
      if (!n || n > (u32)(oend - out) || src >= send)
        return 0;
      v = *src++;
      while (n--)
        *out++ = v;
    }
  }
  return out == oend;
}

static const Asset *lm_asset(u32 id) {
  return (id < ASSET_COUNT && lm_assets[id].data) ? &lm_assets[id] : 0;
}

/* 0 when the asset is missing, so callers can fall back. */
static int lm_draw_boxed(volatile u8 *fb, int bx, int by, int bw, int bh,
                         int sh, u32 id, Color tint) {
  const Asset *a = lm_asset(id);
  if (!a)
    return 0;
  draw_asset(fb, bx + (bw - (int)a->w) / 2, by + (bh - (int)a->h) / 2, sh, a,
             tint);
  return 1;
}

static int lm_font_of(u32 id, Font *out) {
  const Asset *a = lm_asset(id);
  const AssetFontHead *h;
  if (!a || a->type != ASSET_TYPE_FONT)
    return 0;
  h = (const AssetFontHead *)a->data;
  out->head = h;
  out->glyphs = (const AssetGlyph *)(a->data + sizeof(AssetFontHead));
  out->atlas = a->data + sizeof(AssetFontHead) + h->count * sizeof(AssetGlyph);
  return 1;
}

static void lm_tick(void);

/* Decodes the embedded entries; one that fails to decode is left out and the
 * launcher draws its built-in art in its place. */
static void lm_load_assets(void) {
  u32 used = 0;
  u8 *arena = (u8 *)LM_ARENA;

  lm_fonts = 0;
  for (unsigned n = 0; n < sizeof(lm_emb) / sizeof(lm_emb[0]); n++) {
    const LmEmbedded *e = &lm_emb[n];
    lm_tick();
    if (e->id >= ASSET_COUNT || !e->usize || e->usize > LM_ARENA_MAX - used)
      continue;
    if (!lm_rle(e->data, e->csize, arena + used, e->usize))
      continue;
    if (e->type == ASSET_TYPE_ALPHA8) {
      if (!e->w || !e->h || (u32)e->w * e->h != e->usize)
        continue;
    } else if (e->type == ASSET_TYPE_FONT) {
      const AssetFontHead *fh = (const AssetFontHead *)(arena + used);
      if (!fh->count || !fh->atlas_w || !fh->atlas_h ||
          sizeof(AssetFontHead) + (u32)fh->count * sizeof(AssetGlyph) +
                  (u32)fh->atlas_w * fh->atlas_h !=
              e->usize)
        continue;
    } else {
      continue;
    }
    lm_assets[e->id].type = e->type;
    lm_assets[e->id].w = e->w;
    lm_assets[e->id].h = e->h;
    lm_assets[e->id].data = arena + used;
    used += (e->usize + 3u) & ~3u;
  }
  if (lm_font_of(ASSET_FONT_SMALL, &lm_small) &&
      lm_font_of(ASSET_FONT_UI, &lm_font) &&
      lm_font_of(ASSET_FONT_BOLD, &lm_bold) &&
      lm_font_of(ASSET_FONT_TITLE, &lm_title))
    lm_fonts = 1;
}

static int lm_tw(const Font *f, const char *s) {
  return lm_fonts ? text_width(f, s) : (int)str_len(s) * FONT_WIDTH;
}

static int lm_th(const Font *f) {
  return lm_fonts ? text_line_height(f) : FONT_HEIGHT;
}

/* `y` is the top of the text box with either font. */
static void lm_text(volatile u8 *fb, int x, int y, int sh, const char *s,
                    Color fg, Color bg, const Font *f) {
  if (lm_fonts)
    draw_text(fb, x, y + text_ascent(f), sh, s, fg, f);
  else
    draw_string(fb, x, y, sh, s, fg, bg);
}

static void lm_text_mid(volatile u8 *fb, int cx, int y, int sh, const char *s,
                        Color fg, Color bg, const Font *f) {
  lm_text(fb, cx - lm_tw(f, s) / 2, y, sh, s, fg, bg, f);
}

/* Snapshots, in FCRAM clear of the OS image (0x22000000), the backbuffers
 * (0x23E80000..0x23F38400) and the asset arena (0x23F40000). Each is a whole
 * screen in framebuffer layout. */
#define LM_BG_TOP  0x23000000u /* top: wallpaper, logo, version              */
#define LM_BG_WALL 0x23100000u /* bottom: wallpaper only                     */
#define LM_BG_MAIN 0x23200000u /* bottom: wallpaper, bar, title and hint     */

/* Both sides share one layout, hence one alignment. */
static void lm_copy_run(volatile u8 *d, const u8 *s, u32 n) {
  while (n && (((u32)d | (u32)s) & 3u)) {
    *d++ = *s++;
    n--;
  }
  {
    volatile u32 *dw = (volatile u32 *)d;
    const u32 *sw = (const u32 *)s;
    u32 words = n >> 2;
    for (u32 i = 0; i < words; i++)
      dw[i] = sw[i];
    d += words << 2;
    s += words << 2;
  }
  for (n &= 3u; n; n--)
    *d++ = *s++;
}

/* Copies a rectangle between two buffers of the same screen. A rectangle is
 * `w` runs of `h*3` bytes at the same offset in both. */
static void lm_copy_rect(volatile u8 *dst, const u8 *src, int sw, int sh, int x,
                         int y, int w, int h) {
  if (x < 0) {
    w += x;
    x = 0;
  }
  if (y < 0) {
    h += y;
    y = 0;
  }
  if (x + w > sw)
    w = sw - x;
  if (y + h > sh)
    h = sh - y;
  if (w <= 0 || h <= 0)
    return;
  screen_touch(dst);
  for (int c = 0; c < w; c++) {
    u32 off = ((u32)(x + c) * (u32)sh + (u32)(sh - y - h)) * 3u;
    lm_copy_run(dst + off, src + off, (u32)h * 3u);
  }
}

/* Draws the snapshot rectangle into the backbuffer. */
static void lm_restore_bottom(u32 bg, int x, int y, int w, int h) {
  lm_copy_rect(VRAM_BOT_A, (const u8 *)bg, BOT_SCREEN_WIDTH, BSH, x, y, w, h);
}

static void lm_restore_top(int x, int y, int w, int h) {
  lm_copy_rect(VRAM_TOP_LA, (const u8 *)LM_BG_TOP, TOP_SCREEN_WIDTH, TSH, x, y,
               w, h);
}

/* Sends a rectangle of the backbuffer to the panel. */
static void lm_show_bottom(int x, int y, int w, int h) {
  lm_copy_rect(VRAM_BOT_PHYS, (const u8 *)VRAM_BOT_A, BOT_SCREEN_WIDTH, BSH, x,
               y, w, h);
}

static void lm_show_top(int x, int y, int w, int h) {
  lm_copy_rect(VRAM_TOP_PHYS, (const u8 *)VRAM_TOP_LA, TOP_SCREEN_WIDTH, TSH, x,
               y, w, h);
}

/* The loading bar shown while the menu is prepared: a plain gradient on both
 * screens and a bar on the bottom one that fills one step per asset decoded
 * and per background drawn. */
#define LM_LB_W 120
#define LM_LB_H 8

static int lm_step, lm_steps = 1;

static void lm_loading_screen(void) {
  draw_vgradient(VRAM_TOP_LA, 0, 0, TOP_SCREEN_WIDTH, TSH, TSH,
                 COLOR_HM_BG_TOP, COLOR_HM_BG_BOT);
  draw_vgradient(VRAM_BOT_A, 0, 0, BOT_SCREEN_WIDTH, BSH, BSH, COLOR_HM_BG_TOP,
                 COLOR_HM_BG_BOT);
  screen_present_top();
  screen_present_bottom();
}

static void lm_tick(void) {
  volatile u8 *fb = VRAM_BOT_A;
  int x = (BOT_SCREEN_WIDTH - LM_LB_W) / 2, y = BSH / 2 - LM_LB_H / 2;
  int fill;

  if (++lm_step > lm_steps)
    lm_step = lm_steps;
  fill = LM_LB_W * lm_step / lm_steps;
  draw_filled_round_rect(fb, x, y, LM_LB_W, LM_LB_H, LM_LB_H / 2, BSH,
                         COLOR_PANEL_TOP);
  if (fill > 0)
    draw_filled_round_rect(fb, x, y, fill, LM_LB_H, LM_LB_H / 2, BSH,
                           lm_accent);
  lm_show_bottom(x - 2, y - 2, LM_LB_W + 4, LM_LB_H + 4);
}

static void lm_snapshot(u32 bg, volatile u8 *fb, int sw, int sh) {
  lm_copy_rect((volatile u8 *)bg, (const u8 *)fb, sw, sh, 0, 0, sw, sh);
}

/* The Home Menu's background: the theme gradient with the pack's pattern
 * tinted by the accent, or diagonal facets without the pack. Slow, so only
 * ever drawn into a snapshot. */
static void lm_wallpaper(volatile u8 *fb, int w, int sh, Color top,
                         Color bot) {
  const Asset *a = lm_asset(ASSET_WALLPAPER);

  draw_vgradient(fb, 0, 0, w, sh, sh, top, bot);
  if (a) {
    draw_asset(fb, 0, 0, sh, a, lm_accent);
    return;
  }
  for (int k = -6; k <= w / 46 + 6; k++) {
    for (int py = 0; py < sh; py++) {
      int px = 46 * k + py;
      if (px >= 0 && px < w)
        draw_pixel(fb, px, py, sh, COLOR_HM_FACET);
      px = 46 * k + sh - py;
      if (px >= 0 && px < w)
        draw_pixel(fb, px, py, sh, COLOR_HM_FACET);
    }
  }
}

/* The logo at half size, each pixel the coverage of a 2x2 block. */
static int lm_logo_bit(int col, int row) {
  return (aurora_logo_bits[row * AURORA_LOGO_ROW_BYTES + (col >> 3)] &
          (0x80 >> (col & 7)))
             ? 1
             : 0;
}

static void lm_logo_half(volatile u8 *fb, int x0, int y0, int sh, Color c) {
  for (int row = 0; row < AURORA_LOGO_HEIGHT / 2; row++) {
    for (int col = 0; col < AURORA_LOGO_WIDTH / 2; col++) {
      int n = lm_logo_bit(col * 2, row * 2) + lm_logo_bit(col * 2 + 1, row * 2) +
              lm_logo_bit(col * 2, row * 2 + 1) +
              lm_logo_bit(col * 2 + 1, row * 2 + 1);
      if (n)
        draw_pixel_alpha(fb, x0 + col, y0 + row, sh, c, n * 64);
    }
  }
}

static void lm_chevron(volatile u8 *fb, int x, int cy, Color c) {
  for (int i = 0; i < 6; i++) {
    draw_filled_rect(fb, x + i, cy - i - 1, 2, 2, BSH, c);
    draw_filled_rect(fb, x + i, cy + i - 1, 2, 2, BSH, c);
  }
}

/* The Home Menu's top bar. */
#define LM_BAR_X 8
#define LM_BAR_Y 8
#define LM_BAR_W (BOT_SCREEN_WIDTH - 16)
#define LM_BAR_H 30

static void lm_bar(volatile u8 *fb) {
  draw_filled_round_rect(fb, LM_BAR_X, LM_BAR_Y, LM_BAR_W, LM_BAR_H, 10, BSH,
                         COLOR_HM_BAR);
}

/* A filled play triangle, anti-aliased by 4x4 supersampling, since the pack
 * has no play icon. `x`,`y` is the top left of a px-by-px box (px <= 64). */
static void lm_play(volatile u8 *fb, int x, int y, int sh, int px, Color c) {
  int n = px * 4; /* the sample grid, in samples */
  int ax = n * 22 / 100, ay = n * 12 / 100, by = n * 88 / 100;
  int cx = n * 90 / 100, cy = n / 2;
  int half = cy - ay;
  int cov[64];

  for (int j = 0; j < px; j++) {
    for (int i = 0; i < px; i++)
      cov[i] = 0;
    for (int s = 0; s < 4; s++) {
      /* Everything below is in half samples, so a sample's centre is whole. */
      int sy2 = (j * 4 + s) * 2 + 1;
      int dy2, xr2;
      if (sy2 < ay * 2 || sy2 > by * 2)
        continue;
      dy2 = sy2 > cy * 2 ? sy2 - cy * 2 : cy * 2 - sy2;
      xr2 = cx * 2 - (cx - ax) * dy2 / half;
      for (int i = 0; i < px; i++) {
        int lo = ax * 2 > i * 8 ? ax * 2 : i * 8;
        int hi = xr2 < i * 8 + 8 ? xr2 : i * 8 + 8;
        if (hi > lo)
          cov[i] += hi - lo;
      }
    }
    for (int i = 0; i < px; i++)
      if (cov[i] > 0)
        draw_pixel_alpha(fb, x + i, y + j, sh, c, cov[i] * 8);
  }
}

enum { LM_BOOT, LM_POWER, LM_SETTINGS, LM_COUNT };

static const char *const lm_names[LM_COUNT] = {"Boot Aurora", "Power Off",
                                               "Settings"};
static const char *const lm_desc[LM_COUNT] = {
    "Start AuroraOS from the SD card", "Turn the console off",
    "Screen brightness"};
static const Color lm_tint[LM_COUNT] = {
    {0x2D, 0x8F, 0x83}, {0xC0, 0x20, 0x20}, {0x4A, 0x4C, 0x58}};

/* An icon of `px` pixels (32, 48 or 64) centred on (cx, cy): pack art when it
 * loaded, else the built-in bitmap. */
static void lm_icon(volatile u8 *fb, int cx, int cy, int sh, int kind, int px,
                    Color tint) {
  u32 id;
  int scale = px >= 64 ? 2 : 1;
  if (kind == LM_BOOT) {
    lm_play(fb, cx - px / 2, cy - px / 2, sh, px, tint);
    return;
  }
  if (kind == LM_POWER)
    id = px >= 64 ? ASSET_ICON_POWER_64
                  : px >= 48 ? ASSET_ICON_POWER_48 : ASSET_ICON_POWER_32;
  else
    id = px >= 64 ? ASSET_ICON_SETTINGS_64
                  : px >= 48 ? ASSET_ICON_SETTINGS_48 : ASSET_ICON_SETTINGS_32;
  if (lm_draw_boxed(fb, cx - px / 2, cy - px / 2, px, px, sh, id, tint))
    return;
  draw_icon_scaled(fb, cx - ICON_SIZE * scale / 2, cy - ICON_SIZE * scale / 2,
                   sh, kind == LM_POWER ? icon_power_bits : icon_settings_bits,
                   tint, scale);
}

/* ------------------------------------------------------------------------ *
 * The launcher screen
 * ------------------------------------------------------------------------ */

/* The Home Menu's tile and selection metrics, with the tile a little larger
 * since only three are shown. */
#define LM_SLOT 54
#define LM_GAP  14
#define LM_STEP (LM_SLOT + LM_GAP)
#define LM_ROW_W (LM_SLOT * 3 + LM_GAP * 2)
#define LM_X0   ((BOT_SCREEN_WIDTH - LM_ROW_W) / 2)
#define LM_Y    84
#define LM_RING 3 /* gap between a tile and its ring */
#define LM_LIFT 3 /* how far the selected tile grows on each side */
#define LM_OUT  (LM_RING + LM_LIFT)

/* What changes on the bottom screen: the tiles, the ring and the name. */
#define LM_DIRTY_X 47
#define LM_DIRTY_Y 70
#define LM_DIRTY_W 226
#define LM_DIRTY_H 116

/* Top screen layout: logo, version, then the box and card of the selection. */
#define LM_TOP_BG_TOP ((Color){0x3A, 0x3D, 0x4A})
#define LM_TOP_BG_BOT ((Color){0x1E, 0x20, 0x29})
#define LM_LOGO_W (AURORA_LOGO_WIDTH / 2)
#define LM_LOGO_Y 14
#define LM_VER_Y  48
#define LM_BOX    84
#define LM_BOX_Y  72
#define LM_CARD_W 280
#define LM_CARD_X ((TOP_SCREEN_WIDTH - LM_CARD_W) / 2)
#define LM_CARD_Y 164
#define LM_CARD_H 58

/* What changes on the top screen. */
#define LM_TDIRTY_X 50
#define LM_TDIRTY_Y 66
#define LM_TDIRTY_W 300
#define LM_TDIRTY_H 164

static int lm_tile_x(int i) { return LM_X0 + i * LM_STEP; }

static void lm_tile(volatile u8 *fb, int kind, int x, int y, int size) {
  int r = 10 + (size - LM_SLOT) / 4;
  draw_gradient_round_rect(fb, x, y, size, size, r, BSH, COLOR_HM_SLOT_TOP,
                           COLOR_HM_SLOT_BOT);
  lm_icon(fb, x + size / 2, y + size / 2, BSH, kind, 32, COLOR_WHITE);
}

/* Draws what never changes into the snapshots. The pack must have loaded (or
 * failed) by now. */
static void lm_build_backgrounds(void) {
  volatile u8 *fb = VRAM_TOP_LA;
  int th = lm_th(&lm_bold);

  /* The top is mostly bare wallpaper, so it gets a lighter ground than the
   * bottom, whose tiles and bar carry the light. */
  lm_wallpaper(fb, TOP_SCREEN_WIDTH, TSH, LM_TOP_BG_TOP, LM_TOP_BG_BOT);
  lm_logo_half(fb, (TOP_SCREEN_WIDTH - LM_LOGO_W) / 2, LM_LOGO_Y, TSH,
               COLOR_WHITE);
  lm_text_mid(fb, TOP_SCREEN_WIDTH / 2, LM_VER_Y, TSH, AURORA_VERSION,
              COLOR_HM_TEXT2, COLOR_HM_BG_TOP, &lm_small);
  lm_snapshot(LM_BG_TOP, fb, TOP_SCREEN_WIDTH, TSH);
  lm_tick();

  fb = VRAM_BOT_A;
  lm_wallpaper(fb, BOT_SCREEN_WIDTH, BSH, COLOR_HM_BG_TOP, COLOR_HM_BG_BOT);
  lm_snapshot(LM_BG_WALL, fb, BOT_SCREEN_WIDTH, BSH);
  lm_tick();

  /* The tick drew the loading bar into the backbuffer; start the next
   * snapshot from the clean wallpaper so the bar is not baked into it. */
  lm_restore_bottom(LM_BG_WALL, 0, 0, BOT_SCREEN_WIDTH, BSH);

  lm_bar(fb);
  draw_filled_round_rect(fb, LM_BAR_X + 12, LM_BAR_Y + 8, 4, 14, 2, BSH,
                         lm_accent);
  lm_text(fb, LM_BAR_X + 24, LM_BAR_Y + (LM_BAR_H - th) / 2, BSH,
          "Aurora Launcher", COLOR_WHITE, COLOR_HM_BAR, &lm_bold);
  lm_text_mid(fb, BOT_SCREEN_WIDTH / 2, BSH - 24, BSH,
              "A:OK  START:SD  SELECT:FS", COLOR_HM_TEXT2, COLOR_HM_BG_BOT,
              &lm_small);
  lm_snapshot(LM_BG_MAIN, fb, BOT_SCREEN_WIDTH, BSH);
  lm_tick();
}

/* The bottom screen, with tile `i` grown by lift[i] pixels and the ring's left
 * edge at `ring_x`. `full` repaints and sends the whole screen. */
static void lm_bottom(int sel, const int *lift, int ring_x, int full) {
  volatile u8 *fb = VRAM_BOT_A;
  int x = full ? 0 : LM_DIRTY_X, y = full ? 0 : LM_DIRTY_Y;
  int w = full ? BOT_SCREEN_WIDTH : LM_DIRTY_W;
  int h = full ? BSH : LM_DIRTY_H;

  lm_restore_bottom(LM_BG_MAIN, x, y, w, h);

  for (int i = 0; i < LM_COUNT; i++) {
    int l = lift[i];
    lm_tile(fb, i, lm_tile_x(i) - l, LM_Y - l, LM_SLOT + 2 * l);
  }

  {
    int s = LM_SLOT + 2 * LM_OUT;
    draw_round_ring(fb, ring_x, LM_Y - LM_OUT, s, s, 10 + LM_LIFT / 2 + LM_RING,
                    LM_RING + 1, BSH, lm_accent);
  }

  lm_text_mid(fb, BOT_SCREEN_WIDTH / 2, LM_Y + LM_SLOT + 24, BSH, lm_names[sel],
              COLOR_WHITE, COLOR_HM_BG, &lm_bold);
  lm_show_bottom(x, y, w, h);
}

/* The top screen: like the Home Menu's preview, a tinted box with the icon
 * of the selected entry and a card with its name. */
static void lm_top(int sel, int full) {
  volatile u8 *fb = VRAM_TOP_LA;
  Color tint = lm_tint[sel], dark;
  int x = full ? 0 : LM_TDIRTY_X, y = full ? 0 : LM_TDIRTY_Y;
  int w = full ? TOP_SCREEN_WIDTH : LM_TDIRTY_W;
  int h = full ? TSH : LM_TDIRTY_H;
  int bx = (TOP_SCREEN_WIDTH - LM_BOX) / 2;
  int cx = TOP_SCREEN_WIDTH / 2;

  dark.r = (u8)(tint.r * 5 / 9);
  dark.g = (u8)(tint.g * 5 / 9);
  dark.b = (u8)(tint.b * 5 / 9);

  lm_restore_top(x, y, w, h);

  draw_gradient_round_rect(fb, bx, LM_BOX_Y, LM_BOX, LM_BOX, 18, TSH, tint,
                           dark);
  lm_icon(fb, cx, LM_BOX_Y + LM_BOX / 2, TSH, sel, 64, COLOR_WHITE);

  draw_gradient_round_rect(fb, LM_CARD_X, LM_CARD_Y, LM_CARD_W, LM_CARD_H, 12,
                           TSH, COLOR_PANEL_TOP, COLOR_PANEL_BOT);
  lm_text_mid(fb, cx, LM_CARD_Y + 6, TSH, lm_names[sel], COLOR_WHITE,
              COLOR_PANEL_TOP, &lm_title);
  lm_text_mid(fb, cx, LM_CARD_Y + 8 + lm_th(&lm_title), TSH, lm_desc[sel],
              COLOR_HM_TEXT2, COLOR_PANEL_BOT, &lm_font);
  lm_show_top(x, y, w, h);
}

/* Moves the ring from `from` to `to` on a small spring, growing the new tile
 * and shrinking the old. With from == to it just pops the tile up. */
static void lm_select(int from, int to) {
  static const int up[4] = {1, 3, 4, 3};
  static const int dn[4] = {2, 0, 0, 0};
  static const int ease[4] = {45, 85, 104, 100};
  int x0 = lm_tile_x(from) - LM_OUT, x1 = lm_tile_x(to) - LM_OUT;

  for (int f = 0; f < 4; f++) {
    int lift[LM_COUNT] = {0, 0, 0};
    if (from != to)
      lift[from] = dn[f];
    lift[to] = up[f];
    lm_bottom(to, lift, x0 + (x1 - x0) * ease[f] / 100, 0);
  }
}

/* Both screens, settled on `sel`. */
static void lm_redraw(int sel) {
  int lift[LM_COUNT] = {0, 0, 0};
  lift[sel] = LM_LIFT;
  lm_top(sel, 1);
  lm_bottom(sel, lift, lm_tile_x(sel) - LM_OUT, 1);
}

void ui_draw_home_screen(void) { lm_redraw(0); }

int ui_power_off_confirm(void) {
  volatile u8 *fb = VRAM_BOT_A;
  Color tint = lm_tint[LM_POWER], dark;
  int size = 72, x = (BOT_SCREEN_WIDTH - size) / 2, y = 60;
  const char *msg = "Powering off...";

  dark.r = (u8)(tint.r * 5 / 9);
  dark.g = (u8)(tint.g * 5 / 9);
  dark.b = (u8)(tint.b * 5 / 9);

  lm_restore_bottom(LM_BG_WALL, 0, 0, BOT_SCREEN_WIDTH, BSH);
  draw_gradient_round_rect(fb, x, y, size, size, 16, BSH, tint, dark);
  lm_icon(fb, x + size / 2, y + size / 2, BSH, LM_POWER, 48, COLOR_WHITE);
  lm_text_mid(fb, BOT_SCREEN_WIDTH / 2, y + size + 18, BSH, msg, COLOR_WHITE,
              COLOR_HM_BG, &lm_bold);
  screen_present_bottom();

  return 0;
}

/* ------------------------------------------------------------------------ *
 * Settings
 * ------------------------------------------------------------------------ */

#define ST_CARD_X 16
#define ST_CARD_Y 52
#define ST_CARD_W (BOT_SCREEN_WIDTH - 32)
#define ST_CARD_H 110
#define ST_TRACK_X (ST_CARD_X + 16)
#define ST_TRACK_Y (ST_CARD_Y + 72)
#define ST_TRACK_W (ST_CARD_W - 32)
#define ST_TRACK_H 10

/* The bar and hint, drawn once on entering. */
static void draw_settings_frame(void) {
  volatile u8 *fb = VRAM_BOT_A;
  int th = lm_th(&lm_bold);

  lm_restore_bottom(LM_BG_WALL, 0, 0, BOT_SCREEN_WIDTH, BSH);
  lm_bar(fb);
  lm_chevron(fb, LM_BAR_X + 14, LM_BAR_Y + LM_BAR_H / 2, COLOR_HM_TEXT2);
  lm_text(fb, LM_BAR_X + 32, LM_BAR_Y + (LM_BAR_H - th) / 2, BSH, "Settings",
          COLOR_WHITE, COLOR_HM_BAR, &lm_bold);
  lm_text_mid(fb, BOT_SCREEN_WIDTH / 2, BSH - 24, BSH,
              "D-Pad: Adjust   B: Back", COLOR_HM_TEXT2, COLOR_HM_BG_BOT,
              &lm_small);
}

/* The card, redrawn alone when the level changes. */
static void draw_settings_card(int full) {
  volatile u8 *fb = VRAM_BOT_A;
  int th = lm_th(&lm_bold);
  int seg = ST_TRACK_W / BRIGHTNESS_LEVELS;
  int fill = seg * (current_brightness + 1);
  char level[4];

  lm_restore_bottom(LM_BG_WALL, ST_CARD_X - 2, ST_CARD_Y - 2, ST_CARD_W + 4,
                    ST_CARD_H + 4);

  draw_gradient_round_rect(fb, ST_CARD_X, ST_CARD_Y, ST_CARD_W, ST_CARD_H, 14,
                           BSH, COLOR_PANEL_TOP, COLOR_PANEL_BOT);

  if (!lm_draw_boxed(fb, ST_CARD_X + 14, ST_CARD_Y + 14, 32, 32, BSH,
                     ASSET_ICON_DISPLAY_32, COLOR_WHITE))
    draw_icon_32(fb, ST_CARD_X + 14, ST_CARD_Y + 14, BSH, icon_brightness_bits,
                 COLOR_WHITE);
  lm_text(fb, ST_CARD_X + 58, ST_CARD_Y + 14, BSH, "Screen Brightness",
          COLOR_WHITE, COLOR_PANEL_TOP, &lm_bold);

  level[0] = (char)('1' + current_brightness);
  level[1] = '/';
  level[2] = (char)('0' + BRIGHTNESS_LEVELS);
  level[3] = '\0';
  lm_text(fb, ST_CARD_X + 58, ST_CARD_Y + 14 + th + 2, BSH, level,
          COLOR_HM_TEXT2, COLOR_PANEL_TOP, &lm_font);

  draw_filled_round_rect(fb, ST_TRACK_X, ST_TRACK_Y, ST_TRACK_W, ST_TRACK_H, 5,
                         BSH, COLOR_HM_BAR);
  draw_filled_round_rect(fb, ST_TRACK_X, ST_TRACK_Y, fill, ST_TRACK_H, 5, BSH,
                         lm_accent);
  draw_filled_round_rect(fb, ST_TRACK_X + fill - 9,
                         ST_TRACK_Y + ST_TRACK_H / 2 - 9, 18, 18, 9, BSH,
                         COLOR_WHITE);

  if (full)
    screen_present_bottom();
  else
    lm_show_bottom(ST_CARD_X - 2, ST_CARD_Y - 2, ST_CARD_W + 4, ST_CARD_H + 4);
}

void ui_settings_menu(void) {
  draw_settings_frame();
  draw_settings_card(1);

  while (1) {
    u32 kdown = get_keys_down();

    if (kdown & BUTTON_DRIGHT) {
      if (current_brightness < BRIGHTNESS_LEVELS - 1) {
        set_brightness((u32)(current_brightness + 1));
        draw_settings_card(0);
      }
    }

    if (kdown & BUTTON_DLEFT) {
      if (current_brightness > 0) {
        set_brightness((u32)(current_brightness - 1));
        draw_settings_card(0);
      }
    }

    if (kdown & BUTTON_B) {
      return; 
    }

    delay(60000);
  }
}

/* ------------------------------------------------------------------------ *
 * Diagnostics (START: SD card test, SELECT: FatFs test)
 * ------------------------------------------------------------------------ */

static char hex_nibble(u8 v) {
  return (char)(v < 10 ? '0' + v : 'A' + (v - 10));
}

static char *str_cpy_ret(char *dst, const char *src) {
  while ((*dst = *src)) {
    dst++;
    src++;
  }
  return dst;
}

static void int_to_str(int v, char *out) {
  char tmp[12];
  int i = 0;
  unsigned uv = (v < 0) ? (unsigned)(-v) : (unsigned)v;
  if (uv == 0)
    tmp[i++] = '0';
  while (uv) {
    tmp[i++] = (char)('0' + (uv % 10));
    uv /= 10;
  }
  int p = 0;
  if (v < 0)
    out[p++] = '-';
  while (i)
    out[p++] = tmp[--i];
  out[p] = '\0';
}

static void build_hex_line(const u8 *data, int n, char *out) {
  int p = 0;
  for (int i = 0; i < n; i++) {
    out[p++] = hex_nibble((u8)(data[i] >> 4));
    out[p++] = hex_nibble((u8)(data[i] & 0xF));
    out[p++] = ' ';
  }
  out[p] = '\0';
}

static u8 sd_test_buf[512];

void ui_sd_test(void) {
  clear_screen(VRAM_BOT_A, BOT_FB_SIZE, COLOR_BG_DARK);

  const char *title = "SD Card Test";
  int title_len = (int)str_len(title);
  int title_x = (BOT_SCREEN_WIDTH - title_len * FONT_WIDTH) / 2;
  draw_string(VRAM_BOT_A, title_x, 8, BOT_SCREEN_HEIGHT, title, COLOR_AURORA,
              COLOR_BG_DARK);
  screen_present_bottom();

  char line[48];
  int y = 32;

  int init_res = sdmmc_sdcard_init();
  char *p = str_cpy_ret(line, "Init: ");
  int_to_str(init_res, p);
  draw_string(VRAM_BOT_A, 8, y, BOT_SCREEN_HEIGHT, line,
              init_res == 0 ? COLOR_GREEN : COLOR_RED, COLOR_BG_DARK);
  y += 16;

  if (init_res != 0) {
    draw_string(VRAM_BOT_A, 8, y, BOT_SCREEN_HEIGHT, "SD init failed",
                COLOR_RED, COLOR_BG_DARK);
  } else {
    int read_res = sdmmc_sdcard_readsectors(0, 1, sd_test_buf);
    p = str_cpy_ret(line, "Read: ");
    int_to_str(read_res, p);
    draw_string(VRAM_BOT_A, 8, y, BOT_SCREEN_HEIGHT, line,
                read_res == 0 ? COLOR_GREEN : COLOR_RED, COLOR_BG_DARK);
    y += 20;

    draw_string(VRAM_BOT_A, 8, y, BOT_SCREEN_HEIGHT, "Sector 0, bytes 0-15:",
                COLOR_LIGHT_GRAY, COLOR_BG_DARK);
    y += 16;
    build_hex_line(&sd_test_buf[0], 8, line);
    draw_string(VRAM_BOT_A, 8, y, BOT_SCREEN_HEIGHT, line, COLOR_WHITE,
                COLOR_BG_DARK);
    y += 12;
    build_hex_line(&sd_test_buf[8], 8, line);
    draw_string(VRAM_BOT_A, 8, y, BOT_SCREEN_HEIGHT, line, COLOR_WHITE,
                COLOR_BG_DARK);
    y += 20;

    p = str_cpy_ret(line, "Signature: ");
    *p++ = hex_nibble((u8)(sd_test_buf[510] >> 4));
    *p++ = hex_nibble((u8)(sd_test_buf[510] & 0xF));
    *p++ = hex_nibble((u8)(sd_test_buf[511] >> 4));
    *p++ = hex_nibble((u8)(sd_test_buf[511] & 0xF));
    *p = '\0';
    draw_string(VRAM_BOT_A, 8, y, BOT_SCREEN_HEIGHT, line, COLOR_WHITE,
                COLOR_BG_DARK);
    y += 16;

    int pass = (sd_test_buf[510] == 0x55 && sd_test_buf[511] == 0xAA);
    draw_string(VRAM_BOT_A, 8, y, BOT_SCREEN_HEIGHT,
                pass ? "PASS: valid boot sector" : "No 0x55AA signature",
                pass ? COLOR_GREEN : COLOR_YELLOW, COLOR_BG_DARK);
  }

  draw_string(VRAM_BOT_A, 8, BOT_SCREEN_HEIGHT - 20, BOT_SCREEN_HEIGHT,
              "B: Back", COLOR_LIGHT_GRAY, COLOR_BG_DARK);
  screen_present_bottom();

  while (1) {
    u32 kdown = get_keys_down();
    if (kdown & BUTTON_B)
      return;
    delay(60000);
  }
}

static FATFS fs_volume;
static FIL fs_file;

void ui_fs_test(void) {
  clear_screen(VRAM_BOT_A, BOT_FB_SIZE, COLOR_BG_DARK);

  const char *title = "FatFs Test";
  int title_len = (int)str_len(title);
  int title_x = (BOT_SCREEN_WIDTH - title_len * FONT_WIDTH) / 2;
  draw_string(VRAM_BOT_A, title_x, 8, BOT_SCREEN_HEIGHT, title, COLOR_AURORA,
              COLOR_BG_DARK);
  screen_present_bottom();

  char line[48];
  int y = 32;

  FRESULT fr = f_mount(&fs_volume, "", 1);
  char *p = str_cpy_ret(line, "Mount: ");
  int_to_str((int)fr, p);
  draw_string(VRAM_BOT_A, 8, y, BOT_SCREEN_HEIGHT, line,
              fr == FR_OK ? COLOR_GREEN : COLOR_RED, COLOR_BG_DARK);
  y += 16;

  if (fr == FR_OK) {
    fr = f_open(&fs_file, "test.txt", FA_READ);
    p = str_cpy_ret(line, "Open test.txt: ");
    int_to_str((int)fr, p);
    draw_string(VRAM_BOT_A, 8, y, BOT_SCREEN_HEIGHT, line,
                fr == FR_OK ? COLOR_GREEN : COLOR_RED, COLOR_BG_DARK);
    y += 16;

    if (fr == FR_OK) {
      char data[33];
      UINT br = 0;
      fr = f_read(&fs_file, data, sizeof(data) - 1, &br);
      f_close(&fs_file);

      p = str_cpy_ret(line, "Read: ");
      p = str_cpy_ret(p, (fr == FR_OK) ? "OK  bytes=" : "ERR bytes=");
      int_to_str((int)br, p);
      draw_string(VRAM_BOT_A, 8, y, BOT_SCREEN_HEIGHT, line,
                  fr == FR_OK ? COLOR_GREEN : COLOR_RED, COLOR_BG_DARK);
      y += 20;

      for (UINT i = 0; i < br; i++) {
        if (data[i] < 0x20 || data[i] > 0x7E)
          data[i] = '.';
      }
      data[br] = '\0';
      draw_string(VRAM_BOT_A, 8, y, BOT_SCREEN_HEIGHT, "Contents:",
                  COLOR_LIGHT_GRAY, COLOR_BG_DARK);
      y += 16;
      draw_string(VRAM_BOT_A, 8, y, BOT_SCREEN_HEIGHT, data, COLOR_WHITE,
                  COLOR_BG_DARK);
      y += 20;

      if (fr == FR_OK && br > 0)
        draw_string(VRAM_BOT_A, 8, y, BOT_SCREEN_HEIGHT,
                    "PASS: file read OK", COLOR_GREEN, COLOR_BG_DARK);
    } else {
      draw_string(VRAM_BOT_A, 8, y, BOT_SCREEN_HEIGHT,
                  "Put test.txt on SD root", COLOR_YELLOW, COLOR_BG_DARK);
    }
  }

  f_mount(NULL, "", 0);

  draw_string(VRAM_BOT_A, 8, BOT_SCREEN_HEIGHT - 20, BOT_SCREEN_HEIGHT,
              "B: Back", COLOR_LIGHT_GRAY, COLOR_BG_DARK);
  screen_present_bottom();

  while (1) {
    u32 kdown = get_keys_down();
    if (kdown & BUTTON_B)
      return;
    delay(60000);
  }
}

int main(void) {
  int selection = 0;

  screen_init();
  /* Draw into FCRAM and copy finished rectangles to the panels, as the OS
   * does, instead of drawing straight into the slow framebuffer. */
  screen_use_backbuffer(1);
  set_brightness((u32)current_brightness);

  lm_loading_screen();
  lm_steps = (int)(sizeof(lm_emb) / sizeof(lm_emb[0])) + 3;
  lm_load_assets();
  lm_build_backgrounds();

  /* Both screens whole, settled on the first entry: the animation only
   * redraws a rectangle, so the rest must be on screen already. */
  lm_redraw(selection);

  while (1) {
    u32 kdown = get_keys_down();

    if (kdown & BUTTON_DLEFT) {
      if (selection > 0) {
        int old = selection--;
        lm_select(old, selection);
        lm_top(selection, 0);
      }
    }

    if (kdown & BUTTON_DRIGHT) {
      if (selection < LM_COUNT - 1) {
        int old = selection++;
        lm_select(old, selection);
        lm_top(selection, 0);
      }
    }

    if (kdown & BUTTON_A) {
      if (selection == LM_BOOT) {
        boot_aurora();
        lm_redraw(selection);
      } else if (selection == LM_POWER) {
        if (!ui_power_off_confirm())
          power_off();
        lm_redraw(selection);
      } else if (selection == LM_SETTINGS) {
        ui_settings_menu();
        lm_redraw(selection);
      }
    }

    if (kdown & BUTTON_START) {
      ui_sd_test();
      lm_redraw(selection);
    }

    if (kdown & BUTTON_SELECT) {
      ui_fs_test();
      lm_redraw(selection);
    }

    delay(60000);
  }

  return 0;
}
