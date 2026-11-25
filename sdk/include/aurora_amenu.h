/* aurora_amenu.h - AuroraOS Aurora Menu (/Aurora/amenu) plugin SDK.
 *
 * Build a plugin with:
 *   python sdk/aurcc.py build --amenu my_plugin.c -o my_plugin.bin
 * and copy my_plugin.bin to SD:/Aurora/amenu/ on the SD card.
 *
 * Plugins run inside the Tesla-inspired Aurora Menu sidebar (L+Down+Select)
 * and can also draw persistent HUD overlays or mod the currently running app
 * (when the app permits modding; newly compiled C SDK apps default to YES,
 * old compiled apps default to NO). */
#ifndef AURORA_AMENU_SDK_H
#define AURORA_AMENU_SDK_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AMENU_ABI_VERSION   1u
#define AMENU_SIDEBAR_W     184
#define AMENU_SCREEN_W      320
#define AMENU_SCREEN_H      240
#define AMENU_TOP_W         400
#define AMENU_TOP_H         240

/* Screens for hud_rect / hud_text */
#define AMENU_SCREEN_TOP    0
#define AMENU_SCREEN_BOTTOM 1

/* Plugin capability flags */
#define AMENU_FLAG_HAS_MENU (1u << 0) /* Opens an interactive mini-app in sidebar */
#define AMENU_FLAG_HAS_HUD  (1u << 1) /* Runs on_hud_tick even when menu is closed */
#define AMENU_FLAG_MODS_APP (1u << 2) /* Interacts with / mods the running app */

/* Hardware button bitmasks passed to on_menu_frame and returned by keys_held */
#define AMENU_KEY_A      (1u << 0)
#define AMENU_KEY_B      (1u << 1)
#define AMENU_KEY_SELECT (1u << 2)
#define AMENU_KEY_START  (1u << 3)
#define AMENU_KEY_RIGHT  (1u << 4)
#define AMENU_KEY_LEFT   (1u << 5)
#define AMENU_KEY_UP     (1u << 6)
#define AMENU_KEY_DOWN   (1u << 7)
#define AMENU_KEY_R      (1u << 8)
#define AMENU_KEY_L      (1u << 9)
#define AMENU_KEY_X      (1u << 10)
#define AMENU_KEY_Y      (1u << 11)

/* Color helper (0xRRGGBB) */
#define AMENU_RGB(r, g, b) \
  ((uint32_t)((((r) & 0xFFu) << 16) | (((g) & 0xFFu) << 8) | ((b) & 0xFFu)))

#define AMENU_COL_WHITE   0xFFFFFFu
#define AMENU_COL_BLACK   0x000000u
#define AMENU_COL_DIM     0x9AA0A8u
#define AMENU_COL_GREEN   0x4CD964u
#define AMENU_COL_RED     0xFF453Au
#define AMENU_COL_YELLOW  0xFFD60Au
#define AMENU_COL_CYAN    0x64E8C8u

/* App memory regions accessible when app_modding_allowed() is true */
#define AMENU_APP_RAM_START  0x22000000u
#define AMENU_APP_RAM_END    0x23000000u
#define AMENU_APP_HEAP_START 0x24000000u
#define AMENU_APP_HEAP_END   0x25000000u

