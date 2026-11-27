#include "assets.h"
#include "aurora.h"
#include "aurora_logo.h"
#include "ff.h"
#include "font.h"
#include "fx.h"
#include "i2c.h"
#include "icons.h"
#include "loader.h"
#include "power.h"
#include "screen11.h"
#include "sdmmc.h"
#include "user.h"
#include "wipe.h"

#define DELAY_MS(ms) delay((ms) * 7000u) /* delay() is in src/fx.c */

/* The firm's ARM11 points the panels at Aurora's framebuffers as it starts
 * (src/screen11.c). If the loader left them off, the LCDs still need power,
 * over I2C, in Luma3DS's order (arm9/source/screen.c initScreens), with its
 * waits doubled. */
static void screens_ready(void) {
  u32 state = SCREEN11_BUSY;
  for (int i = 0; i < 500 && (state = SCREEN11_STATE) == SCREEN11_BUSY; i++)
    DELAY_MS(1);
  if (state != SCREEN11_POWER)
    return;

  I2C_init();
  I2C_writeReg(I2C_DEV_LCD_TOP, 0xFE, 0xAA); /* controller out of reset */
  I2C_writeReg(I2C_DEV_LCD_BOT, 0xFE, 0xAA);
  DELAY_MS(10);
  I2C_writeReg(I2C_DEV_LCD_TOP, 0x01, 0x10); /* controller power */
  I2C_writeReg(I2C_DEV_LCD_BOT, 0x01, 0x10);
  DELAY_MS(10);
  I2C_writeReg(I2C_DEV_LCD_TOP, 0x60, 0x00); /* clear the error flag */
  I2C_writeReg(I2C_DEV_LCD_BOT, 0x60, 0x00);
  DELAY_MS(10);
  I2C_writeReg(I2C_DEV_MCU, 0x22, 0x02); /* LCD power */
  DELAY_MS(100);
  I2C_writeReg(I2C_DEV_MCU, 0x22, 0x28); /* both backlights */
  DELAY_MS(10);
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
  return ~REG_HID_PAD & 0xFFF;
}

