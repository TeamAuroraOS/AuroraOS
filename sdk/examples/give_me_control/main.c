/* GiveMeControl: Full system inspector & AuroraOS playground mod.
 *
 * Build with:
 *   python sdk/aurcc.py build --amenu sdk/examples/give_me_control -o Aurora/amenu/GiveMeControl.bin
 * Copy GiveMeControl.bin to SD:/Aurora/amenu/ (or /Aurora/Apps/C/ — Aurora
 * automatically moves AMNU mods to /Aurora/amenu/).
 *
 * Works on BOTH the Home Menu and inside any running app!
 * Use L / R to switch tabs: System | Hardware | Overlays | Memory Lab.
 */
#include <aurora_amenu.h>

enum {
  TAB_SYSTEM = 0,
  TAB_HARDWARE,
  TAB_OVERLAYS,
  TAB_MEMORY,
  TAB_COUNT
};

static const char *const g_tab_names[TAB_COUNT] = {
  "1/4 System",
  "2/4 Hardware",
  "3/4 Overlays",
  "4/4 Mem Lab",
};

static int g_tab = TAB_SYSTEM;
static int g_sel = 0;

/* Hardware state */
static int g_accent_idx = 0;
static int g_led_idx = 0;
static int g_tone_idx = 0;

/* Overlay / fun toggles */
static int g_hud_sysmon = 0;
static int g_hud_input = 0;
static int g_rainbow_ui = 0;
static int g_night_tint = 0;
static int g_crosshair = 0;

/* Memory lab state */
static int g_mem_heap = 0;
static uint32_t g_mem_addr = AMENU_APP_RAM_START;
static uint32_t g_mem_val = 0;
static int g_mem_ok = 0;
static int g_turbo_a = 0;

static uint32_t g_tick = 0;

static const uint32_t g_accents[] = {
  0x64E8C8u, 0xFF5A52u, 0xFF9F0Au, 0x34C85Au,
  0x3B82F6u, 0xA05CE2u, 0xFF2DB8u, 0xFFFFFFu,
};
static const char *const g_accent_names[] = {
  "Teal", "Coral", "Amber", "Mint", "Sky", "Violet", "Pink", "White",
};

static const char *const g_led_names[] = {
  "Off", "Aurora", "Red", "Green", "Blue", "Purple", "Gold", "Rainbow",
};
static const uint8_t g_led_rgb[7][3] = {
  {0, 0, 0},
  {80, 230, 200},
  {255, 20, 20},
  {20, 240, 60},
  {30, 110, 255},
  {180, 40, 255},
  {255, 190, 20},
};

static const uint32_t g_tones[] = {0, 220, 440, 660, 880};
static const char *const g_tone_names[] = {
  "Off", "220Hz", "440Hz", "660Hz", "880Hz",
};

/* Tiny freestanding formatting helpers */
static char *put_str(char *p, const char *s) {
  while (*s) *p++ = *s++;
  *p = '\0';
  return p;
}

static char *put_uint(char *p, uint32_t v) {
  char tmp[12];
  int n = 0;
  do {
    tmp[n++] = (char)('0' + (v % 10u));
    v /= 10u;
  } while (v);
  while (n) *p++ = tmp[--n];
  *p = '\0';
  return p;
}

static char *put_int(char *p, int v) {
  if (v < 0) {
    *p++ = '-';
    return put_uint(p, (uint32_t)(-v));
  }
  return put_uint(p, (uint32_t)v);
}

static void put_hex32(char *out, uint32_t v) {
  static const char h[] = "0123456789ABCDEF";
  out[0] = '0';
  out[1] = 'x';
  for (int i = 0; i < 8; i++)
    out[2 + i] = h[(v >> (28 - i * 4)) & 0xFu];
  out[10] = '\0';
}