typedef struct AmenuApi {
  uint32_t abi_version;
  uint32_t sidebar_w;
  uint32_t screen_w;
  uint32_t screen_h;

  /* Tesla-style sidebar UI helpers */
  void (*sidebar_clear)(void);
  void (*sidebar_header)(const char *title, const char *subtitle);
  void (*sidebar_item)(int row, const char *label, const char *value,
                       int selected);
  void (*sidebar_separator)(int row, const char *label);
  void (*sidebar_footer)(const char *hint);

  /* Low-level drawing on the sidebar framebuffer (320x240) */
  void (*draw_pixel)(int x, int y, uint32_t rgb);
  void (*draw_rect)(int x, int y, int w, int h, uint32_t rgb);
  void (*draw_rect_alpha)(int x, int y, int w, int h, uint32_t rgb, int alpha);
  void (*draw_text)(int x, int y, const char *text, uint32_t rgb);

  /* Persistent HUD overlay drawing (screen: AMENU_SCREEN_TOP or BOTTOM) */
  void (*hud_rect)(int screen, int x, int y, int w, int h, uint32_t rgb,
                   int alpha);
  void (*hud_text)(int screen, int x, int y, const char *text, uint32_t rgb);

  /* System utilities */
  uint32_t (*accent_color)(void);
  uint32_t (*now_us)(void);
  uint32_t (*keys_held)(void);
  int      (*get_brightness)(void);
  void     (*set_brightness)(int level);
  void     (*request_home)(void);

  /* Active app inspection & modding (gated by app's modding permission) */
  int         (*app_active)(void);
  int         (*app_modding_allowed)(void);
  const char *(*app_path)(void);
  int         (*app_mem_read)(uint32_t addr, void *dst, uint32_t len);
  int         (*app_mem_write)(uint32_t addr, const void *src, uint32_t len);
  uint32_t    (*app_mem_find)(uint32_t start, uint32_t len,
                              const void *pattern, uint32_t pat_len);
  int         (*app_set_mod_keys)(uint32_t key_mask);

  /* Extended system & hardware control (available on both Home Menu and apps) */
  void (*set_accent)(uint32_t rgb);
  void (*set_led_color)(uint8_t r, uint8_t g, uint8_t b);
  void (*play_tone)(uint32_t freq_hz);
  int  (*get_battery)(int *charging, int *mv, int *temp_c);
  void (*get_sys_info)(uint32_t *cpu_mhz, uint32_t *is_n3ds,
                       uint32_t *core_ver, uint32_t *gpu_ops);
  void (*reboot)(void);
} AmenuApi;

typedef struct AmenuPlugin {
  uint32_t abi_version;
  char     name[32];
  char     author[24];
  char     version[12];
  uint32_t flags;
  void (*on_load)(const AmenuApi *api);
  void (*on_menu_open)(const AmenuApi *api);
  /* Return 1 to stay open in the sidebar, 0 to return to the main menu. */
  int  (*on_menu_frame)(const AmenuApi *api, uint32_t keys_held,
                        uint32_t keys_down);
  /* Called periodically while running (even when sidebar is closed) if HUD
   * is enabled for this plugin. */
  void (*on_hud_tick)(const AmenuApi *api, int app_active, int modding_allowed);
  /* Optional callback when the plugin's HUD/mod is toggled from the sidebar. */
  int  (*on_toggle)(const AmenuApi *api, int enabled);
} AmenuPlugin;

/* Convenience typed memory helpers for app modding */
static inline int amenu_read32(const AmenuApi *api, uint32_t addr,
                               uint32_t *out) {
  return api->app_mem_read(addr, out, sizeof(uint32_t));
}

static inline int amenu_write32(const AmenuApi *api, uint32_t addr,
                                uint32_t val) {
  return api->app_mem_write(addr, &val, sizeof(uint32_t));
}

static inline int amenu_read16(const AmenuApi *api, uint32_t addr,
                               uint16_t *out) {
  return api->app_mem_read(addr, out, sizeof(uint16_t));
}

static inline int amenu_write16(const AmenuApi *api, uint32_t addr,
                                uint16_t val) {
  return api->app_mem_write(addr, &val, sizeof(uint16_t));
}

static inline int amenu_read8(const AmenuApi *api, uint32_t addr,
                              uint8_t *out) {
  return api->app_mem_read(addr, out, sizeof(uint8_t));
}

static inline int amenu_write8(const AmenuApi *api, uint32_t addr,
                               uint8_t val) {
  return api->app_mem_write(addr, &val, sizeof(uint8_t));
}

/* Export your AmenuPlugin struct as the plugin entry point:
 *   static const AmenuPlugin my_plugin = { ... };
 *   AMENU_EXPORT_PLUGIN(my_plugin);
 */
#define AMENU_EXPORT_PLUGIN(plugin_var)                                \
  __attribute__((section(".amenu_entry"), used))                       \
  const AmenuPlugin *amenu_plugin_entry(const AmenuApi *api) {         \
    (void)api;                                                         \
    return &(plugin_var);                                              \
  }

#ifdef __cplusplus
}
#endif

#endif
