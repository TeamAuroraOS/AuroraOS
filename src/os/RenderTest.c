/* Each frame repaints the top screen the way the UI does: wallpaper blit,
 * blended cards, icons and type, async present. The bottom screen updates four
 * times a second. The mean is over one-second samples, each normalised by the
 * time it actually spanned. On a New 3DS the 804 MHz switch is tried first and
 * its outcome reported. */
#include "rendertest.h"
#include "assets.h"
#include "icons.h"
#include "model.h"
#include "timer.h"
#include "touch.h"
#include "ui.h"

#define TEST_SECONDS 60u
#define TOP_BAR      24
#define CARD_W       112
#define CARD_H       56
#define CARD_COUNT   6

typedef struct {
  int x, y, vx, vy;
  u32 asset;
  const char *label;
} Card;

static char *put_str(char *p, const char *s) {
  while (*s)
    *p++ = *s++;
  return p;
}

static char *put_u32(char *p, u32 v) {
  char t[12];
  int n = 0;
  if (!v)
    *p++ = '0';
  while (v) {
    t[n++] = (char)('0' + v % 10u);
    v /= 10u;
  }
  while (n--)
    *p++ = t[n];
  return p;
}

/* Frame rates are carried in tenths, so they print with one decimal. */
static char *put_tenths(char *p, u32 t10) {
  p = put_u32(p, t10 / 10u);
  *p++ = '.';
  *p++ = (char)('0' + t10 % 10u);
  return p;
}

static void move_card(Card *k) {
  k->x += k->vx;
  k->y += k->vy;
  if (k->x < 0) {
    k->x = 0;
    k->vx = -k->vx;
  } else if (k->x > TOP_SCREEN_WIDTH - CARD_W) {
    k->x = TOP_SCREEN_WIDTH - CARD_W;
    k->vx = -k->vx;
  }
  if (k->y < TOP_BAR + 2) {
    k->y = TOP_BAR + 2;
    k->vy = -k->vy;
  } else if (k->y > TOP_SCREEN_HEIGHT - CARD_H) {
    k->y = TOP_SCREEN_HEIGHT - CARD_H;
    k->vy = -k->vy;
  }
}

static void draw_frame(Card *cards, u32 seconds_left, u32 fps10) {
  const int sh = TOP_SCREEN_HEIGHT;
  char buf[40];
  char *p;

  ui_wallpaper(VRAM_TOP_LA, TOP_SCREEN_WIDTH, TOP_SCREEN_HEIGHT, sh);

  for (int i = 0; i < CARD_COUNT; i++) {
    Card *k = &cards[i];
    move_card(k);
    draw_gradient_round_rect(VRAM_TOP_LA, k->x, k->y, CARD_W, CARD_H, 10, sh,
                             COLOR_PANEL_TOP, COLOR_PANEL_BOT);
    ui_icon(VRAM_TOP_LA, k->x + 8, k->y + (CARD_H - 40) / 2, 40, sh, k->asset,
            icon_boot_bits, i == 0 ? g_accent : COLOR_WHITE);
    ui_text(VRAM_TOP_LA, k->x + 54, k->y + (CARD_H - ui_th(&ui_bold)) / 2, sh,
            k->label, COLOR_WHITE, COLOR_PANEL_TOP, &ui_bold);
  }

  draw_filled_rect(VRAM_TOP_LA, 0, 0, TOP_SCREEN_WIDTH, TOP_BAR, sh,
                   COLOR_HM_BAR);
  ui_text(VRAM_TOP_LA, 10, (TOP_BAR - ui_th(&ui_bold)) / 2, sh, "Render test",
          COLOR_WHITE, COLOR_HM_BAR, &ui_bold);
  p = put_tenths(buf, fps10);
  p = put_str(p, " fps   ");
  p = put_u32(p, seconds_left);
  p = put_str(p, " s");
  *p = 0;
  ui_text(VRAM_TOP_LA, TOP_SCREEN_WIDTH - 10 - ui_tw(&ui_font, buf),
          (TOP_BAR - ui_th(&ui_font)) / 2, sh, buf, COLOR_HM_TEXT2,
          COLOR_HM_BAR, &ui_font);

  screen_present_top();
}

static char *put_hex(char *p, u32 v) {
  for (int s = 28; s >= 0; s -= 4)
    *p++ = "0123456789ABCDEF"[(v >> s) & 0xFu];
  return p;
}