/* Smooth 6-sector HSV-like rainbow color from phase 0..255 */
static uint32_t rainbow_rgb(uint32_t phase) {
  uint32_t p = phase & 0xFFu;
  uint32_t r = 0, g = 0, b = 0;
  if (p < 85u) {
    r = 255u - p * 3u;
    g = p * 3u;
  } else if (p < 170u) {
    p -= 85u;
    g = 255u - p * 3u;
    b = p * 3u;
  } else {
    p -= 170u;
    b = 255u - p * 3u;
    r = p * 3u;
  }
  return AMENU_RGB(r, g, b);
}

static const char *basename_of(const char *path) {
  if (!path || !path[0]) return "Home OS";
  const char *last = path;
  for (const char *p = path; *p; p++)
    if (*p == '/' || *p == '\\')
      last = p + 1;
  return last[0] ? last : path;
}

static int tab_item_count(int tab) {
  if (tab == TAB_SYSTEM)   return 7;
  if (tab == TAB_HARDWARE) return 5;
  if (tab == TAB_OVERLAYS) return 5;
  return 6;
}

static void apply_led(const AmenuApi *api) {
  if (!api->set_led_color) return;
  if (g_led_idx < 7)
    api->set_led_color(g_led_rgb[g_led_idx][0], g_led_rgb[g_led_idx][1],
                       g_led_rgb[g_led_idx][2]);
}

static void on_open(const AmenuApi *api) {
  g_mem_ok = amenu_read32(api, g_mem_addr, &g_mem_val);
}