u32 get_keys_down(void) {
  u32 cur = get_keys();
  u32 down = cur & ~prev_keys;
  prev_keys = cur;
  return down;
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

/* ---- Home-Menu-style typography & icon helpers (no OS dependencies) ---- */

static Font bm_small, bm_font, bm_bold, bm_title;
static int bm_assets_ok;

static void bm_assets_init(void) {
  bm_assets_ok = assets_load();
  if (!bm_assets_ok)
    return;
  if (!asset_font(ASSET_FONT_SMALL, &bm_small) ||
      !asset_font(ASSET_FONT_UI, &bm_font) ||
      !asset_font(ASSET_FONT_BOLD, &bm_bold) ||
      !asset_font(ASSET_FONT_TITLE, &bm_title))
    bm_assets_ok = 0;
}

static int bm_have(const Font *f) { return bm_assets_ok && f && f->head != 0; }

static int bm_tw(const Font *f, const char *s) {
  return bm_have(f) ? text_width(f, s) : (int)str_len(s) * FONT_WIDTH;
}

static int bm_th(const Font *f) {
  return bm_have(f) ? text_line_height(f) : FONT_HEIGHT;
}

static void bm_text(volatile u8 *fb, int x, int y, int sh, const char *s,
                    Color fg, Color bg, const Font *f) {
  if (bm_have(f))
    draw_text(fb, x, y + text_ascent(f), sh, s, fg, f);
  else
    draw_string(fb, x, y, sh, s, fg, bg);
}

static void bm_text_mid(volatile u8 *fb, int cx, int y, int sh, const char *s,
                        Color fg, Color bg, const Font *f) {
  bm_text(fb, cx - bm_tw(f, s) / 2, y, sh, s, fg, bg, f);
}

/* A path too wide for `maxw` loses its start: "...payloads/Aurora.firm". */
static void bm_fit_left(char *out, u32 outsz, const Font *f, const char *s,
                        int maxw) {
  const char *p = s;
  u32 n;
  while (*p) {
    char *o = out;
    if (p != s) {
      o[0] = o[1] = o[2] = '.';
      o += 3;
    }
    for (n = 0; p[n] && (u32)(o - out) + n < outsz - 1u; n++)
      o[n] = p[n];
    o[n] = 0;
    if (bm_tw(f, out) <= maxw)
      return;
    p++;
    while ((*p & 0xC0) == 0x80)
      p++;
  }
  out[0] = 0;
}

static void bm_icon(volatile u8 *fb, int bx, int by, int box, int sh,
                    u32 asset_id, const unsigned char *bits, Color tint) {
  if (bm_assets_ok && draw_asset_boxed(fb, bx, by, box, box, sh, asset_id, tint))
    return;
  if (bits) {
    int s = box / ICON_SIZE;
    if (s < 1)
      s = 1;
    draw_icon_scaled(fb, bx + (box - ICON_SIZE * s) / 2,
                     by + (box - ICON_SIZE * s) / 2, sh, bits, tint, s);
  }
}

static void bm_bg(volatile u8 *fb, int w, int h) {
  draw_vgradient(fb, 0, 0, w, h, h, COLOR_HM_BG_TOP, COLOR_HM_BG_BOT);
}

#define COLOR_WARN  ((Color){0xFF, 0x5A, 0x52})
#define COLOR_AMBER ((Color){0xFF, 0xB0, 0x3A})
#define COLOR_OK    ((Color){0x5A, 0xD8, 0x7A})

/* Timings in milliseconds. */
#define T_INTRO 600  /* the menu coming in, every part */
#define T_MOVE  200  /* the highlight from tile to tile */
#define T_SLIDE 300  /* the top preview's carousel */
#define T_PRESS 150  /* a tile pressed */
#define T_PUSH  280  /* one screen pushing another off */
#define T_PULSE 1600 /* the highlight's glow, one cycle */
#define T_HOLD  3000 /* L + R + Y held for a wipe */

/* 0..256 and back over each T_PULSE. */
static int pulse(u32 now) {
  u32 p = now % T_PULSE, half = T_PULSE / 2;
  int t = (int)(((p < half ? p : T_PULSE - p) << 8) / half);
  return fx_ease_in_out(t);
}

static void top_bar(volatile u8 *fb, int y, const char *title, Color c) {
  const int W = BOT_SCREEN_WIDTH, H = BOT_SCREEN_HEIGHT;
  draw_filled_round_rect(fb, 8, y, W - 16, 30, 10, H, COLOR_HM_BAR);
  bm_text_mid(fb, W / 2, y + (30 - bm_th(&bm_bold)) / 2, H, title, c,
              COLOR_HM_BAR, &bm_bold);
}

static void hint_pill(volatile u8 *fb, int y, const char *text) {
  const int W = BOT_SCREEN_WIDTH, H = BOT_SCREEN_HEIGHT;
  draw_filled_round_rect(fb, 12, y, W - 24, 26, 9, H, COLOR_HM_BAR);
  bm_text_mid(fb, W / 2, y + (26 - bm_th(&bm_small)) / 2, H, text,
              COLOR_HM_TEXT2, COLOR_HM_BAR, &bm_small);
}

static void status_bar(volatile u8 *fb) {
  const int W = TOP_SCREEN_WIDTH, H = TOP_SCREEN_HEIGHT;
  int vw = bm_tw(&bm_small, AURORA_VERSION);
  draw_filled_rect(fb, 0, 0, W, 22, H, COLOR_HM_BAR);
  bm_text(fb, 12, (22 - bm_th(&bm_small)) / 2, H, "AuroraOS", COLOR_AURORA,
          COLOR_HM_BAR, &bm_small);
  bm_text(fb, W - 12 - vw, (22 - bm_th(&bm_small)) / 2, H, AURORA_VERSION,
          COLOR_HM_TEXT2, COLOR_HM_BAR, &bm_small);
}

/* Eight dots round a circle, the brightest one running round. */
static void spinner(volatile u8 *fb, int cx, int cy, int r, int sh, Color c,
                    Color bg, u32 now) {
  static const s16 dir[8][2] = {{0, -256}, {181, -181}, {256, 0}, {181, 181},
                                {0, 256},  {-181, 181}, {-256, 0}, {-181, -181}};
  int head = (int)((now / 90u) & 7u), d = r / 3 + 2;
  for (int i = 0; i < 8; i++) {
    int age = (head - i) & 7;
    int a = age < 5 ? 256 - age * 48 : 40;
    draw_filled_round_rect(fb, cx + (dir[i][0] * r >> 8) - d / 2,
                           cy + (dir[i][1] * r >> 8) - d / 2, d, d, d / 2, sh,
                           fx_mix(bg, c, a));
  }
}

/* ---- The menu ---- */

typedef struct {
  const char *label;
  const char *title;
  const char *desc;
  u32 asset_sm;
  u32 asset_lg;
  const unsigned char *bits;
  Color accent;
} BootOption;

static const BootOption boot_opts[3] = {
    {
        "Start",
        "AuroraOS",
        "Launch AuroraOS on your Nintendo 3DS",
        ASSET_ICON_FILE_AURORA_32,
        ASSET_ICON_FILE_AURORA_64,
        icon_boot_bits,
        {0x64, 0xE8, 0xC8},
    },
    {
        "Power",
        "Power Off",
        "Turn off your Nintendo 3DS safely",
        ASSET_ICON_POWER_32,
        ASSET_ICON_POWER_64,
        icon_power_bits,
        {0xFF, 0x5A, 0x52},
    },
    {
        "Settings",
        "Settings",
        "Remove Aurora from this console",
        ASSET_ICON_SETTINGS_32,
        ASSET_ICON_SETTINGS_64,
        icon_settings_bits,
        {0x7A, 0xB8, 0xFF},
    },
};

#define TILE_SIZE    74
#define TILE_LIFT    4
#define TILE_GAP     18
#define TILE_Y       74
#define TILE_TOTAL_W (TILE_SIZE * 3 + TILE_GAP * 2)
#define TILE_X_START ((BOT_SCREEN_WIDTH - TILE_TOTAL_W) / 2)
#define TILE_STEP    (TILE_SIZE + TILE_GAP)

/* One moment of the menu; the draw functions only read it. */
typedef struct {
  int sel;   /* the choice, where the preview settles */
  int pos;   /* the highlight, in 1/256ths of a tile from Start */
  int intro; /* milliseconds into the intro, T_INTRO when it is done */
  int prev;  /* the preview sliding out, or -1 */
  int slide; /* 0..256 through that slide */
  int dir;   /* 1: the new preview comes in from the right */
  int press; /* 0..256: how far the chosen tile is pushed in */
  int glow;  /* 0..256: the highlight's pulse */
} MenuView;

static int intro_t(const MenuView *v, int start, int ms) {
  if (v->intro <= start)
    return 0;
  if (v->intro >= start + ms)
    return 256;
  return ((v->intro - start) << 8) / ms;
}

static int nearness(const MenuView *v, int i) {
  int d = v->pos - i * 256;
  if (d < 0)
    d = -d;
  return d >= 256 ? 0 : 256 - d;
}

static void draw_preview(volatile u8 *fb, const BootOption *opt, int dx,
                         int dy) {
  const int W = TOP_SCREEN_WIDTH, H = TOP_SCREEN_HEIGHT;
  int box = 76, bx = (W - box) / 2 + dx, by = 42 + dy;
  int cw = 264, ch = 64, cx = (W - cw) / 2 + dx, cy = 132 + dy;

  if (dx <= -W || dx >= W)
    return;
  draw_gradient_round_rect(fb, bx, by, box, box, 16, H, COLOR_HM_SLOT_TOP,
                           COLOR_HM_SLOT_BOT);
  draw_round_ring(fb, bx - 2, by - 2, box + 4, box + 4, 18, 2, H, opt->accent);
  bm_icon(fb, bx + 6, by + 6, box - 12, H, opt->asset_lg, opt->bits,
          COLOR_WHITE);
  draw_filled_round_rect(fb, cx, cy, cw, ch, 12, H, COLOR_HM_CARD);
  bm_text_mid(fb, W / 2 + dx, cy + 12, H, opt->title, COLOR_WHITE,
              COLOR_HM_CARD, &bm_title);
  bm_text_mid(fb, W / 2 + dx, cy + 40, H, opt->desc, COLOR_HM_TEXT2,
              COLOR_HM_CARD, &bm_small);
}

static void draw_menu_top(const MenuView *v) {
  volatile u8 *fb = VRAM_TOP_LA;
  const int W = TOP_SCREEN_WIDTH, H = TOP_SCREEN_HEIGHT;
  int rise = ((256 - fx_ease_out(intro_t(v, 80, 400))) * 24) >> 8;

  bm_bg(fb, W, H);
  status_bar(fb);
  if (v->prev >= 0 && v->slide < 256) {
    int off = (W * fx_ease_out(v->slide)) >> 8;
    draw_preview(fb, &boot_opts[v->prev], -v->dir * off, rise);
    draw_preview(fb, &boot_opts[v->sel], v->dir * (W - off), rise);
  } else {
    draw_preview(fb, &boot_opts[v->sel], 0, rise);
  }
  bm_text_mid(fb, W / 2, H - 22, H, "D-Pad: Choose   A: Select",
              COLOR_HM_TEXT2, COLOR_HM_BG_BOT, &bm_small);
  if (v->intro < 260)
    fx_dim(FX_TOP, fx_ease_out(intro_t(v, 0, 260)));
}

static void draw_tile(volatile u8 *fb, int idx, int near, int rise,
                      int squash) {
  const int H = BOT_SCREEN_HEIGHT;
  const BootOption *opt = &boot_opts[idx];
  int lift = (TILE_LIFT * near) >> 8;
  int size = TILE_SIZE + lift * 2 - squash * 2;
  int x = TILE_X_START + idx * TILE_STEP - lift + squash;
  int y = TILE_Y - lift + squash + rise;
  int icon_box = 36;

  draw_gradient_round_rect(fb, x, y, size, size, 14 + lift / 2, H,
                           COLOR_HM_SLOT_TOP, COLOR_HM_SLOT_BOT);
  bm_icon(fb, x + (size - icon_box) / 2, y + (size - icon_box) / 2 - 8,
          icon_box, H, opt->asset_sm, opt->bits,
          fx_mix(COLOR_WHITE, opt->accent, near));
  bm_text_mid(fb, x + size / 2, y + size - bm_th(&bm_small) - 8, H, opt->label,
              fx_mix(COLOR_HM_TEXT2, COLOR_WHITE, near), COLOR_HM_SLOT_BOT,
              &bm_small);
}

static void draw_menu_bottom(const MenuView *v) {
  volatile u8 *fb = VRAM_BOT_A;
  const int W = BOT_SCREEN_WIDTH, H = BOT_SCREEN_HEIGHT;
  int squash = (v->press * 5) >> 8, ring_rise = 0, total = 20, dx;
  int under = (v->pos + 128) >> 8, k = v->pos >> 8, k2 = k < 2 ? k + 1 : 2;
  int lower = 256 - fx_ease_out(intro_t(v, 220, 340));
  int widths[3];

  bm_bg(fb, W, H);
  top_bar(fb, fx_lerp(-34, 8, fx_ease_out(intro_t(v, 0, 340))), "AuroraOS",
          COLOR_AURORA);

  for (int i = 0; i < 3; i++) {
    int rise = ((256 - fx_ease_back(intro_t(v, 60 + i * 70, 400))) * 44) >> 8;
    draw_tile(fb, i, nearness(v, i), rise, i == v->sel ? squash : 0);
    if (i == under)
      ring_rise = rise;
  }

  {
    Color c = fx_mix(boot_opts[k].accent, boot_opts[k2].accent, v->pos & 255);
    int rs = TILE_SIZE + TILE_LIFT * 2 + 6 - squash * 2;
    int rx = TILE_X_START + ((v->pos * TILE_STEP) >> 8) - TILE_LIFT - 3 + squash;
    int ry = TILE_Y - TILE_LIFT - 3 + squash + ring_rise;
    draw_round_ring(fb, rx, ry, rs, rs, 14 + TILE_LIFT / 2 + 3, 3, H,
                    fx_mix(c, COLOR_WHITE, (v->glow * 80) >> 8));
  }

  /* Page dots: the one under the highlight stretches. */
  for (int i = 0; i < 3; i++) {
    widths[i] = 6 + ((8 * nearness(v, i)) >> 8);
    total += widths[i];
  }
  dx = W / 2 - total / 2;
  for (int i = 0; i < 3; i++) {
    draw_filled_round_rect(fb, dx, 174 + ((lower * 24) >> 8), widths[i], 6, 3,
                           H, fx_mix(COLOR_HM_TEXT2, COLOR_WHITE,
                                     nearness(v, i)));
    dx += widths[i] + 10;
  }

  hint_pill(fb, H - 36 + ((lower * 44) >> 8), "D-Pad: Choose   A: Select");
  if (v->intro < 260)
    fx_dim(FX_BOT, fx_ease_out(intro_t(v, 0, 260)));
}

static void draw_menu(void *ctx) {
  const MenuView *v = (const MenuView *)ctx;
  draw_menu_top(v);
  draw_menu_bottom(v);
}

/* ---- Settings ---- */

static const BootOption settings_preview = {
    "", "Settings", "Remove Aurora from this console", ASSET_ICON_SETTINGS_32,
    ASSET_ICON_SETTINGS_64, icon_settings_bits, {0x7A, 0xB8, 0xFF},
};

static void draw_settings_top(void) {
  volatile u8 *fb = VRAM_TOP_LA;
  const int W = TOP_SCREEN_WIDTH, H = TOP_SCREEN_HEIGHT;

  bm_bg(fb, W, H);
  status_bar(fb);
  draw_preview(fb, &settings_preview, 0, 0);
  bm_text_mid(fb, W / 2, H - 22, H, "A: Select   B: Back", COLOR_HM_TEXT2,
              COLOR_HM_BG_BOT, &bm_small);
}

static void draw_settings_bottom(int glow) {
  volatile u8 *fb = VRAM_BOT_A;
  const int W = BOT_SCREEN_WIDTH, H = BOT_SCREEN_HEIGHT;
  char path[64];

  bm_bg(fb, W, H);
  top_bar(fb, 8, "Settings", COLOR_WHITE);

  draw_gradient_round_rect(fb, 14, 52, W - 28, 62, 12, H, COLOR_HM_SLOT_TOP,
                           COLOR_HM_SLOT_BOT);
  draw_round_ring(fb, 11, 49, W - 22, 68, 15, 3, H,
                  fx_mix(COLOR_WARN, COLOR_WHITE, (glow * 80) >> 8));
  bm_icon(fb, 26, 67, 32, H, ASSET_ICON_REPORT_32, icon_power_bits,
          COLOR_WARN);
  bm_text(fb, 70, 61, H, "Wipe Aurora", COLOR_WHITE, COLOR_HM_SLOT_TOP,
          &bm_bold);
  bm_text(fb, 70, 86, H, "Delete AuroraOS and this firm", COLOR_HM_TEXT2,
          COLOR_HM_SLOT_BOT, &bm_small);

  draw_filled_round_rect(fb, 14, 126, W - 28, 62, 12, H, COLOR_HM_CARD);
  bm_text(fb, 26, 136, H, "Version", COLOR_HM_TEXT2, COLOR_HM_CARD, &bm_small);
  bm_text(fb, 86, 136, H, AURORA_VERSION, COLOR_WHITE, COLOR_HM_CARD,
          &bm_small);
  bm_text(fb, 26, 160, H, "Firm", COLOR_HM_TEXT2, COLOR_HM_CARD, &bm_small);
  bm_fit_left(path, sizeof(path), &bm_small,
              wipe_firm_known() ? wipe_firm_path() : "unknown loader", 206);
  bm_text(fb, 86, 160, H, path, COLOR_WHITE, COLOR_HM_CARD, &bm_small);

  hint_pill(fb, H - 36, "A: Select   B: Back");
}

static void draw_settings(void *ctx) {
  draw_settings_top();
  draw_settings_bottom(ctx ? *(const int *)ctx : 0);
}

/* ---- Wipe: the confirmation ---- */

#define WIPE_KEYS (BUTTON_L | BUTTON_R | BUTTON_Y)

typedef struct {
  int hold;    /* ms of L + R + Y so far */
  int cap[3];  /* 0..256: how lit each key cap is */
} ConfirmView;

static void target_row(volatile u8 *fb, int y, u32 asset, const char *name,
                       const char *what) {
  const int W = TOP_SCREEN_WIDTH, H = TOP_SCREEN_HEIGHT;
  char fit[64];
  int nw;

  bm_icon(fb, 44, y - 2, 24, H, asset, 0, COLOR_HM_TEXT2);
  bm_fit_left(fit, sizeof(fit), &bm_font, name, W - 210);
  bm_text(fb, 78, y, H, fit, COLOR_WHITE, COLOR_HM_CARD, &bm_font);
  nw = bm_tw(&bm_font, fit);
  bm_text(fb, 78 + nw + 10, y + 2, H, what, COLOR_HM_TEXT2, COLOR_HM_CARD,
          &bm_small);
}

static void draw_confirm_top(void) {
  volatile u8 *fb = VRAM_TOP_LA;
  const int W = TOP_SCREEN_WIDTH, H = TOP_SCREEN_HEIGHT;

  bm_bg(fb, W, H);
  status_bar(fb);
  draw_filled_round_rect(fb, 24, 34, W - 48, 186, 14, H, COLOR_HM_CARD);
  bm_icon(fb, 42, 48, 40, H, ASSET_ICON_REPORT_48, icon_power_bits,
          COLOR_WARN);
  bm_text(fb, 96, 46, H, "Wipe Aurora?", COLOR_WHITE, COLOR_HM_CARD,
          &bm_title);
  bm_text(fb, 96, 76, H, "This deletes from the SD card:", COLOR_HM_TEXT2,
          COLOR_HM_CARD, &bm_small);
  target_row(fb, 108, ASSET_ICON_FOLDER_24, "/Aurora", "apps, saves, settings");
  target_row(fb, 136, ASSET_ICON_FILE_BIN_24, "/AURORAOS.BIN", "the system");
  target_row(fb, 164, ASSET_ICON_FILE_BIN_24, wipe_firm_path(),
             wipe_firm_known() ? "this firm" : "if it is there");
  bm_text_mid(fb, W / 2, 194, H, "Then the console restarts. This can't be undone.",
              COLOR_AMBER, COLOR_HM_CARD, &bm_small);
}

static void draw_confirm_bottom(const ConfirmView *v) {
  volatile u8 *fb = VRAM_BOT_A;
  const int W = BOT_SCREEN_WIDTH, H = BOT_SCREEN_HEIGHT;
  static const char *const caps[3] = {"L", "R", "Y"};
  int frac = (v->hold << 8) / T_HOLD;
  int bx = 34, bw = W - 68, fill = (bw * frac) >> 8;
  char line[32];

  bm_bg(fb, W, H);
  top_bar(fb, 8, "Wipe Aurora", COLOR_WARN);
  draw_filled_round_rect(fb, 14, 48, W - 28, 140, 12, H, COLOR_HM_CARD);

  bm_text_mid(fb, W / 2, 58, H, "Hold L + R + Y", COLOR_WHITE, COLOR_HM_CARD,
              &bm_bold);
  if (v->hold <= 0) {
    bm_text_mid(fb, W / 2, 82, H, "for 3 seconds to wipe", COLOR_HM_TEXT2,
                COLOR_HM_CARD, &bm_small);
  } else {
    int left = (T_HOLD - v->hold + 99) / 100; /* tenths of a second */
    char *p = line;
    const char *s = "Keep holding... ";
    while (*s)
      *p++ = *s++;
    *p++ = (char)('0' + left / 10);
    *p++ = '.';
    *p++ = (char)('0' + left % 10);
    *p++ = ' ';
    *p++ = 's';
    *p = 0;
    bm_text_mid(fb, W / 2, 82, H, line, COLOR_AMBER, COLOR_HM_CARD, &bm_small);
  }

  for (int i = 0; i < 3; i++) {
    int x = W / 2 - 74 + i * 52, y = 106 + (v->cap[i] * 2 >> 8);
    Color face = fx_mix(COLOR_HM_SLOT_TOP, COLOR_WARN, v->cap[i]);
    draw_filled_round_rect(fb, x, y, 44, 30, 8, H, face);
    bm_text_mid(fb, x + 22, y + (30 - bm_th(&bm_bold)) / 2, H, caps[i],
                fx_mix(COLOR_HM_TEXT2, COLOR_WHITE, v->cap[i]), face, &bm_bold);
  }

  draw_filled_round_rect(fb, bx, 156, bw, 12, 6, H, COLOR_HM_BAR);
  if (fill >= 12)
    draw_filled_round_rect(fb, bx, 156, fill, 12, 6, H,
                           fx_mix(COLOR_AMBER, COLOR_WARN, frac));

  hint_pill(fb, H - 36, "B: Cancel");
}

static void draw_confirm(void *ctx) {
  draw_confirm_top();
  draw_confirm_bottom((const ConfirmView *)ctx);
}

/* ---- Wipe: the work ---- */

typedef struct {
  const WipeStatus *st;
  int ok;      /* -1 running, then wipe_run()'s answer */
  u32 drawn;   /* when the last frame went up */
} WipeView;

static const char *const step_names[3] = {"Aurora folder", "AURORAOS.BIN",
                                          "Firm file"};

static void draw_wipe_top(const WipeView *v) {
  volatile u8 *fb = VRAM_TOP_LA;
  const int W = TOP_SCREEN_WIDTH, H = TOP_SCREEN_HEIGHT;
  const char *title = "Wiping Aurora";
  const char *sub = "Keep the console on and the SD card in.";
  Color tint = COLOR_AMBER;

  if (v->ok == 1) {
    title = "Aurora was removed";
    sub = v->st->firm_missing ? "Delete the firm file from the SD card yourself."
                              : "The console will restart.";
    tint = COLOR_OK;
  } else if (v->ok == 0) {
    title = "The wipe stopped";
    sub = "Some files could not be deleted.";
    tint = COLOR_WARN;
  }
  bm_bg(fb, W, H);
  status_bar(fb);
  draw_filled_round_rect(fb, 40, 60, W - 80, 120, 14, H, COLOR_HM_CARD);
  bm_icon(fb, (W - 48) / 2, 76, 48, H,
          v->ok == 1 ? ASSET_ICON_CHECK_48
                     : v->ok == 0 ? ASSET_ICON_CROSS_48 : ASSET_ICON_REPORT_48,
          icon_power_bits, tint);
  bm_text_mid(fb, W / 2, 130, H, title, COLOR_WHITE, COLOR_HM_CARD, &bm_title);
  bm_text_mid(fb, W / 2, 158, H, sub, COLOR_HM_TEXT2, COLOR_HM_CARD,
              &bm_small);
}

static void u32_to_str(u32 v, char *out) {
  char tmp[11];
  int i = 0, p = 0;
  do {
    tmp[i++] = (char)('0' + v % 10u);
    v /= 10u;
  } while (v);
  while (i)
    out[p++] = tmp[--i];
  out[p] = 0;
}

static void draw_wipe_bottom(const WipeView *v) {
  volatile u8 *fb = VRAM_BOT_A;
  const int W = BOT_SCREEN_WIDTH, H = BOT_SCREEN_HEIGHT;
  const WipeStatus *st = v->st;
  u32 now = fx_ms();
  char line[80], fit[80];

  bm_bg(fb, W, H);
  top_bar(fb, 8, v->ok == 0 ? "Wipe Failed" : "Wipe Aurora",
          v->ok == 0 ? COLOR_WARN : COLOR_WHITE);
  draw_filled_round_rect(fb, 14, 48, W - 28, 140, 12, H, COLOR_HM_CARD);

  for (int i = 0; i < 3; i++) {
    int y = 58 + i * 28, cx = 38, cy = y + 9;
    int done = st->step > i || (i == WIPE_FIRM && v->ok == 1);
    int failed = v->ok == 0 && st->step == i;
    Color label = COLOR_HM_TEXT2;

    if (done) {
      if (!(bm_assets_ok && draw_asset_boxed(fb, cx - 10, cy - 10, 20, 20, H,
                                             ASSET_ICON_CHECK_16, COLOR_OK)))
        draw_filled_round_rect(fb, cx - 6, cy - 6, 12, 12, 6, H, COLOR_OK);
      label = COLOR_WHITE;
    } else if (failed) {
      if (!(bm_assets_ok && draw_asset_boxed(fb, cx - 10, cy - 10, 20, 20, H,
                                             ASSET_ICON_CROSS_16, COLOR_WARN)))
        draw_filled_round_rect(fb, cx - 6, cy - 6, 12, 12, 6, H, COLOR_WARN);
      label = COLOR_WARN;
    } else if (st->step == i && v->ok < 0) {
      spinner(fb, cx, cy, 7, H, COLOR_AURORA, COLOR_HM_CARD, now);
      label = COLOR_WHITE;
    } else {
      draw_round_ring(fb, cx - 6, cy - 6, 12, 12, 6, 2, H, COLOR_HM_TEXT2);
    }
    bm_text(fb, 58, y + 2, H, step_names[i], label, COLOR_HM_CARD, &bm_font);
  }

  if (v->ok == 0) {
    char *p = line;
    const char *s = "Couldn't delete it (FatFs error ";
    while (*s)
      *p++ = *s++;
    u32_to_str((u32)st->fr, p);
    while (*p)
      p++;
    *p++ = ')';
    *p = 0;
    bm_text(fb, 26, 146, H, line, COLOR_WARN, COLOR_HM_CARD, &bm_small);
    bm_fit_left(fit, sizeof(fit), &bm_small, st->where, W - 52);
  } else {
    u32_to_str(st->deleted, line);
    {
      char *p = line;
      while (*p)
        p++;
      const char *s = st->deleted == 1 ? " item deleted" : " items deleted";
      while ((*p++ = *s++))
        ;
    }
    bm_text(fb, 26, 146, H, line, COLOR_HM_TEXT2, COLOR_HM_CARD, &bm_small);
    bm_fit_left(fit, sizeof(fit), &bm_small, v->ok < 0 ? st->current : "",
                W - 52);
  }
  bm_text(fb, 26, 164, H, fit, COLOR_HM_TEXT2, COLOR_HM_CARD, &bm_small);

  hint_pill(fb, H - 36,
            v->ok < 0                 ? "Don't turn off the console"
            : v->ok == 0              ? "B: Back"
            : v->st->firm_missing     ? "A: Restart"
                                      : "Restarting...");
}

static void draw_wipe(void *ctx) {
  draw_wipe_top((const WipeView *)ctx);
  draw_wipe_bottom((const WipeView *)ctx);
}

/* Called after every item deleted: a frame at most every 40 ms, so the
 * screen keeps up without slowing the card down. */
static void wipe_tick(const WipeStatus *st, void *ctx) {
  WipeView *v = (WipeView *)ctx;
  u32 now = fx_ms();
  (void)st;
  if (now - v->drawn < 40u)
    return;
  v->drawn = now;
  draw_wipe_bottom(v);
  fx_present(FX_BOT);
}

/* Returns only if the wipe stopped at an error and B was pressed. */
static void run_wipe(void) {
  static WipeStatus st;
  WipeView v = {&st, -1, 0};

  st.step = WIPE_FOLDER;
  st.deleted = 0;
  st.fr = 0;
  st.firm_missing = 0;
  st.current = "";
  st.where[0] = 0;
  draw_wipe(&v);
  fx_present(FX_BOTH);
  v.drawn = fx_ms();

  v.ok = wipe_run(&st, wipe_tick, &v);
  draw_wipe(&v);
  fx_present(FX_BOTH);

  if (!v.ok) {
    while (!(get_keys_down() & BUTTON_B))
      delay(60000);
    return;
  }
  if (st.firm_missing) {
    while (!(get_keys_down() & BUTTON_A))
      delay(60000);
  } else {
    u32 t0 = fx_ms();
    while (fx_ms() - t0 < 1400u)
      delay(60000);
  }
  fx_fade_out(FX_BOTH, 450);
  power_reboot();
}

/* 1 when the wipe should start. The hold only counts once all three keys
 * were up since the screen opened, and drains back three times as fast as it
 * fills. */
static int confirm_wipe(ConfirmView *v) {
  int armed = (get_keys() & WIPE_KEYS) != WIPE_KEYS;
  u32 last = fx_ms();

  for (;;) {
    u32 now = fx_ms(), dt = now - last;
    u32 down = get_keys_down(), held = get_keys();
    last = now;

    if (down & BUTTON_B)
      return 0;
    if ((held & WIPE_KEYS) != WIPE_KEYS)
      armed = 1;
    if (armed && (held & WIPE_KEYS) == WIPE_KEYS)
      v->hold += (int)dt;
    else
      v->hold -= (int)dt * 3;
    if (v->hold < 0)
      v->hold = 0;
    if (v->hold >= T_HOLD) {
      v->hold = T_HOLD;
      draw_confirm_bottom(v);
      fx_present(FX_BOT);
      return 1;
    }
    for (int i = 0; i < 3; i++) {
      static const u32 key[3] = {BUTTON_L, BUTTON_R, BUTTON_Y};
      int step = (int)dt * 256 / 90, goal = (held & key[i]) ? 256 : 0;
      if (v->cap[i] < goal)
        v->cap[i] = v->cap[i] + step > goal ? goal : v->cap[i] + step;
      else if (v->cap[i] > goal)
        v->cap[i] = v->cap[i] - step < goal ? goal : v->cap[i] - step;
    }
    draw_confirm_bottom(v);
    fx_present(FX_BOT);
  }
}

/* Settings until B; the wipe never comes back unless it failed. */
static void run_settings(void) {
  int glow = 0;

  for (;;) {
    u32 down = get_keys_down();
    if (down & BUTTON_B)
      return;
    if (down & BUTTON_A) {
      ConfirmView cv = {0, {0, 0, 0}};
      fx_push(FX_BOTH, 1, draw_confirm, &cv, T_PUSH);
      if (confirm_wipe(&cv)) {
        run_wipe();
        glow = 0;
        fx_push(FX_BOTH, -1, draw_settings, &glow, T_PUSH);
      } else {
        fx_push(FX_BOTH, -1, draw_settings, &glow, T_PUSH);
      }
      continue;
    }
    glow = pulse(fx_ms());
    draw_settings_bottom(glow);
    fx_present(FX_BOT);
  }
}

static void draw_power_card(void *ctx) {
  volatile u8 *fb = VRAM_BOT_A;
  const int W = BOT_SCREEN_WIDTH, H = BOT_SCREEN_HEIGHT;
  (void)ctx;

  bm_bg(fb, W, H);
  draw_filled_round_rect(fb, 36, 56, W - 72, 128, 14, H, COLOR_HM_CARD);
  bm_icon(fb, (W - 40) / 2, 74, 40, H, ASSET_ICON_POWER_32, icon_power_bits,
          COLOR_WARN);
  bm_text_mid(fb, W / 2, 132, H, "Powering off...", COLOR_WHITE, COLOR_HM_CARD,
              &bm_bold);
}

static void menu_view(MenuView *v, int sel, int pos_from, u32 move_t0,
                      int prev, int dir, u32 slide_t0, u32 intro_t0, u32 now) {
  int intro = (int)(now - intro_t0);
  v->sel = sel;
  v->pos = fx_lerp(pos_from, sel * 256,
                   fx_ease_out(fx_progress(move_t0, now, T_MOVE)));
  v->intro = intro > T_INTRO ? T_INTRO : intro;
  v->prev = prev;
  v->slide = fx_progress(slide_t0, now, T_SLIDE);
  v->dir = dir;
  v->press = 0;
  v->glow = pulse(now);
}

/* The chosen tile pushed in and let back out. */
static void press_tile(MenuView *v) {
  u32 t0 = fx_ms();
  int t = 0;
  while (t < 256) {
    t = fx_progress(t0, fx_ms(), T_PRESS);
    v->press = fx_ease_in_out(t < 128 ? t * 2 : (256 - t) * 2);
    draw_menu_bottom(v);
    fx_present(FX_BOT);
  }
  v->press = 0;
}

static void run_menu(void) {
  MenuView v;
  int sel = 0, pos_from = 0, prev = -1, dir = 1, moving, top_settled = 0;
  u32 now = fx_ms(), intro_t0 = now, move_t0 = now - T_MOVE,
      slide_t0 = now - T_SLIDE;

  for (;;) {
    u32 down = get_keys_down();
    now = fx_ms();
    menu_view(&v, sel, pos_from, move_t0, prev, dir, slide_t0, intro_t0, now);

    if (((down & BUTTON_DLEFT) && sel > 0) ||
        ((down & BUTTON_DRIGHT) && sel < 2)) {
      dir = (down & BUTTON_DLEFT) ? -1 : 1;
      pos_from = v.pos;
      move_t0 = slide_t0 = now;
      prev = sel;
      sel += dir;
      menu_view(&v, sel, pos_from, move_t0, prev, dir, slide_t0, intro_t0, now);
    }

    if (down & BUTTON_A) {
      MenuView still;
      press_tile(&v);
      if (sel == 0) {
        fx_push(FX_BOT, 1, loader_draw_start, 0, T_PUSH);
        boot_aurora();
      } else if (sel == 1) {
        fx_push(FX_BOT, 1, draw_power_card, 0, T_PUSH);
        delay(60000 * 20);
        fx_fade_out(FX_BOTH, 350);
        power_off();
      } else {
        int glow = 0;
        fx_push(FX_BOTH, 1, draw_settings, &glow, T_PUSH);
        run_settings();
      }
      /* Back from a boot error or Settings: the menu as it was, pushed in. */
      now = fx_ms();
      menu_view(&still, sel, sel * 256, now - T_MOVE, -1, dir, now - T_SLIDE,
                now - T_INTRO, now);
      fx_push(FX_BOTH, -1, draw_menu, &still, T_PUSH);
      pos_from = sel * 256;
      move_t0 = now - T_MOVE;
      prev = -1;
      continue;
    }

    /* The top screen only moves during the intro and a slide, and gets one
     * frame more once it has settled; the bottom keeps the pulse going. */
    moving = v.intro < T_INTRO || v.slide < 256;
    draw_menu_bottom(&v);
    if (moving || !top_settled) {
      draw_menu_top(&v);
      fx_present(FX_BOTH);
      top_settled = !moving;
    } else {
      fx_present(FX_BOT);
    }
  }
}

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
  volatile u8 *fb = VRAM_BOT_A;
  const int W = BOT_SCREEN_WIDTH, H = BOT_SCREEN_HEIGHT;

  bm_bg(fb, W, H);
  draw_filled_round_rect(fb, 8, 8, W - 16, 30, 10, H, COLOR_HM_BAR);
  bm_text(fb, 20, 8 + (30 - bm_th(&bm_bold)) / 2, H, "SD Card Test",
          COLOR_AURORA, COLOR_HM_BAR, &bm_bold);
  draw_filled_round_rect(fb, 12, 46, W - 24, H - 90, 12, H, COLOR_HM_CARD);
  screen_present_bottom();

  char line[48];
  int y = 56;

  int init_res = sdmmc_sdcard_init();
  char *p = str_cpy_ret(line, "Init: ");
  int_to_str(init_res, p);
  bm_text(fb, 22, y, H, line, init_res == 0 ? COLOR_AURORA : COLOR_RED,
          COLOR_HM_CARD, &bm_small);
  y += 18;

  if (init_res != 0) {
    bm_text(fb, 22, y, H, "SD init failed", COLOR_RED, COLOR_HM_CARD, &bm_small);
  } else {
    int read_res = sdmmc_sdcard_readsectors(0, 1, sd_test_buf);
    p = str_cpy_ret(line, "Read: ");
    int_to_str(read_res, p);
    bm_text(fb, 22, y, H, line, read_res == 0 ? COLOR_AURORA : COLOR_RED,
            COLOR_HM_CARD, &bm_small);
    y += 18;

    bm_text(fb, 22, y, H, "Sector 0, bytes 0-15:", COLOR_HM_TEXT2,
            COLOR_HM_CARD, &bm_small);
    y += 16;
    build_hex_line(&sd_test_buf[0], 8, line);
    draw_string(fb, 22, y, H, line, COLOR_WHITE, COLOR_HM_CARD);
    y += 12;
    build_hex_line(&sd_test_buf[8], 8, line);
    draw_string(fb, 22, y, H, line, COLOR_WHITE, COLOR_HM_CARD);
    y += 18;

    p = str_cpy_ret(line, "Signature: ");
    *p++ = hex_nibble((u8)(sd_test_buf[510] >> 4));
    *p++ = hex_nibble((u8)(sd_test_buf[510] & 0xF));
    *p++ = hex_nibble((u8)(sd_test_buf[511] >> 4));
    *p++ = hex_nibble((u8)(sd_test_buf[511] & 0xF));
    *p = '\0';
    bm_text(fb, 22, y, H, line, COLOR_WHITE, COLOR_HM_CARD, &bm_small);
    y += 18;

    int pass = (sd_test_buf[510] == 0x55 && sd_test_buf[511] == 0xAA);
    bm_text(fb, 22, y, H,
            pass ? "PASS: valid boot sector" : "No 0x55AA signature",
            pass ? COLOR_AURORA : COLOR_YELLOW, COLOR_HM_CARD, &bm_small);
  }

  draw_filled_round_rect(fb, 12, H - 36, W - 24, 26, 9, H, COLOR_HM_BAR);
  bm_text_mid(fb, W / 2, H - 36 + (26 - bm_th(&bm_small)) / 2, H, "B: Back",
              COLOR_HM_TEXT2, COLOR_HM_BAR, &bm_small);
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
  volatile u8 *fb = VRAM_BOT_A;
  const int W = BOT_SCREEN_WIDTH, H = BOT_SCREEN_HEIGHT;

  bm_bg(fb, W, H);
  draw_filled_round_rect(fb, 8, 8, W - 16, 30, 10, H, COLOR_HM_BAR);
  bm_text(fb, 20, 8 + (30 - bm_th(&bm_bold)) / 2, H, "FatFs Test",
          COLOR_AURORA, COLOR_HM_BAR, &bm_bold);
  draw_filled_round_rect(fb, 12, 46, W - 24, H - 90, 12, H, COLOR_HM_CARD);
  screen_present_bottom();

  char line[48];
  int y = 56;

  FRESULT fr = f_mount(&fs_volume, "", 1);
  char *p = str_cpy_ret(line, "Mount: ");
  int_to_str((int)fr, p);
  bm_text(fb, 22, y, H, line, fr == FR_OK ? COLOR_AURORA : COLOR_RED,
          COLOR_HM_CARD, &bm_small);
  y += 18;

  if (fr == FR_OK) {
    fr = f_open(&fs_file, "test.txt", FA_READ);
    p = str_cpy_ret(line, "Open test.txt: ");
    int_to_str((int)fr, p);
    bm_text(fb, 22, y, H, line, fr == FR_OK ? COLOR_AURORA : COLOR_RED,
            COLOR_HM_CARD, &bm_small);
    y += 18;

    if (fr == FR_OK) {
      char data[33];
      UINT br = 0;
      fr = f_read(&fs_file, data, sizeof(data) - 1, &br);
      f_close(&fs_file);

      p = str_cpy_ret(line, "Read: ");
      p = str_cpy_ret(p, (fr == FR_OK) ? "OK  bytes=" : "ERR bytes=");
      int_to_str((int)br, p);
      bm_text(fb, 22, y, H, line, fr == FR_OK ? COLOR_AURORA : COLOR_RED,
              COLOR_HM_CARD, &bm_small);
      y += 18;

      for (UINT i = 0; i < br; i++) {
        if (data[i] < 0x20 || data[i] > 0x7E)
          data[i] = '.';
      }
      data[br] = '\0';
      bm_text(fb, 22, y, H, "Contents:", COLOR_HM_TEXT2, COLOR_HM_CARD,
              &bm_small);
      y += 16;
      draw_string(fb, 22, y, H, data, COLOR_WHITE, COLOR_HM_CARD);
      y += 18;

      if (fr == FR_OK && br > 0)
        bm_text(fb, 22, y, H, "PASS: file read OK", COLOR_AURORA, COLOR_HM_CARD,
                &bm_small);
    } else {
      bm_text(fb, 22, y, H, "Put test.txt on SD root", COLOR_YELLOW,
              COLOR_HM_CARD, &bm_small);
    }
  }

  f_mount(NULL, "", 0);

  draw_filled_round_rect(fb, 12, H - 36, W - 24, 26, 9, H, COLOR_HM_BAR);
  bm_text_mid(fb, W / 2, H - 36 + (26 - bm_th(&bm_small)) / 2, H, "B: Back",
              COLOR_HM_TEXT2, COLOR_HM_BAR, &bm_small);
  screen_present_bottom();

  while (1) {
    u32 kdown = get_keys_down();
    if (kdown & BUTTON_B)
      return;
    delay(60000);
  }
}