static void draw_stats(u32 elapsed_us, u32 frames, u32 fps10, const char *hw) {
  const int sh = BOT_SCREEN_HEIGHT, w = BOT_SCREEN_WIDTH;
  const u32 total_us = TEST_SECONDS * 1000000u;
  const int bar_w = w - 48;
  char buf[40];
  char *p;
  int fill = (elapsed_us >= total_us)
                 ? bar_w
                 : (int)(((unsigned long long)elapsed_us * (u32)bar_w) /
                         total_us);

  ui_wallpaper(VRAM_BOT_A, w, sh, sh);
  draw_gradient_round_rect(VRAM_BOT_A, 16, 16, w - 32, 168, 14, sh,
                           COLOR_PANEL_TOP, COLOR_PANEL_BOT);

  p = put_tenths(buf, fps10);
  p = put_str(p, " fps");
  *p = 0;
  ui_text_mid(VRAM_BOT_A, w / 2, 34, sh, buf, COLOR_WHITE, COLOR_PANEL_TOP,
              &ui_title);

  p = put_u32(buf, frames);
  p = put_str(p, " frames");
  *p = 0;
  ui_text_mid(VRAM_BOT_A, w / 2, 68, sh, buf, COLOR_HM_TEXT2, COLOR_PANEL_TOP,
              &ui_font);

  draw_filled_round_rect(VRAM_BOT_A, 24, 100, bar_w, 12, 6, sh, COLOR_HM_BG);
  if (fill > 12)
    draw_filled_round_rect(VRAM_BOT_A, 24, 100, fill, 12, 6, sh, g_accent);

  ui_text_mid(VRAM_BOT_A, w / 2, 128, sh, hw, COLOR_HM_TEXT2, COLOR_PANEL_BOT,
              &ui_small);
  ui_text_mid(VRAM_BOT_A, w / 2, sh - 26, sh, "B: stop", COLOR_HM_TEXT2,
              COLOR_HM_BG_BOT, &ui_small);
  screen_present_bottom();
}

static void wait_close(void) {
  for (;;) {
    int tx, ty;
    u32 k = get_keys_down();
    if ((k & (BUTTON_A | BUTTON_B)) || touch_tap(&tx, &ty))
      return;
    ui_idle();
  }
}

static void message(const char *title, const char *detail) {
  ui_wallpaper(VRAM_BOT_A, BOT_SCREEN_WIDTH, BOT_SCREEN_HEIGHT,
               BOT_SCREEN_HEIGHT);
  ui_dialog(VRAM_BOT_A, BOT_SCREEN_WIDTH, BOT_SCREEN_HEIGHT, BOT_SCREEN_HEIGHT,
            title, detail, g_accent, 0);
  screen_present_bottom();
}

