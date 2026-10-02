/* Touch calibration. Each target's raw readings are averaged over the press,
 * the two targets on each edge are averaged together, and a straight line per
 * axis through those gives the readings at the screen's edges. */
#include "touchcal.h"
#include "aurora.h"
#include "assets.h"
#include "icons.h"
#include "lang.h"
#include "statusbar.h"
#include "ui.h"
#include "anim.h"

#define CAL_MARGIN      40
#define CAL_SAMPLES     64 /* averaged per target; later readings are ignored */
#define CAL_MIN_SAMPLES 4  /* a shorter press is taken as a slip */
#define CAL_SKIP        2  /* the first readings of a press are still settling */

#define TSH TOP_SCREEN_HEIGHT
#define BSH BOT_SCREEN_HEIGHT

static const int target_x[4] = {CAL_MARGIN, BOT_SCREEN_WIDTH - CAL_MARGIN,
                                BOT_SCREEN_WIDTH - CAL_MARGIN, CAL_MARGIN};
static const int target_y[4] = {CAL_MARGIN, CAL_MARGIN,
                                BOT_SCREEN_HEIGHT - CAL_MARGIN,
                                BOT_SCREEN_HEIGHT - CAL_MARGIN};

static void cal_top(const char *line1, const char *line2, const char *step) {
  volatile u8 *fb = VRAM_TOP_LA;

  ui_wallpaper(fb, TOP_SCREEN_WIDTH, TSH, TSH);
  status_bar_draw();
  ui_icon(fb, (TOP_SCREEN_WIDTH - 96) / 2, 30, 96, TSH, ASSET_ICON_CONSOLE_64,
          icon_boot_bits, COLOR_WHITE);
  ui_text_mid(fb, TOP_SCREEN_WIDTH / 2, 132, TSH, L(STR_TOUCH_CAL), COLOR_WHITE,
              COLOR_HM_BG_BOT, &ui_title);
  ui_text_mid_fit(fb, TOP_SCREEN_WIDTH / 2, 164, TSH, line1,
                  TOP_SCREEN_WIDTH - 40, COLOR_WHITE, COLOR_HM_BG_BOT,
                  &ui_font);
  ui_text_mid_fit(fb, TOP_SCREEN_WIDTH / 2, 188, TSH, line2,
                  TOP_SCREEN_WIDTH - 40, COLOR_HM_TEXT2, COLOR_HM_BG_BOT,
                  &ui_small);
  if (step)
    ui_text_mid(fb, TOP_SCREEN_WIDTH / 2, 210, TSH, step, g_accent,
                COLOR_HM_BG_BOT, &ui_bold);
  screen_present_top();
}

static void cross(int x, int y, int arm, Color c) {
  draw_filled_rect(VRAM_BOT_A, x - arm, y, 2 * arm + 1, 1, BSH, c);
  draw_filled_rect(VRAM_BOT_A, x, y - arm, 1, 2 * arm + 1, BSH, c);
}

static void cal_target(int i) {
  volatile u8 *fb = VRAM_BOT_A;
  int x = target_x[i], y = target_y[i];

  draw_filled_rect(fb, 0, 0, BOT_SCREEN_WIDTH, BSH, BSH, COLOR_HM_BG);
  draw_filled_round_rect(fb, x - 14, y - 14, 29, 29, 14, BSH, g_accent);
  draw_filled_round_rect(fb, x - 11, y - 11, 23, 23, 11, BSH, COLOR_HM_BG);
  cross(x, y, 20, COLOR_WHITE);
  screen_present_bottom();
}

/* The raw reading averaged over one press. 0 when B cancels. A press already
 * down on arrival has to lift first, so it cannot count for this target. */
static int cal_sample(int *rx, int *ry) {
  int armed = 0, seen = 0, n = 0, sx = 0, sy = 0;

  for (;;) {
    u32 k = get_keys_down();
    int x, y;

    if (k & BUTTON_B)
      return 0;
    if (!touch_read(0, 0, &x, &y)) {
      if (armed && n >= CAL_MIN_SAMPLES) {
        *rx = sx / n;
        *ry = sy / n;
        return 1;
      }
      armed = 1;
      seen = n = sx = sy = 0;
    } else if (armed && ++seen > CAL_SKIP && n < CAL_SAMPLES) {
      sx += x;
      sy += y;
      n++;
    }
    ui_idle();
  }
}

static int dist(int a, int b) { return a > b ? a - b : b - a; }

/* Targets 0..3 run clockwise from the top left. 0 when the two readings for an
 * edge disagree by more than a quarter of the span, which means a missed tap. */