static int on_frame(const AmenuApi *api, uint32_t held, uint32_t down) {
  (void)held;
  if (down & AMENU_KEY_B)
    return 0;

  if (down & AMENU_KEY_L) {
    g_tab = (g_tab + TAB_COUNT - 1) % TAB_COUNT;
    g_sel = 0;
  }
  if (down & AMENU_KEY_R) {
    g_tab = (g_tab + 1) % TAB_COUNT;
    g_sel = 0;
  }

  int count = tab_item_count(g_tab);
  if (down & AMENU_KEY_UP)
    g_sel = g_sel > 0 ? g_sel - 1 : count - 1;
  if (down & AMENU_KEY_DOWN)
    g_sel = g_sel + 1 < count ? g_sel + 1 : 0;

  /* Handle interactions per tab */
  if (g_tab == TAB_HARDWARE) {
    if (g_sel == 0) {
      int br = api->get_brightness();
      if (down & AMENU_KEY_LEFT)  api->set_brightness(br - 1);
      if (down & AMENU_KEY_RIGHT) api->set_brightness(br + 1);
    } else if (g_sel == 1 && (down & (AMENU_KEY_A | AMENU_KEY_LEFT | AMENU_KEY_RIGHT))) {
      int n = (int)(sizeof(g_accents) / sizeof(g_accents[0]));
      if (down & AMENU_KEY_LEFT) g_accent_idx = (g_accent_idx + n - 1) % n;
      else                       g_accent_idx = (g_accent_idx + 1) % n;
      g_rainbow_ui = 0;
      if (api->set_accent) api->set_accent(g_accents[g_accent_idx]);
    } else if (g_sel == 2 && (down & (AMENU_KEY_A | AMENU_KEY_LEFT | AMENU_KEY_RIGHT))) {
      if (down & AMENU_KEY_LEFT) g_led_idx = (g_led_idx + 7) % 8;
      else                       g_led_idx = (g_led_idx + 1) % 8;
      apply_led(api);
    } else if (g_sel == 3 && (down & (AMENU_KEY_A | AMENU_KEY_LEFT | AMENU_KEY_RIGHT))) {
      int n = (int)(sizeof(g_tones) / sizeof(g_tones[0]));
      if (down & AMENU_KEY_LEFT) g_tone_idx = (g_tone_idx + n - 1) % n;
      else                       g_tone_idx = (g_tone_idx + 1) % n;
      if (api->play_tone) api->play_tone(g_tones[g_tone_idx]);
    } else if (g_sel == 4 && (down & AMENU_KEY_A)) {
      if (api->reboot) api->reboot();
    }
  } else if (g_tab == TAB_OVERLAYS) {
    if (down & (AMENU_KEY_A | AMENU_KEY_LEFT | AMENU_KEY_RIGHT)) {
      if (g_sel == 0) g_hud_sysmon = !g_hud_sysmon;
      else if (g_sel == 1) g_hud_input = !g_hud_input;
      else if (g_sel == 2) g_rainbow_ui = !g_rainbow_ui;
      else if (g_sel == 3) g_night_tint = !g_night_tint;
      else if (g_sel == 4) g_crosshair = !g_crosshair;
    }
  } else if (g_tab == TAB_MEMORY) {
    uint32_t base = g_mem_heap ? AMENU_APP_HEAP_START : AMENU_APP_RAM_START;
    uint32_t end  = g_mem_heap ? AMENU_APP_HEAP_END   : AMENU_APP_RAM_END;
    if (g_sel == 0 && (down & (AMENU_KEY_A | AMENU_KEY_LEFT | AMENU_KEY_RIGHT))) {
      g_mem_heap = !g_mem_heap;
      g_mem_addr = g_mem_heap ? AMENU_APP_HEAP_START : AMENU_APP_RAM_START;
      g_mem_ok = amenu_read32(api, g_mem_addr, &g_mem_val);
    } else if (g_sel == 1) {
      if (down & AMENU_KEY_LEFT)  g_mem_addr = (g_mem_addr >= base + 4u) ? g_mem_addr - 4u : base;
      if (down & AMENU_KEY_RIGHT) g_mem_addr = (g_mem_addr + 8u <= end)  ? g_mem_addr + 4u : base;
      if (down & AMENU_KEY_X)     g_mem_addr = (g_mem_addr + 0x104u <= end) ? g_mem_addr + 0x100u : base;
      if (down & AMENU_KEY_Y)     g_mem_addr = (g_mem_addr >= base + 0x100u) ? g_mem_addr - 0x100u : base;
      g_mem_ok = amenu_read32(api, g_mem_addr, &g_mem_val);
    } else if (g_sel == 3 && (down & AMENU_KEY_A)) {
      if (amenu_read32(api, g_mem_addr, &g_mem_val)) {
        g_mem_val += 1u;
        amenu_write32(api, g_mem_addr, g_mem_val);
        g_mem_ok = 1;
      }
    } else if (g_sel == 4 && (down & AMENU_KEY_A)) {
      amenu_write32(api, g_mem_addr, 0u);
      g_mem_ok = amenu_read32(api, g_mem_addr, &g_mem_val);
    } else if (g_sel == 5 && (down & (AMENU_KEY_A | AMENU_KEY_LEFT | AMENU_KEY_RIGHT))) {
      g_turbo_a = !g_turbo_a;
      if (!g_turbo_a) api->app_set_mod_keys(0);
    }
  }

  /* Draw current tab */
  api->sidebar_clear();
  api->sidebar_header("GiveMeControl", g_tab_names[g_tab]);

  if (g_tab == TAB_SYSTEM) {
    uint32_t mhz = 268, n3ds = 0, ver = 0, ops = 0;
    int chg = 0, mv = -1, temp = -1000, pct = -1;
    char b1[20], b2[20], b3[20], b4[20], b5[20];
    if (api->get_sys_info) api->get_sys_info(&mhz, &n3ds, &ver, &ops);
    if (api->get_battery)  pct = api->get_battery(&chg, &mv, &temp);

    char *p = put_uint(b1, mhz); put_str(p, " MHz");
    if (pct >= 0) {
      p = put_int(b2, pct);
      put_str(p, chg > 0 ? "% CHG" : "%");
    } else {
      put_str(b2, "--");
    }
    if (mv > 0) { p = put_int(b3, mv); put_str(p, " mV"); }
    else put_str(b3, "--");
    if (temp > -100) { p = put_int(b4, temp); put_str(p, " C"); }
    else put_str(b4, "--");
    p = put_str(b5, "v"); p = put_uint(p, ver);

    api->sidebar_item(0, "Model", n3ds ? "New 3DS" : "Old 3DS", g_sel == 0);
    api->sidebar_item(1, "CPU", b1, g_sel == 1);
    api->sidebar_item(2, "Battery", b2, g_sel == 2);
    api->sidebar_item(3, "Voltage", b3, g_sel == 3);
    api->sidebar_item(4, "Temp", b4, g_sel == 4);
    api->sidebar_item(5, "ARM11 Core", b5, g_sel == 5);
    api->sidebar_item(6, "Target", basename_of(api->app_path()), g_sel == 6);
    api->sidebar_footer("L/R: Tab   B: Back");
  } else if (g_tab == TAB_HARDWARE) {
    char br_s[8];
    int br = api->get_brightness();
    br_s[0] = (char)('1' + br); br_s[1] = '/'; br_s[2] = '5'; br_s[3] = '\0';
    api->sidebar_item(0, "Brightness", br_s, g_sel == 0);
    api->sidebar_item(1, "Accent", g_accent_names[g_accent_idx], g_sel == 1);
    api->sidebar_item(2, "Notify LED", g_led_names[g_led_idx], g_sel == 2);
    api->sidebar_item(3, "Audio Tone", g_tone_names[g_tone_idx], g_sel == 3);
    api->sidebar_item(4, "Reboot 3DS", ">", g_sel == 4);
    api->sidebar_footer("L/R: Tab  A/</>: Set");
  } else if (g_tab == TAB_OVERLAYS) {
    api->sidebar_item(0, "SysMon HUD", g_hud_sysmon ? "ON" : "OFF", g_sel == 0);
    api->sidebar_item(1, "Input HUD", g_hud_input ? "ON" : "OFF", g_sel == 1);
    api->sidebar_item(2, "Rainbow UI", g_rainbow_ui ? "ON" : "OFF", g_sel == 2);
    api->sidebar_item(3, "Night Tint", g_night_tint ? "ON" : "OFF", g_sel == 3);
    api->sidebar_item(4, "Crosshair", g_crosshair ? "ON" : "OFF", g_sel == 4);
    api->sidebar_footer("L/R: Tab  A: Toggle");
  } else {
    char addr_s[12], val_s[12];
    g_mem_ok = amenu_read32(api, g_mem_addr, &g_mem_val);
    put_hex32(addr_s, g_mem_addr);
    if (g_mem_ok) put_hex32(val_s, g_mem_val);
    else put_str(val_s, "--");

    api->sidebar_item(0, "Region", g_mem_heap ? "Heap" : "OS/App", g_sel == 0);
    api->sidebar_item(1, "Address", addr_s, g_sel == 1);
    api->sidebar_item(2, "Word", val_s, g_sel == 2);
    api->sidebar_item(3, "Poke +1", "Run", g_sel == 3);
    api->sidebar_item(4, "Poke Zero", "Run", g_sel == 4);
    api->sidebar_item(5, "Turbo A", g_turbo_a ? "ON" : "OFF", g_sel == 5);
    api->sidebar_footer(g_sel == 1 ? "</>:4 X/Y:256 L/R:Tab" : "L/R: Tab  A: Run");
  }
  return 1;
}