static int read_autoboot_flag(void) {
  static FATFS ab_fs;
  static FIL ab_f;
  u8 buf[USER_DAT_SIZE];
  UINT br = 0;
  int on = 0;
  for (int attempt = 0; attempt < 4; attempt++) {
    if (f_mount(&ab_fs, "", 1) == FR_OK) {
      if (f_open(&ab_f, USER_DAT_PATH, FA_READ) == FR_OK) {
        if (f_read(&ab_f, buf, sizeof(buf), &br) == FR_OK &&
            br > USER_DAT_AUTOBOOT &&
            buf[0] == 'A' && buf[1] == 'D' && buf[2] == 'A' && buf[3] == 'T') {
          on = (buf[USER_DAT_AUTOBOOT] & 1u) != 0;
        }
        f_close(&ab_f);
      }
      f_mount(NULL, "", 0);
      return on;
    }
    f_mount(NULL, "", 0);
    delay(60000);
  }
  return 0;
}

static void draw_autoboot_logo(void) {
  clear_screen(VRAM_TOP_LA, TOP_FB_SIZE, COLOR_BLACK);
  int lx = (TOP_SCREEN_WIDTH - AURORA_LOGO_WIDTH) / 2;
  int ly = (TOP_SCREEN_HEIGHT - AURORA_LOGO_HEIGHT) / 2;
  draw_aurora_logo(VRAM_TOP_LA, lx, ly, TOP_SCREEN_HEIGHT, COLOR_WHITE);
  clear_screen(VRAM_BOT_A, BOT_FB_SIZE, COLOR_BLACK);
  fx_present(FX_BOTH);
}

int main(void) {
  /* Held SELECT keeps the caches off, as it does in the OS. */
  int plain = (get_keys() & BUTTON_SELECT) != 0;

  screens_ready();
  fx_init(plain);
  clear_screen(VRAM_TOP_LA, TOP_FB_SIZE, COLOR_BLACK);
  clear_screen(VRAM_BOT_A, BOT_FB_SIZE, COLOR_BLACK);

  /* Autoboot: show only the Aurora logo and launch AURORAOS.BIN directly.
   * Holding A across a short window (~350 ms) reroutes to the bootloader menu,
   * while a brief A-tap from a chainloader (e.g. Luma3DS) still autoboot-loads. */
  if (read_autoboot_flag()) {
    draw_autoboot_logo();
    int hold_a = 1;
    for (int i = 0; i < 40; i++) {
      delay(60000);
      if (!(get_keys() & BUTTON_A)) {
        hold_a = 0;
        break;
      }
    }
    if (!hold_a) {
      boot_aurora_ex(1);
      /* Only reaches here if autoboot hit an error and user pressed B. */
    }
  }

  /* Seed prev_keys so holding A to enter the menu doesn't immediately launch. */
  prev_keys = get_keys();
  bm_assets_init();
  run_menu();
  return 0;
}