static int cal_solve(const int *rx, const int *ry, TouchCal *out) {
  const int dx = BOT_SCREEN_WIDTH - 2 * CAL_MARGIN;
  const int dy = BOT_SCREEN_HEIGHT - 2 * CAL_MARGIN;
  int left = (rx[0] + rx[3]) / 2, right = (rx[1] + rx[2]) / 2;
  int top = (ry[0] + ry[1]) / 2, bottom = (ry[2] + ry[3]) / 2;
  int span_x = right - left, span_y = bottom - top;
  int tol_x = dist(right, left) / 4, tol_y = dist(bottom, top) / 4;

  if (dist(rx[0], rx[3]) > tol_x || dist(rx[1], rx[2]) > tol_x ||
      dist(ry[0], ry[1]) > tol_y || dist(ry[2], ry[3]) > tol_y)
    return 0;
  out->x_min = (int16_t)(left - CAL_MARGIN * span_x / dx);
  out->x_max = (int16_t)(left + (BOT_SCREEN_WIDTH - CAL_MARGIN) * span_x / dx);
  out->y_min = (int16_t)(top - CAL_MARGIN * span_y / dy);
  out->y_max = (int16_t)(top + (BOT_SCREEN_HEIGHT - CAL_MARGIN) * span_y / dy);
  return 1;
}

/* Returns 1 to keep, 2 to measure again, 0 to cancel. */
static int cal_check(void) {
  volatile u8 *fb = VRAM_BOT_A;
  int last_x = -1, last_y = -1;

  anim_transition(ANIM_FADE, ANIM_BOTH);
  cal_top(L(STR_CAL_CHECK), L(STR_CAL_CHECK_HINT), 0);
  draw_filled_rect(fb, 0, 0, BOT_SCREEN_WIDTH, BSH, BSH, COLOR_HM_BG);
  for (int i = 0; i < 4; i++)
    cross(target_x[i], target_y[i], 8, COLOR_HM_TEXT2);
  cross(BOT_SCREEN_WIDTH / 2, BSH / 2, 8, COLOR_HM_TEXT2);
  screen_present_bottom();

  for (;;) {
    u32 k = get_keys_down();
    int x, y;

    if (k & BUTTON_A)
      return 1;
    if (k & BUTTON_X)
      return 2;
    if (k & BUTTON_B)
      return 0;
    if (touch_read(&x, &y, 0, 0) && (x != last_x || y != last_y)) {
      draw_filled_round_rect(fb, x - 2, y - 2, 5, 5, 2, BSH, g_accent);
      screen_present_bottom();
      last_x = x;
      last_y = y;
    }
    ui_idle();
  }
}

/* 1 to measure again, 0 to cancel. */
static int cal_retry(void) {
  ui_dialog(VRAM_BOT_A, BOT_SCREEN_WIDTH, BSH, BSH, L(STR_TOUCH_CAL),
            L(STR_CAL_RETRY), g_accent, 1);
  screen_present_bottom();
  for (;;) {
    u32 k = get_keys_down();
    int x, y;
    if (k & BUTTON_B)
      return 0;
    if ((k & BUTTON_A) || touch_tap(&x, &y))
      return 1;
    ui_idle();
  }
}

int touch_calibrate(TouchCal *result) {
  TouchCal old, fresh;
  int rx[4], ry[4];

  touch_cal_get(&old);
  for (int pass = 0;; pass++) {
    int measured = 1;

    for (int i = 0; i < 4 && measured; i++) {
      char step[24];
      const char *word = L(STR_CAL_POINT);
      int n = 0;
      while (word[n] && n < 16) {
        step[n] = word[n];
        n++;
      }
      step[n++] = ' ';
      step[n++] = (char)('1' + i);
      step[n++] = ' ';
      step[n++] = '/';
      step[n++] = ' ';
      step[n++] = '4';
      step[n] = 0;
      /* The first target arrives with the screen's own slide. */
      if (i || pass)
        anim_transition(ANIM_FADE, ANIM_BOTH);
      cal_top(L(STR_CAL_TAP), L(STR_CAL_HINT), step);
      cal_target(i);
      measured = cal_sample(&rx[i], &ry[i]);
    }
    if (!measured)
      return 0;

    if (!cal_solve(rx, ry, &fresh) || !touch_cal_set(&fresh)) {
      if (!cal_retry())
        return 0;
      continue;
    }

    int answer = cal_check();
    if (answer == 1) {
      *result = fresh;
      return 1;
    }
    touch_cal_set(&old);
    if (answer == 0)
      return 0;
  }
}
