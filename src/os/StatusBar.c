/* Shared by the Home Menu and the setup wizard. Without the asset pack it falls
 * back to the 8x8 font and drawn indicators. */
#include "statusbar.h"
#include "ui.h"
#include "power.h"
#include "model.h"

#define SH   TOP_SCREEN_HEIGHT
#define BAR  STATUS_BAR_HEIGHT
#define EDGE 10

static const Color charge_green = {0x4C, 0xD9, 0x64};

/* Battery pill: 20x11 with a 2x5 nub, 16px of fill. pct < 0 means the MCU did
 * not answer, which leaves the pill empty rather than guessing. */
static void battery(int x, int y, int pct, int charging) {
  volatile u8 *fb = VRAM_TOP_LA;
  int w;

  draw_filled_round_rect(fb, x, y, 20, 11, 3, SH, COLOR_WHITE);
  draw_filled_round_rect(fb, x + 1, y + 1, 18, 9, 2, SH, COLOR_HM_BAR);
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
                           : (pct <= 15 ? COLOR_DARK_RED : g_accent));
}

/* Fallback indicators, for a card without the asset pack. */
static void wifi_bars(int x, int y) {
  for (int b = 0; b < 3; b++) {
    int bh = 3 + b * 3;
    draw_filled_rect(VRAM_TOP_LA, x + b * 5, y + (9 - bh), 3, bh, SH,
                     COLOR_HM_TEXT2);
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
  RtcTime now;
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

  if (rtc_read(&now)) {
    rtc_format_time(&now, tbuf);
    rtc_format_date(&now, dbuf);
  } else {
    tbuf[0] = '-'; tbuf[1] = '-'; tbuf[2] = ':';
    tbuf[3] = '-'; tbuf[4] = '-'; tbuf[5] = '\0';
    dbuf[0] = '\0';
  }

  draw_filled_rect(fb, 0, 0, TOP_SCREEN_WIDTH, BAR, SH, COLOR_HM_BAR);
  ui_text(fb, EDGE, ty, SH, tbuf, COLOR_WHITE, COLOR_HM_BAR, &ui_bold);
  ui_text(fb, EDGE + ui_tw(&ui_bold, tbuf) + 8, sy, SH, dbuf, COLOR_HM_TEXT2,
          COLOR_HM_BAR, &ui_small);

  /* Right to left: battery, charge, indicators, model tag. */
  x = TOP_SCREEN_WIDTH - EDGE - 23;
  battery(x, (BAR - 11) / 2, pct, chg);
  x -= 6;

  if (pct >= 0) {
    int n = 0;
    if (pct >= 100) {
      pbuf[n++] = '1'; pbuf[n++] = '0'; pbuf[n++] = '0';
    } else if (pct >= 10) {
      pbuf[n++] = (char)('0' + pct / 10);
      pbuf[n++] = (char)('0' + pct % 10);
    } else {
      pbuf[n++] = (char)('0' + pct);
    }
    pbuf[n++] = '%';
    pbuf[n] = '\0';
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
  if (pack)
    ui_icon(fb, x, iy, 16, SH, ASSET_ICON_WIFI_16, 0, COLOR_HM_TEXT2);
  else
    wifi_bars(x + 1, 6);

  if (aurora_is_new3ds()) {
    x -= 8 + ui_tw(&ui_bold, aurora_model_tag());
    ui_text(fb, x, ty, SH, aurora_model_tag(), COLOR_WHITE, COLOR_HM_BAR,
            &ui_bold);
  }
}