void render_test_screen(void) {
  static u32 samples[TEST_SECONDS]; /* frames per second, in tenths */
  static const Card start[CARD_COUNT] = {
      {12, 40, 3, 2, ASSET_ICON_SETTINGS_32, "Home"},
      {140, 60, -2, 3, ASSET_ICON_MUSIC_32, "Music"},
      {260, 90, 2, -2, ASSET_ICON_APPS_32, "Apps"},
      {60, 150, -3, -2, ASSET_ICON_WIFI_32, "Wi-Fi"},
      {200, 130, 3, 3, ASSET_ICON_MONITOR_32, "GPU"},
      {280, 170, -2, -3, ASSET_ICON_POWER_32, "Power"},
  };
  Card cards[CARD_COUNT];
  u32 t_all, t_sec, t_stats, us;
  u32 frames_total = 0, frames_sec = 0, count = 0, fps10 = 0;
  u32 sum = 0, lo = 0, hi = 0, mean10 = 0, secs;
  int aborted = 0;
  AuroraN3dsHw hw;
  const char *hw_text;
  u32 clk_before = 0, clk_after = 0;
  char line[48];
  char *p;

  for (int i = 0; i < CARD_COUNT; i++)
    cards[i] = start[i];

  if (!timer_calibrated()) {
    message("Render test", "The timer is not calibrated");
    wait_close();
    return;
  }

  message("Render test", aurora_is_new3ds() ? "Enabling New 3DS hardware"
                                            : "Starting");
  hw = aurora_n3ds_hardware();
  hw_text = aurora_n3ds_hw_text(hw);
  aurora_n3ds_clock(&clk_before, &clk_after);

  t_all = t_sec = t_stats = timer_ticks();
  while (count < TEST_SECONDS) {
    draw_frame(cards, TEST_SECONDS - count, fps10);
    frames_total++;
    frames_sec++;

    us = timer_us_since(t_sec);
    if (us >= 1000000u) {
      fps10 = (u32)(((unsigned long long)frames_sec * 10000000ull) / us);
      samples[count++] = fps10;
      frames_sec = 0;
      t_sec = timer_ticks();
    }
    if (timer_us_since(t_stats) >= 250000u) {
      draw_stats(timer_us_since(t_all), frames_total, fps10, hw_text);
      t_stats = timer_ticks();
    }
    if (get_keys_down() & BUTTON_B) {
      aborted = 1;
      break;
    }
  }
  secs = timer_us_since(t_all) / 1000000u;

  if (count) {
    lo = samples[0];
    for (u32 i = 0; i < count; i++) {
      sum += samples[i];
      if (samples[i] < lo)
        lo = samples[i];
      if (samples[i] > hi)
        hi = samples[i];
    }
    mean10 = sum / count;
  }

  p = put_str(line, "Mean ");
  p = put_tenths(p, mean10);
  p = put_str(p, " fps");
  *p = 0;
  ui_dialog(VRAM_TOP_LA, TOP_SCREEN_WIDTH, TOP_SCREEN_HEIGHT, TOP_SCREEN_HEIGHT,
            count ? line : "No full second measured", hw_text, g_accent, 1);
  screen_present_top();

  ui_wallpaper(VRAM_BOT_A, BOT_SCREEN_WIDTH, BOT_SCREEN_HEIGHT,
               BOT_SCREEN_HEIGHT);
  draw_gradient_round_rect(VRAM_BOT_A, 16, 16, BOT_SCREEN_WIDTH - 32, 178, 14,
                           BOT_SCREEN_HEIGHT, COLOR_PANEL_TOP, COLOR_PANEL_BOT);
  ui_text_mid(VRAM_BOT_A, BOT_SCREEN_WIDTH / 2, 26, BOT_SCREEN_HEIGHT,
              count ? line : "No result", COLOR_WHITE, COLOR_PANEL_TOP,
              &ui_title);

  p = put_str(line, "Min ");
  p = put_tenths(p, lo);
  p = put_str(p, "   Max ");
  p = put_tenths(p, hi);
  *p = 0;
  ui_text_mid(VRAM_BOT_A, BOT_SCREEN_WIDTH / 2, 64, BOT_SCREEN_HEIGHT, line,
              COLOR_HM_TEXT2, COLOR_PANEL_TOP, &ui_font);

  p = put_u32(line, frames_total);
  p = put_str(p, " frames in ");
  p = put_u32(p, secs);
  p = put_str(p, " s");
  *p = 0;
  ui_text_mid(VRAM_BOT_A, BOT_SCREEN_WIDTH / 2, 90, BOT_SCREEN_HEIGHT, line,
              COLOR_HM_TEXT2, COLOR_PANEL_TOP, &ui_font);

  if (aborted)
    ui_text_mid(VRAM_BOT_A, BOT_SCREEN_WIDTH / 2, 116, BOT_SCREEN_HEIGHT,
                "Stopped early", COLOR_HM_TEXT2, COLOR_PANEL_BOT, &ui_font);
  ui_text_mid(VRAM_BOT_A, BOT_SCREEN_WIDTH / 2, 146, BOT_SCREEN_HEIGHT,
              hw_text, COLOR_HM_TEXT2, COLOR_PANEL_BOT, &ui_small);
  if (hw == AURORA_N3DS_HW_APPLIED || hw == AURORA_N3DS_HW_ALREADY ||
      hw == AURORA_N3DS_HW_REFUSED) {
    /* The raw register before and after, to diagnose a switch that did not
     * take. */
    p = put_str(line, "Clock ");
    p = put_hex(p, clk_before);
    p = put_str(p, " to ");
    p = put_hex(p, clk_after);
    *p = 0;
    ui_text_mid(VRAM_BOT_A, BOT_SCREEN_WIDTH / 2, 166, BOT_SCREEN_HEIGHT, line,
                COLOR_HM_TEXT2, COLOR_PANEL_BOT, &ui_small);
  }
  ui_text_mid(VRAM_BOT_A, BOT_SCREEN_WIDTH / 2, BOT_SCREEN_HEIGHT - 26,
              BOT_SCREEN_HEIGHT, "A / B: close", COLOR_HM_TEXT2,
              COLOR_HM_BG_BOT, &ui_small);
  screen_present_bottom();

  wait_close();
}
