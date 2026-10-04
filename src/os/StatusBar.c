/* Without the asset pack it falls back to the 8x8 font and drawn indicators. */
#include "statusbar.h"
#include "ui.h"
#include "power.h"
#include "model.h"
#include "wifi.h"

#define SH   TOP_SCREEN_HEIGHT
#define BAR  STATUS_BAR_HEIGHT
#define EDGE 10

static const Color charge_green = {0x4C, 0xD9, 0x64};

/* Battery pill: 20x11 with a 2x5 nub, 16px of fill. pct < 0 means the MCU did
 * not answer, which leaves the pill empty rather than guessing. */
static void battery(int x, int y, int pct, int charging, Color bg, Color fill) {
  volatile u8 *fb = VRAM_TOP_LA;
  int w;

  draw_filled_round_rect(fb, x, y, 20, 11, 3, SH, COLOR_WHITE);
  draw_filled_round_rect(fb, x + 1, y + 1, 18, 9, 2, SH, bg);
  draw_filled_round_rect(fb, x + 21, y + 3, 2, 5, 1, SH, COLOR_WHITE);
  if (pct < 0)
    return;
  w = pct * 16 / 100;
  if (w < 2 && pct > 0)
    w = 2; /* never an empty cell for a battery that still has charge */
  if (w > 16)
    w = 16;
  if (w > 0)
    draw_filled_round_rect(fb, x + 2, y + 2, w, 7, 1, SH,
                           charging > 0 ? charge_green
                           : (pct <= 15 ? COLOR_DARK_RED : fill));
}

/* "100%", or an empty string when the MCU did not answer. */
static void pct_text(char *out, int pct) {
  int n = 0;
  if (pct >= 0) {
    if (pct >= 100) {
      out[n++] = '1'; out[n++] = '0'; out[n++] = '0';
    } else if (pct >= 10) {
      out[n++] = (char)('0' + pct / 10);
      out[n++] = (char)('0' + pct % 10);
    } else {
      out[n++] = (char)('0' + pct);
    }
    out[n++] = '%';
  }
  out[n] = '\0';
}

static void clock_text(char *tbuf, char *dbuf) {
  RtcTime now;
  if (rtc_read(&now)) {
    rtc_format_time(&now, tbuf);
    if (dbuf)
      rtc_format_date(&now, dbuf);
  } else {
    tbuf[0] = '-'; tbuf[1] = '-'; tbuf[2] = ':';
    tbuf[3] = '-'; tbuf[4] = '-'; tbuf[5] = '\0';
    if (dbuf)
      dbuf[0] = '\0';
  }
}

/* Fallback indicators, for a card without the asset pack. */
static void wifi_bars(int x, int y, Color c) {
  for (int b = 0; b < 3; b++) {
    int bh = 3 + b * 3;
    draw_filled_rect(VRAM_TOP_LA, x + b * 5, y + (9 - bh), 3, bh, SH, c);
  }
}

static void grid(int x, int y) {
  for (int r = 0; r < 2; r++)
    for (int c = 0; c < 2; c++)
      draw_filled_rect(VRAM_TOP_LA, x + c * 5, y + r * 5, 3, 3, SH,
                       COLOR_HM_TEXT2);
}

