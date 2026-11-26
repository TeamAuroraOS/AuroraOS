// Example mod for Aurora Menu: a page with two rows and a status HUD.
#include <aurora_amenu.h>

static int g_sel = 0;
static int g_hud = 0;

static void on_open(const AmenuApi *api) {
  (void)api;
  g_sel = 0;
}

static int on_frame(const AmenuApi *api, uint32_t held, uint32_t down) {
  (void)held;

  if (down & AMENU_KEY_B)
    return 0;
  if (down & (AMENU_KEY_UP | AMENU_KEY_DOWN))
    g_sel ^= 1;
  if (down & AMENU_KEY_A) {
    if (g_sel == 0)
      g_hud = !g_hud;
    else
      return 0;
  }

  api->sidebar_clear();
  api->sidebar_header("Aurora Menu Example",
                      api->app_modding_allowed() ? "MOD: ON" : "MOD: OFF");
  api->sidebar_item(0, "Status HUD", g_hud ? "ON" : "OFF", g_sel == 0);
  api->sidebar_item(1, "Back", "", g_sel == 1);
  api->sidebar_footer("A: Toggle  B: Back");
  return 1;
}

static void on_hud(const AmenuApi *api, int app_active, int modding_allowed) {
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
  AMENU_FLAG_HAS_MENU | AMENU_FLAG_HAS_HUD,
  NULL,
  on_open,
  on_frame,
  on_hud,
  on_toggle,
};

AMENU_EXPORT_PLUGIN(g_plugin);
