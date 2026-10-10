/* Buttons from the HID pad, touch and the circle pad from TouchShared (the
 * ARM11 core samples both), read once per hid_scan(). */
#include "app_internal.h"
#include "crash.h"
#include "touch.h"
#include "user.h"

/* Raw ADC units from the rest point: the dead zone and a full push, as the
 * 3D Model screen uses them. */
#define CPAD_DEAD 150
#define CPAD_FULL 1000
/* How far (of 127) the pad must go to count as a KEY_CPAD_* press. */
#define CPAD_KEY 48

static u32 g_held, g_down, g_up;
static int g_tx = -1, g_ty = -1, g_cx, g_cy;
static int g_touch;            /* 0 not set up yet, 1 usable, -1 not */
static bool g_touch_ignore = true; /* a touch from before the app started */

static u32 pad(void) { return ~REG_HID_PAD & 0xFFFu; }

/* Whatever is held as the app starts (the A that launched it) is not a new
 * press. */
void app_hid_init(void) { g_held = pad(); }

static int axis(int v) {
  int a = v < 0 ? -v : v;
  if (a <= CPAD_DEAD)
    return 0;
  a = (a - CPAD_DEAD) * 127 / (CPAD_FULL - CPAD_DEAD);
  if (a > 127)
    a = 127;
  return v < 0 ? -a : a;
}

static s16 get16(const u8 *p) { return (s16)(p[0] | (p[1] << 8)); }

/* The OS's calibration was in the image this app replaced, so it comes back
 * from USER.dat. */
static void touch_setup(void) {
  const u8 *d;
  TouchCal cal;

  g_touch = app_core() ? 1 : -1;
  if (g_touch < 0)
    return;
  d = app_user_dat();
  if (d && d[USER_DAT_TOUCH]) {
    cal.x_min = get16(d + USER_DAT_TOUCH + 1);
    cal.x_max = get16(d + USER_DAT_TOUCH + 3);
    cal.y_min = get16(d + USER_DAT_TOUCH + 5);
    cal.y_max = get16(d + USER_DAT_TOUCH + 7);
    touch_cal_set(&cal); /* keeps the default if these cannot be one */
  }
}

void hid_scan(void) {
  u32 cur = pad();
  int x, y;

  app_poll_home();
  crash_poll_arm11();
  if (!g_touch)
    touch_setup();
  if (g_touch > 0) {
    int now = touch_read(&x, &y, 0, 0);
    if (!now)
      g_touch_ignore = false;
    else if (g_touch_ignore)
      now = 0;
    if (now) {
      g_tx = x;
      g_ty = y;
      cur |= KEY_TOUCH;
    }
    cpad_read(&x, &y, 0, 0);
    g_cx = axis(x);
    g_cy = axis(y);
    if (g_cx >= CPAD_KEY)
      cur |= KEY_CPAD_RIGHT;
    if (g_cx <= -CPAD_KEY)
      cur |= KEY_CPAD_LEFT;
    if (g_cy >= CPAD_KEY)
      cur |= KEY_CPAD_UP;
    if (g_cy <= -CPAD_KEY)
      cur |= KEY_CPAD_DOWN;
  }
  g_down = cur & ~g_held;
  g_up = g_held & ~cur;
  g_held = cur;
}

uint32_t hid_keys_down(void) { return g_down; }
uint32_t hid_keys_held(void) { return g_held; }
uint32_t hid_keys_up(void) { return g_up; }

bool hid_touch_held(void) { return (g_held & KEY_TOUCH) != 0; }
bool hid_touch_down(void) { return (g_down & KEY_TOUCH) != 0; }
bool hid_touch_up(void) { return (g_up & KEY_TOUCH) != 0; }
int hid_touch_x(void) { return g_tx; }
int hid_touch_y(void) { return g_ty; }

bool hid_touch_in(int x, int y, int w, int h) {
  return g_tx >= 0 && touch_in(g_tx, g_ty, x, y, w, h);
}

void hid_cpad(int *x, int *y) {
  if (x)
    *x = g_cx;
  if (y)
    *y = g_cy;
}

uint32_t hid_wait(uint32_t keys) {
  while (app_loop()) {
    uint32_t k = g_down & keys;
    if (k)
      return k;
  }
  return 0;
}
