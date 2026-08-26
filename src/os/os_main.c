/* Coded By DisLoPik for the AuroraOS Project. */
/*
 * AuroraOS -- the operating system payload (AURORAOS.BIN).
 *
 * This is the real OS the launcher firm boots into: the two-screen 3DS-style
 * Home Menu (see the mockup PNGs). It reuses the shared drawing code in
 * src/screen.c and the I2C driver in src/i2c.c (for power off), and runs
 * standalone at 0x22000000.
 */
#include "aurora.h"
#include "font.h"
#include "i2c.h"
#include "icons.h"

/* ---- small helpers (this payload does not link the firm's main.c) ---- */
void delay(volatile u32 cycles) {
  while (cycles--)
    __asm__ volatile("nop");
}

u32 get_keys(void) { return ~REG_HID_PAD & 0x3FF; }

static u32 prev_keys = 0;
u32 get_keys_down(void) {
  u32 cur = get_keys();
  u32 down = cur & ~prev_keys;
  prev_keys = cur;
  return down;
}

static u32 str_len(const char *s) {
  u32 n = 0;
  while (*s++)
    n++;
  return n;
}

/* Power the console off via the MCU (same sequence as the firm, learned from
   GodMode9). Included so the OS is not a dead end. */
static void os_power_off(void) {
  I2C_init();
  I2C_writeReg(I2C_DEV_MCU, 0x22, 1 << 0); /* LCDs off first */
  __asm__ volatile("mcr p15, 0, %0, c7, c10, 4" ::"r"(0) : "memory");
  I2C_writeReg(I2C_DEV_MCU, 0x20, 1 << 0); /* system power off */
  while (1)
    __asm__ volatile("mcr p15, 0, r0, c7, c0, 4");
}

/* ============================ Home Menu ================================== */

typedef enum { ACT_NONE = 0, ACT_POWER } HomeAction;

typedef struct {
  const char *name; /* NULL => empty placeholder slot */
  const char *dev;
  const unsigned char *icon; /* 32x32 icon bits, or NULL */
  Color tint;                /* big-icon background tint */
  HomeAction action;
} HomeApp;

#define HOME_COLS  5
#define HOME_ROWS  3
#define HOME_COUNT (HOME_COLS * HOME_ROWS)

static const HomeApp home_apps[HOME_COUNT] = {
    {"Power Off", "System", icon_power_bits, COLOR_DARK_RED, ACT_POWER},
    /* the rest are zero-initialised -> empty placeholder slots */
};

/* ---- top-screen status-bar glyphs ---- */
static void hm_wifi(int x, int y) {
  for (int b = 0; b < 3; b++) {
    int bh = 3 + b * 3;
    draw_filled_rect(VRAM_TOP_LA, x + b * 5, y + (9 - bh), 3, bh,
                     TOP_SCREEN_HEIGHT, COLOR_WHITE);
  }
}
static void hm_grid_icon(int x, int y) {
  for (int r = 0; r < 2; r++)
    for (int c = 0; c < 2; c++)
      draw_filled_rect(VRAM_TOP_LA, x + c * 5, y + r * 5, 3, 3, TOP_SCREEN_HEIGHT,
                       COLOR_WHITE);
}
static void hm_battery(int x, int y) {
  draw_filled_rect(VRAM_TOP_LA, x, y, 15, 9, TOP_SCREEN_HEIGHT, COLOR_WHITE);
  draw_filled_rect(VRAM_TOP_LA, x + 1, y + 1, 13, 7, TOP_SCREEN_HEIGHT,
                   COLOR_HM_BAR);
  draw_filled_rect(VRAM_TOP_LA, x + 2, y + 2, 8, 5, TOP_SCREEN_HEIGHT,
                   COLOR_AURORA);
  draw_filled_rect(VRAM_TOP_LA, x + 15, y + 3, 2, 3, TOP_SCREEN_HEIGHT,
                   COLOR_WHITE);
}

