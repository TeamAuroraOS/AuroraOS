/* Resident ARM11 overlay. It owns input detection and the bottom-screen menu,
 * so an application's button loop and renderer cannot hide the chord. */
#include "core11.h"
#include "font.h"
#include "power.h"
#include "system_menu.h"

#define HID_PAD_ADDR 0x10146000u
#define KEY_A       (1u << 0)
#define KEY_B       (1u << 1)
#define KEY_SELECT  (1u << 2)
#define KEY_UP      (1u << 6)
#define KEY_DOWN    (1u << 7)
#define KEY_L       (1u << 9)
#define KEY_CHORD   (KEY_L | KEY_DOWN | KEY_SELECT)

#define BOT_FB_ADDR_A 0x18346500u
#define BOT_FB_ADDR_B 0x18446500u
#define BOT_FB_ADDR_C 0x18546500u
#define BOT_WIDTH     320
#define BOT_HEIGHT    240

static uint32_t menu_chord_latched;

static uint32_t menu_keys(void) {
  core11_sleep_poll();
  return ~MMIO32(HID_PAD_ADDR) & 0xFFFu;
}

static void menu_pixel(volatile uint8_t *fb, int x, int y, uint32_t rgb) {
  uint32_t offset;
  if ((uint32_t)x >= BOT_WIDTH || (uint32_t)y >= BOT_HEIGHT)
    return;
  offset = ((uint32_t)x * BOT_HEIGHT + (BOT_HEIGHT - 1u - (uint32_t)y)) * 3u;
  fb[offset] = (uint8_t)rgb;
  fb[offset + 1] = (uint8_t)(rgb >> 8);
  fb[offset + 2] = (uint8_t)(rgb >> 16);
}

static void menu_fill(volatile uint8_t *fb, uint32_t rgb) {
  for (int y = 0; y < BOT_HEIGHT; y++)
    for (int x = 0; x < BOT_WIDTH; x++)
      menu_pixel(fb, x, y, rgb);
}

static void menu_char(volatile uint8_t *fb, int x, int y, char c,
                      uint32_t color) {
  if (c < 0x20 || c > 0x7E)
    return;
  const uint8_t *glyph = font_data[(uint8_t)c - 0x20u];
  for (int row = 0; row < FONT_HEIGHT; row++)
    for (int col = 0; col < FONT_WIDTH; col++)
      if (glyph[row] & (uint8_t)(0x80u >> col))
        menu_pixel(fb, x + col, y + row, color);
}

static void menu_text(volatile uint8_t *fb, int x, int y, const char *text,
                      uint32_t color) {
  while (*text) {
    menu_char(fb, x, y, *text++, color);
    x += FONT_WIDTH;
  }
}

static volatile uint8_t *framebuffer(int frame) {
  uint32_t address = frame == 0 ? BOT_FB_ADDR_A
                     : frame == 1 ? BOT_FB_ADDR_B
                                  : BOT_FB_ADDR_C;
  return (volatile uint8_t *)address;
}

static void menu_draw(int frame, int selected, int confirm) {
  volatile uint8_t *fb = framebuffer(frame);
  const uint32_t cyan = 0x64E8C8u; /* stored as B, G, R in the framebuffer */
  const uint32_t white = 0xFFFFFFu;
  static const char *const menu_rows[] = {
      "Return to Home", "Power off", "Cancel"};
  static const char *const confirm_rows[] = {"Yes", "Back"};
  const char *const *rows = confirm ? confirm_rows : menu_rows;
  int count = confirm ? 2 : 3;

  menu_fill(fb, 0);
  menu_text(fb, 8, 8, "Aurore Tools", cyan);
  if (confirm)
    menu_text(fb, 8, 32, "Power off the console?", white);
  for (int i = 0; i < count; i++) {
    int y = (confirm ? 52 : 32) + i * 16;
    if (i == selected)
      menu_text(fb, 8, y, ">", cyan);
    menu_text(fb, 28, y, rows[i], white);
  }
  menu_text(fb, 8, BOT_HEIGHT - 16,
            confirm ? "A: Select   B: Back" : "A: Select   B: Close", cyan);
}

/* The return stub clears app_active. An app built before Beta 7 still polls
 * HOME while the menu is up and can leave that way; the OS then starts again
 * and probes this core, so the menu has to go at once. */