void status_bar_draw(void) {
  volatile u8 *fb = VRAM_TOP_LA;
  char tbuf[8], dbuf[12], pbuf[6];
  int pct = battery_percent();
  int chg = battery_charging();
  int pack = ui_have(&ui_bold) && ui_have(&ui_small);
  /* Time in bold, the rest small, on one baseline; the 8x8 fallback uses a
   * fixed row. */
  int ty = pack ? (BAR - ui_th(&ui_bold)) / 2 : 7;
  int sy = pack ? ty + text_ascent(&ui_bold) - text_ascent(&ui_small) : 7;
  int iy = (BAR - 16) / 2;
  int x;

  clock_text(tbuf, dbuf);

  draw_filled_rect(fb, 0, 0, TOP_SCREEN_WIDTH, BAR, SH, COLOR_HM_BAR);
  ui_text(fb, EDGE, ty, SH, tbuf, COLOR_WHITE, COLOR_HM_BAR, &ui_bold);
  ui_text(fb, EDGE + ui_tw(&ui_bold, tbuf) + 8, sy, SH, dbuf, COLOR_HM_TEXT2,
          COLOR_HM_BAR, &ui_small);

  /* Right to left: battery, charge, indicators, model tag. */
  x = TOP_SCREEN_WIDTH - EDGE - 23;
  battery(x, (BAR - 11) / 2, pct, chg, COLOR_HM_BAR, g_accent);
  x -= 6;

  pct_text(pbuf, pct);
  if (pbuf[0]) {
    x -= ui_tw(&ui_small, pbuf);
    ui_text(fb, x, sy, SH, pbuf, COLOR_HM_TEXT2, COLOR_HM_BAR, &ui_small);
    x -= 10;
  }

  x -= 16;
  if (pack)
    ui_icon(fb, x, iy, 16, SH, ASSET_ICON_APPS_16, 0, COLOR_HM_TEXT2);
  else
    grid(x + 3, 6);

  x -= 6 + 16;
  Color wc = wifi_online() ? COLOR_WHITE : COLOR_HM_TEXT2; /* joined */
  if (pack)
    ui_icon(fb, x, iy, 16, SH, ASSET_ICON_WIFI_16, 0, wc);
  else
    wifi_bars(x + 1, 6, wc);

  if (aurora_is_new3ds()) {
    x -= 8 + ui_tw(&ui_bold, aurora_model_tag());
    ui_text(fb, x, ty, SH, aurora_model_tag(), COLOR_WHITE, COLOR_HM_BAR,
            &ui_bold);
  }
}

#define APP_BAR  STATUS_APP_HEIGHT
#define APP_EDGE 12
#define APP_GAP  12

/* Mixes `c` toward the bar colour, for what is dimmed on it. */
static Color dim_on(Color c, Color bg) {
  Color o;
  o.r = (u8)((c.r * 5 + bg.r * 3) / 8);
  o.g = (u8)((c.g * 5 + bg.g * 3) / 8);
  o.b = (u8)((c.b * 5 + bg.b * 3) / 8);
  return o;
}

void status_bar_draw_app(const char *title, Color bg, u32 icon) {
  volatile u8 *fb = VRAM_TOP_LA;
  char tbuf[8], pbuf[6];
  int pct = battery_percent();
  int chg = battery_charging();
  int pack = ui_have(&ui_bold) && ui_have(&ui_font);
  int ty = pack ? (APP_BAR - ui_th(&ui_bold)) / 2 : 14;
  int sy = pack ? ty + text_ascent(&ui_bold) - text_ascent(&ui_font) : 14;
  int iy = (APP_BAR - 16) / 2;
  Color soft = dim_on(COLOR_HM_TEXT2, bg);
  int x, left;

  clock_text(tbuf, 0);
  draw_filled_rect(fb, 0, 0, TOP_SCREEN_WIDTH, APP_BAR, SH, bg);
  ui_text(fb, APP_EDGE, ty, SH, tbuf, COLOR_WHITE, bg, &ui_bold);
  left = APP_EDGE + ui_tw(&ui_bold, tbuf) + APP_GAP;

  /* Right to left: the app's icon, Home, a divider, then battery and Wi-Fi. */
  x = TOP_SCREEN_WIDTH - APP_EDGE;
  if (icon != UI_NO_ASSET) {
    x -= 16;
    ui_icon(fb, x, iy, 16, SH, icon, 0, COLOR_WHITE);
    draw_filled_rect(fb, x + 4, iy + 20, 8, 2, SH, COLOR_WHITE);
    x -= APP_GAP + 16;
    if (pack)
      ui_icon(fb, x, iy, 16, SH, ASSET_ICON_APPS_16, 0, soft);
    else
      grid(x + 3, iy + 3);
    x -= APP_GAP + 1;
    draw_filled_rect(fb, x, iy + 2, 1, 12, SH, soft);
    x -= APP_GAP;
  }

  pct_text(pbuf, pct);
  if (pbuf[0]) {
    x -= ui_tw(&ui_font, pbuf);
    ui_text(fb, x, sy, SH, pbuf, COLOR_WHITE, bg, &ui_font);
    x -= 5;
  }
  x -= 23;
  battery(x, (APP_BAR - 11) / 2, pct, chg, bg, COLOR_WHITE);

  x -= 10 + 16;
  Color wc = wifi_online() ? COLOR_WHITE : soft;
  if (pack)
    ui_icon(fb, x, iy, 16, SH, ASSET_ICON_WIFI_16, 0, wc);
  else
    wifi_bars(x + 1, iy + 3, wc);

  if (title && title[0])
    ui_text_fit(fb, left, sy, SH, title, x - 12 - left, soft, bg, &ui_font);
}
