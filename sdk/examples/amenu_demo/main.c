/* amenu_demo: Example /Aurora/amenu sidebar plugin for AuroraOS.
 *
 * Build with:
 *   python sdk/aurcc.py build --amenu sdk/examples/amenu_demo -o amenu_demo.bin
 * Copy amenu_demo.bin to SD:/Aurora/amenu/ on the SD card.
 *
 * Demonstrates:
 *  - Custom Tesla-style sidebar mini-app menu
 *  - Persistent top-screen HUD overlay (even while sidebar is closed)
 *  - Live app inspection & memory modding (gated by the app's modding switch)
 */
#include <aurora_amenu.h>

static int g_sel = 0;
static int g_hud = 0;
static int g_turbo_a = 0;
static uint32_t g_peek_addr = AMENU_APP_RAM_START;
static uint32_t g_peek_val = 0;
static int g_peek_ok = 0;

static void hex32(char *out, uint32_t v) {
  static const char hex[] = "0123456789ABCDEF";
  out[0] = '0';
  out[1] = 'x';
  for (int i = 0; i < 8; i++)
    out[2 + i] = hex[(v >> (28 - i * 4)) & 0xFu];
  out[10] = '\0';
}

static void on_open(const AmenuApi *api) {
  g_sel = 0;
  g_peek_ok = amenu_read32(api, g_peek_addr, &g_peek_val);
}

static int on_frame(const AmenuApi *api, uint32_t held, uint32_t down) {
  (void)held;
  int mod_ok = api->app_modding_allowed();
  char addr_s[12], val_s[12];

  if (down & AMENU_KEY_B)
    return 0;
  if (down & AMENU_KEY_UP)
    g_sel = g_sel > 0 ? g_sel - 1 : 4;
  if (down & AMENU_KEY_DOWN)
    g_sel = g_sel < 4 ? g_sel + 1 : 0;

  if (g_sel == 2) {
    if (down & AMENU_KEY_LEFT)
      g_peek_addr = (g_peek_addr >= AMENU_APP_RAM_START + 4u)
                        ? g_peek_addr - 4u
                        : AMENU_APP_RAM_START;
    if (down & AMENU_KEY_RIGHT)
      g_peek_addr += 4u;
    if (down & (AMENU_KEY_LEFT | AMENU_KEY_RIGHT | AMENU_KEY_A))
      g_peek_ok = amenu_read32(api, g_peek_addr, &g_peek_val);
  } else if (down & AMENU_KEY_A) {
    if (g_sel == 0) {
      g_hud = !g_hud;
    } else if (g_sel == 1) {
      if (mod_ok) {
        g_turbo_a = !g_turbo_a;
        if (!g_turbo_a)
          api->app_set_mod_keys(0);
      }
    } else if (g_sel == 3) {
      if (mod_ok) {
        /* Increment the peeked word by 1 as a live memory-mod demo */
        if (amenu_read32(api, g_peek_addr, &g_peek_val)) {
          g_peek_val += 1u;
          amenu_write32(api, g_peek_addr, g_peek_val);
          g_peek_ok = 1;
        }
      }
    } else if (g_sel == 4) {
      return 0;
    }
  }

  hex32(addr_s, g_peek_addr);
  if (mod_ok && g_peek_ok)
    hex32(val_s, g_peek_val);
  else if (!mod_ok) {
    val_s[0] = 'B'; val_s[1] = 'l'; val_s[2] = 'o'; val_s[3] = 'c';
    val_s[4] = 'k'; val_s[5] = 'e'; val_s[6] = 'd'; val_s[7] = '\0';
  } else {
    val_s[0] = '-'; val_s[1] = '-'; val_s[2] = '\0';
  }

  api->sidebar_clear();
  api->sidebar_header("Aurora ModKit", mod_ok ? "MOD: ON" : "MOD: OFF");
  api->sidebar_item(0, "Status HUD", g_hud ? "ON" : "OFF", g_sel == 0);
  api->sidebar_item(1, "Turbo A Mod",
                    mod_ok ? (g_turbo_a ? "ON" : "OFF") : "LOCKED", g_sel == 1);
  api->sidebar_item(2, "Peek Addr", addr_s, g_sel == 2);
  api->sidebar_item(3, "Word +1", val_s, g_sel == 3);
  api->sidebar_item(4, "Back", "", g_sel == 4);
  api->sidebar_footer(g_sel == 2 ? "</>: Addr  B: Back" : "A: Toggle  B: Back");
  return 1;
}

static void on_hud(const AmenuApi *api, int app_active, int modding_allowed) {
  static uint32_t tick_n = 0;
  tick_n++;

  if (g_turbo_a && app_active && modding_allowed)
    api->app_set_mod_keys((tick_n & 1u) ? AMENU_KEY_A : 0u);

  if (!g_hud)
    return;
  api->hud_rect(AMENU_SCREEN_TOP, 4, 4, 152, 16, AMENU_COL_BLACK, 190);
  api->hud_text(AMENU_SCREEN_TOP, 8, 8,
                app_active ? (modding_allowed ? "ModKit | MOD: ON"
                                              : "ModKit | MOD: OFF")
                           : "ModKit | HOME",
                api->accent_color());
}

static int on_toggle(const AmenuApi *api, int enabled) {
  (void)api;
  g_hud = enabled ? 1 : 0;
  return g_hud;
}

static const AmenuPlugin g_plugin = {
  AMENU_ABI_VERSION,
  "Aurora ModKit",
  "AuroraOS",
  "1.0",
  AMENU_FLAG_HAS_MENU | AMENU_FLAG_HAS_HUD | AMENU_FLAG_MODS_APP,
  NULL,
  on_open,
  on_frame,
  on_hud,
  on_toggle,
};

AMENU_EXPORT_PLUGIN(g_plugin);
