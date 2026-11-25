#ifndef AURORA_AMENU_H
#define AURORA_AMENU_H

#include <stdint.h>

/* Resident /Aurora/amenu plugin pool in FCRAM, between the ARM11 wake mailbox
 * (0x27000000) and GLB_FILE_ADDR / VIDEO_ENGINE_ADDR (0x27100000).
 * Persists across app launches so ARM11 overlays and mods stay loaded. */
#define AMENU_DIR           "Aurora/amenu"
#define AMENU_MAGIC         "AMNU"
#define AMENU_TABLE_MAGIC   0x42544D41u /* "AMTB" */
#define AMENU_ABI_VERSION   1u

#define AMENU_TABLE_ADDR    0x27001000u
#define AMENU_POOL_ADDR     0x27004000u
#define AMENU_LINK_BASE     0x27000000u
#define AMENU_MAX_PLUGINS   8u
#define AMENU_SLOT_SIZE     0x0001E000u /* 120 KB per plugin (code+data+bss) */
/* 0x27004000 + 8 * 0x1E000 = 0x270F4000 < 0x27100000 */

#define AMENU_SIDEBAR_W     184
#define AMENU_SCREEN_W      320
#define AMENU_SCREEN_H      240
#define AMENU_TOP_W         400
#define AMENU_TOP_H         240

/* Plugin capability flags */
#define AMENU_FLAG_HAS_MENU (1u << 0) /* Opens an interactive mini-app in sidebar */
#define AMENU_FLAG_HAS_HUD  (1u << 1) /* Runs on_hud_tick even when menu is closed */
#define AMENU_FLAG_MODS_APP (1u << 2) /* Interacts with / mods the running app */

/* On-disk header for SD:/Aurora/amenu/<plugin>.bin */
typedef struct {
  char     magic[4];      /* "AMNU" */
  uint32_t abi_version;   /* AMENU_ABI_VERSION */
  uint32_t base_addr;     /* Link base address (AMENU_LINK_BASE) */
  uint32_t entry_offset;  /* Byte offset of entry function from payload start */
  uint32_t payload_size;  /* Code + rodata + data bytes */
  uint32_t bss_size;      /* Zero-initialized bytes after payload */
  uint32_t reloc_offset;  /* File offset of uint32_t R_ARM_ABS32 offset array */
  uint32_t reloc_count;   /* Number of relocation entries */
  uint32_t flags;         /* AMENU_FLAG_* hints */
  uint32_t reserved;
} AmenuFileHeader;

_Static_assert(sizeof(AmenuFileHeader) == 40,
               "AmenuFileHeader must be 40 bytes");

/* Host API passed to plugins by both ARM11 (in-app) and ARM9 (in-OS) sidebars */
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

  /* Persistent HUD overlay drawing (screen: 0 = top 400x240, 1 = bottom 320x240) */
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

  /* Active app inspection & modding (gated by AURORA_SYSTEM_MENU_CAP_MODDING) */
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

/* Plugin descriptor returned by amenu_plugin_entry(const AmenuApi *api) */
typedef struct AmenuPlugin {
  uint32_t abi_version;
  char     name[32];
  char     author[24];
  char     version[12];
  uint32_t flags;
  void (*on_load)(const AmenuApi *api);
  void (*on_menu_open)(const AmenuApi *api);
  /* Return 1 to stay open in the sidebar, 0 to return to the main list. */
  int  (*on_menu_frame)(const AmenuApi *api, uint32_t keys_held,
                        uint32_t keys_down);
  /* Called periodically while running (even when sidebar is closed) if HUD
   * is enabled for this plugin. */
  void (*on_hud_tick)(const AmenuApi *api, int app_active, int modding_allowed);
  /* Optional quick toggle when HUD/mod is toggled from the main sidebar. */
  int  (*on_toggle)(const AmenuApi *api, int enabled);
} AmenuPlugin;

typedef const AmenuPlugin *(*AmenuEntryFn)(const AmenuApi *api);

typedef struct {
  uint32_t loaded;
  uint32_t base_addr;
  uint32_t entry_addr;
  uint32_t hud_enabled;
  uint32_t initialized;
  uint32_t plugin_ptr;
  char     filename[48];
} AmenuSlot;

typedef struct {
  volatile uint32_t magic;
  volatile uint32_t version;
  volatile uint32_t count;
  volatile uint32_t generation;
  volatile uint32_t hud_mask; /* bit i set iff slot i has HUD active */
  AmenuSlot slots[AMENU_MAX_PLUGINS];
} AmenuTable;

static inline AmenuTable *amenu_table(void) {
  return (AmenuTable *)AMENU_TABLE_ADDR;
}

/* OS-side loaders (ARM9): scan SD:/Aurora/amenu and load plugins into FCRAM.
 * amenu_load_mounted() assumes the SD card is already mounted by the caller. */
int amenu_load_plugins(void);
int amenu_load_mounted(void);
void amenu_migrate_file_mounted(const char *src_path, const char *fname);

#endif