static void hm_draw_top(int selected) {
  const HomeApp *app = &home_apps[selected];

  clear_screen(VRAM_TOP_LA, TOP_FB_SIZE, COLOR_HM_BG);

  /* faint faceted diamond texture */
  for (int o = -TOP_SCREEN_HEIGHT; o < TOP_SCREEN_WIDTH; o += 46) {
    for (int t = 0; t < TOP_SCREEN_HEIGHT; t++) {
      int a = o + t;
      int b = o + (TOP_SCREEN_HEIGHT - t);
      if (a >= 0 && a < TOP_SCREEN_WIDTH)
        draw_pixel(VRAM_TOP_LA, a, t, TOP_SCREEN_HEIGHT, COLOR_HM_FACET);
      if (b >= 0 && b < TOP_SCREEN_WIDTH)
        draw_pixel(VRAM_TOP_LA, b, t, TOP_SCREEN_HEIGHT, COLOR_HM_FACET);
    }
  }

  /* status bar */
  draw_filled_rect(VRAM_TOP_LA, 0, 0, TOP_SCREEN_WIDTH, 22, TOP_SCREEN_HEIGHT,
                   COLOR_HM_BAR);
  draw_string(VRAM_TOP_LA, 10, 7, TOP_SCREEN_HEIGHT, "12:08", COLOR_WHITE,
              COLOR_HM_BAR);
  draw_string(VRAM_TOP_LA, 60, 7, TOP_SCREEN_HEIGHT, "Tue 25 Aug",
              COLOR_HM_TEXT2, COLOR_HM_BAR);
  hm_wifi(322, 7);
  draw_string(VRAM_TOP_LA, 340, 7, TOP_SCREEN_HEIGHT, "|", COLOR_HM_TEXT2,
              COLOR_HM_BAR);
  hm_grid_icon(352, 7);
  hm_battery(371, 7);

  /* big selected-item icon */
  int box = 96, bx = (TOP_SCREEN_WIDTH - box) / 2, by = 34;
  draw_filled_round_rect(VRAM_TOP_LA, bx, by, box, box, 16, TOP_SCREEN_HEIGHT,
                         app->name ? app->tint : COLOR_HM_SLOT_EMPTY);
  if (app->icon)
    draw_icon_32(VRAM_TOP_LA, bx + (box - ICON_SIZE) / 2,
                 by + (box - ICON_SIZE) / 2, TOP_SCREEN_HEIGHT, app->icon,
                 COLOR_WHITE);

  /* info card */
  int cy = 150, cw = TOP_SCREEN_WIDTH - 80, ch = 58;
  draw_filled_round_rect(VRAM_TOP_LA, 40, cy, cw, ch, 12, TOP_SCREEN_HEIGHT,
                         COLOR_HM_CARD);
  const char *name = app->name ? app->name : "Empty Slot";
  const char *dev = app->dev ? app->dev : "";
  draw_string(VRAM_TOP_LA,
              (TOP_SCREEN_WIDTH - (int)str_len(name) * FONT_WIDTH) / 2, cy + 16,
              TOP_SCREEN_HEIGHT, name, COLOR_WHITE, COLOR_HM_CARD);
  draw_string(VRAM_TOP_LA,
              (TOP_SCREEN_WIDTH - (int)str_len(dev) * FONT_WIDTH) / 2, cy + 34,
              TOP_SCREEN_HEIGHT, dev, COLOR_HM_TEXT2, COLOR_HM_CARD);
  screen_present_top();
}

#define SLOT_SIZE 46
#define SLOT_GAP  12
#define SLOT_STEP (SLOT_SIZE + SLOT_GAP)
#define GRID_W    (HOME_COLS * SLOT_SIZE + (HOME_COLS - 1) * SLOT_GAP)
#define GRID_X    ((BOT_SCREEN_WIDTH - GRID_W) / 2)
#define GRID_Y    52