static void on_hud(const AmenuApi *api, int app_active, int modding_allowed) {
  (void)modding_allowed;
  g_tick++;

  /* Rainbow accent animation */
  if (g_rainbow_ui && api->set_accent && (g_tick & 1u) == 0u)
    api->set_accent(rainbow_rgb(g_tick * 3u));

  /* Rainbow physical 3DS notification LED animation */
  if (g_led_idx == 7 && api->set_led_color && (g_tick % 6u) == 0u) {
    uint32_t c = rainbow_rgb(g_tick * 5u);
    api->set_led_color((uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c);
  }

  /* Virtual Turbo A macro */
  if (g_turbo_a)
    api->app_set_mod_keys((g_tick & 1u) ? AMENU_KEY_A : 0u);

  /* Warm night-filter tint on top screen */
  if (g_night_tint)
    api->hud_rect(AMENU_SCREEN_TOP, 0, 0, AMENU_TOP_W, AMENU_TOP_H,
                  AMENU_RGB(255, 130, 30), 42);

  /* Centered crosshair overlay on top screen */
  if (g_crosshair) {
    uint32_t ac = api->accent_color();
    api->hud_rect(AMENU_SCREEN_TOP, 194, 119, 13, 2, ac, 230);
    api->hud_rect(AMENU_SCREEN_TOP, 199, 114, 2, 13, ac, 230);
  }

  /* System Monitor HUD bar on top screen */
  if (g_hud_sysmon) {
    uint32_t mhz = 268, n3ds = 0, ver = 0, ops = 0;
    int chg = 0, pct = -1;
    char line[64];
    if (api->get_sys_info) api->get_sys_info(&mhz, &n3ds, &ver, &ops);
    if (api->get_battery && (g_tick % 30u) == 1u) {
      pct = api->get_battery(&chg, 0, 0);
    }
    static int cached_pct = -1, cached_chg = 0;
    if (pct >= 0) { cached_pct = pct; cached_chg = chg; }
    char *p = put_str(line, n3ds ? "N3DS " : "O3DS ");
    p = put_uint(p, mhz); p = put_str(p, "MHz");
    if (cached_pct >= 0) {
      p = put_str(p, " | ");
      p = put_int(p, cached_pct);
      p = put_str(p, cached_chg > 0 ? "%+" : "%");
    }
    p = put_str(p, " | ");
    p = put_str(p, app_active ? basename_of(api->app_path()) : "HOME");

    api->hud_rect(AMENU_SCREEN_TOP, 6, 218, 250, 16, AMENU_RGB(14, 16, 22), 205);
    api->hud_text(AMENU_SCREEN_TOP, 12, 221, line, api->accent_color());
  }

  /* Live button input overlay */
  if (g_hud_input) {
    uint32_t k = api->keys_held();
    char buf[36];
    char *p = buf;
    if (k & AMENU_KEY_A)      p = put_str(p, "A ");
    if (k & AMENU_KEY_B)      p = put_str(p, "B ");
    if (k & AMENU_KEY_X)      p = put_str(p, "X ");
    if (k & AMENU_KEY_Y)      p = put_str(p, "Y ");
    if (k & AMENU_KEY_L)      p = put_str(p, "L ");
    if (k & AMENU_KEY_R)      p = put_str(p, "R ");
    if (k & AMENU_KEY_UP)     p = put_str(p, "U ");
    if (k & AMENU_KEY_DOWN)   p = put_str(p, "D ");
    if (k & AMENU_KEY_LEFT)   p = put_str(p, "< ");
    if (k & AMENU_KEY_RIGHT)  p = put_str(p, "> ");
    if (k & AMENU_KEY_START)  p = put_str(p, "ST ");
    if (k & AMENU_KEY_SELECT) p = put_str(p, "SL ");
    if (p == buf) put_str(p, "IDLE");
    api->hud_rect(AMENU_SCREEN_TOP, 290, 218, 104, 16, AMENU_RGB(14, 16, 22), 205);
    api->hud_text(AMENU_SCREEN_TOP, 296, 221, buf, AMENU_COL_WHITE);
  }
}

static int on_toggle(const AmenuApi *api, int enabled) {
  (void)api;
  g_hud_sysmon = enabled ? 1 : 0;
  return g_hud_sysmon;
}

static const AmenuPlugin g_plugin = {
  AMENU_ABI_VERSION,
  "GiveMeControl",
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