static int app_gone(void) {
  volatile AuroraSystemMenuShared *shared = aurora_system_menu_shared();
  dcache_inval_line((const void *)&shared->app_active);
  return !shared->app_active;
}

/* For an app whose runtime does not act on Return to Home (no app_caps). */
static void menu_notice(int frame) {
  volatile uint8_t *fb = framebuffer(frame);
  const uint32_t cyan = 0x64E8C8u;
  const uint32_t white = 0xFFFFFFu;
  static const char *const lines[] = {
      "This app was built before Beta 7,",
      "so it cannot be closed from here.",
      "Close this menu and press HOME,",
      "or rebuild the app with the new",
      "C SDK or Auric compiler."};
  uint32_t previous;

  menu_fill(fb, 0);
  menu_text(fb, 8, 8, "Aurore Tools", cyan);
  for (unsigned i = 0; i < sizeof(lines) / sizeof(lines[0]); i++)
    menu_text(fb, 8, 32 + (int)i * 12, lines[i], white);
  menu_text(fb, 8, BOT_HEIGHT - 16, "A or B: Back", cyan);
  gpu11_menu_show(frame);
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

static int menu_confirm_power(int frame) {
  uint32_t previous;
  int selected = 1;
  while (menu_keys() & (KEY_A | KEY_B))
    sleep_ms(16);
  menu_draw(frame, selected, 1);
  gpu11_menu_show(frame);
  previous = menu_keys();
  for (;;) {
    uint32_t current = menu_keys();
    uint32_t down = current & ~previous;
    if (app_gone())
      return 0;
    previous = current;
    if (down & KEY_UP)
      selected ^= 1;
    if (down & KEY_DOWN)
      selected ^= 1;
    if (down & KEY_B)
      return 0;
    if (down & KEY_A)
      return selected == 0;
    if (down & (KEY_UP | KEY_DOWN)) {
      menu_draw(frame, selected, 1);
      gpu11_menu_show(frame);
    }
    stereo11_tick();
    sleep_ms(16);
  }
}

static void menu_run(void) {
  volatile AuroraSystemMenuShared *shared = aurora_system_menu_shared();
  int frame = gpu11_menu_frame();
  int selected = 0;
  uint32_t previous;
  if (frame < 0)
    return;

  shared->menu_open = 1;
  dcache_clean_line((const void *)&shared->menu_open);
  while (menu_keys() & KEY_CHORD)
    sleep_ms(16);
  menu_draw(frame, selected, 0);
  gpu11_menu_show(frame);
  previous = menu_keys();

  for (;;) {
    uint32_t current = menu_keys();
    uint32_t down = current & ~previous;
    int old = selected;
    if (app_gone())
      break;
    previous = current;
    if (down & KEY_UP)
      selected = selected > 0 ? selected - 1 : 2;
    if (down & KEY_DOWN)
      selected = selected < 2 ? selected + 1 : 0;
    if (down & KEY_B)
      break;
    if (down & KEY_A) {
      if (selected == 0) {
        if (shared->app_caps & AURORA_SYSTEM_MENU_CAP_HOME) {
          shared->action = AURORA_SYSTEM_MENU_HOME;
          dcache_clean_line((const void *)&shared->action);
          break;
        }
        menu_notice(frame);
        menu_draw(frame, selected, 0);
        gpu11_menu_show(frame);
        previous = menu_keys();
        continue;
      }
      if (selected == 1) {
        if (menu_confirm_power(frame))
          power_shutdown();
        menu_draw(frame, selected, 0);
        gpu11_menu_show(frame);
        previous = menu_keys();
      } else {
        break;
      }
    }
    if (selected != old) {
      menu_draw(frame, selected, 0);
      gpu11_menu_show(frame);
    }
    stereo11_tick();
    sleep_ms(16);
  }

  while ((menu_keys() & 0xFFFu) && !app_gone())
    sleep_ms(16);
  shared->menu_open = 0;
  dcache_clean_line((const void *)&shared->menu_open);
  gpu11_menu_restore();
}

void system_menu11_poll(void) {
  volatile AuroraSystemMenuShared *shared = aurora_system_menu_shared();
  uint32_t held = menu_keys() & KEY_CHORD;
  dcache_inval_line((const void *)&shared->app_active);
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