static void hm_draw_bottom(int selected) {
  clear_screen(VRAM_BOT_A, BOT_FB_SIZE, COLOR_HM_BG);

  /* top bar with search + settings glyphs (right aligned) */
  draw_filled_round_rect(VRAM_BOT_A, 8, 8, BOT_SCREEN_WIDTH - 16, 30, 10,
                         BOT_SCREEN_HEIGHT, COLOR_HM_BAR);
  int sx = BOT_SCREEN_WIDTH - 62, sy = 16; /* search magnifier */
  draw_filled_round_rect(VRAM_BOT_A, sx, sy, 11, 11, 5, BOT_SCREEN_HEIGHT,
                         COLOR_HM_TEXT2);
  draw_filled_round_rect(VRAM_BOT_A, sx + 2, sy + 2, 7, 7, 3, BOT_SCREEN_HEIGHT,
                         COLOR_HM_BAR);
  draw_filled_rect(VRAM_BOT_A, sx + 10, sy + 10, 4, 2, BOT_SCREEN_HEIGHT,
                   COLOR_HM_TEXT2);
  int gx = BOT_SCREEN_WIDTH - 34, gy = 18; /* settings sliders */
  draw_filled_round_rect(VRAM_BOT_A, gx, gy, 16, 4, 2, BOT_SCREEN_HEIGHT,
                         COLOR_HM_TEXT2);
  draw_filled_round_rect(VRAM_BOT_A, gx + 9, gy - 2, 5, 8, 2, BOT_SCREEN_HEIGHT,
                         COLOR_WHITE);
  draw_filled_round_rect(VRAM_BOT_A, gx, gy + 8, 16, 4, 2, BOT_SCREEN_HEIGHT,
                         COLOR_HM_TEXT2);
  draw_filled_round_rect(VRAM_BOT_A, gx + 2, gy + 6, 5, 8, 2, BOT_SCREEN_HEIGHT,
                         COLOR_WHITE);

  /* app grid */
  for (int i = 0; i < HOME_COUNT; i++) {
    int col = i % HOME_COLS, row = i / HOME_COLS;
    int x = GRID_X + col * SLOT_STEP, y = GRID_Y + row * SLOT_STEP;
    const HomeApp *app = &home_apps[i];

    if (i == selected)
      draw_filled_round_rect(VRAM_BOT_A, x - 3, y - 3, SLOT_SIZE + 6,
                             SLOT_SIZE + 6, 12, BOT_SCREEN_HEIGHT, COLOR_AURORA);
    draw_filled_round_rect(VRAM_BOT_A, x, y, SLOT_SIZE, SLOT_SIZE, 10,
                           BOT_SCREEN_HEIGHT,
                           app->name ? COLOR_HM_SLOT : COLOR_HM_SLOT_EMPTY);
    if (app->icon)
      draw_icon_32(VRAM_BOT_A, x + (SLOT_SIZE - ICON_SIZE) / 2,
                   y + (SLOT_SIZE - ICON_SIZE) / 2, BOT_SCREEN_HEIGHT, app->icon,
                   COLOR_WHITE);
  }
  screen_present_bottom();
}

static void hm_redraw(int sel) {
  hm_draw_top(sel);
  hm_draw_bottom(sel);
}

void os_main(void) {
  int sel = 0;
  hm_redraw(sel);

  while (1) {
    u32 kdown = get_keys_down();
    int prev = sel;
    int col = sel % HOME_COLS, row = sel / HOME_COLS;

    if ((kdown & BUTTON_DLEFT) && col > 0)
      sel--;
    if ((kdown & BUTTON_DRIGHT) && col < HOME_COLS - 1 && sel + 1 < HOME_COUNT)
      sel++;
    if ((kdown & BUTTON_DUP) && row > 0)
      sel -= HOME_COLS;
    if ((kdown & BUTTON_DDOWN) && row < HOME_ROWS - 1 &&
        sel + HOME_COLS < HOME_COUNT)
      sel += HOME_COLS;

    if (sel != prev)
      hm_redraw(sel);

    if (kdown & BUTTON_A) {
      if (home_apps[sel].action == ACT_POWER)
        os_power_off();
    }

    delay(60000);
  }
}
