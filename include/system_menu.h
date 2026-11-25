#ifndef AURORA_SYSTEM_MENU_H
#define AURORA_SYSTEM_MENU_H

#include <stddef.h>
#include <stdint.h>

/* Shared ARM9/ARM11 control block, each field group in its own cache line:
 * the ARM9 writes app_active and app_caps, the ARM11 menu_open and action,
 * which the ARM9 only clears. */
#define AURORA_SYSTEM_MENU_ADDR 0x233A8000u
#define AURORA_SYSTEM_MENU_NONE 0u
#define AURORA_SYSTEM_MENU_HOME 1u

/* app_caps: set by an app's runtime at start. The core cannot take the ARM9
 * out of an app, so Return to Home works only in apps that say they act on
 * it; os_install_return clears the field for every launch.
 *
 * CAP_MODDING: set by newly compiled C SDK apps unless disabled with
 * --no-modding or AURORA_APP_MODDING(0). Old binaries never set this bit,
 * so they default to modding disabled. */
#define AURORA_SYSTEM_MENU_CAP_HOME    (1u << 0)
#define AURORA_SYSTEM_MENU_CAP_MODDING (1u << 1)

#define AURORA_SYSTEM_MENU_PATH_MAX 256u
#define AURORA_DOCK_MAX             3u
#define AURORA_DOCK_LAUNCH_NONE     0xFFFFFFFFu
#define AURORA_DOCK_ICON_W          32u
#define AURORA_DOCK_ICON_H          32u
#define AURORA_DOCK_THUMB_W         40u
#define AURORA_DOCK_THUMB_H         24u

typedef struct {
  char     name[32];
  char     path[128];
  uint32_t tint_rgb;
  uint8_t  has_icon;
  uint8_t  has_banner;
  uint8_t  has_thumb;
  uint8_t  _pad;
  uint16_t icon[AURORA_DOCK_ICON_W * AURORA_DOCK_ICON_H];     /* 32x32 RGB565 */
  uint16_t thumb[AURORA_DOCK_THUMB_W * AURORA_DOCK_THUMB_H];  /* 40x24 RGB565 */
} AuroraDockApp;

typedef struct __attribute__((aligned(32))) {
  /* Cache line 0 (offset 0): ARM9 -> ARM11 app state */
  volatile uint32_t app_active;
  volatile uint32_t app_caps;
  volatile uint32_t accent_rgb; /* 0xRRGGBB active OS accent color */
  volatile uint32_t brightness; /* 0..4 synced between OS and ARM11 */
  uint32_t _app_pad[4];

  /* Cache line 1 (offset 32): ARM11 -> ARM9 menu visibility */
  volatile uint32_t menu_open;
  uint32_t _open_pad[7];

  /* Cache line 2 (offset 64): ARM11 -> ARM9 menu action */
  volatile uint32_t action;
  uint32_t _action_pad[7];

  /* Cache line 3 (offset 96): ARM11 -> ARM9 modding sync */
  volatile uint32_t mod_flush_seq; /* bumped after app_mem_write */
  volatile uint32_t mod_keys;      /* virtual HID keys injected by a mod */
  uint32_t _mod_pad[6];

  /* Offset 128: running app SD path (written by OS on launch) */
  char app_path[AURORA_SYSTEM_MENU_PATH_MAX];

  /* Offset 384: decoded Figtree font blocks in ASSETS_ARENA_ADDR (0 if none) */
  volatile uint32_t font_small_addr;
  volatile uint32_t font_reg_addr;
  volatile uint32_t font_bold_addr;
  volatile uint32_t os_ready; /* 1 once os_main finishes boot */

  /* Active Home Menu theme palette (0xRRGGBB) so ARM11 sidebar matches Home UI */
  volatile uint32_t panel_top_rgb;
  volatile uint32_t panel_bot_rgb;
  volatile uint32_t slot_top_rgb;
  volatile uint32_t slot_bot_rgb;
  volatile uint32_t text_rgb;
  volatile uint32_t text_dim_rgb;
  volatile uint32_t font_title_addr; /* decoded Figtree title font in ASSETS_ARENA */

  /* Right-side Quick Switch Dock (up to 3 pinned apps from Home Menu) */
  volatile uint32_t dock_count;
  volatile uint32_t dock_launch_idx; /* AURORA_DOCK_LAUNCH_NONE or 0..2 */
  AuroraDockApp     dock[AURORA_DOCK_MAX];
} AuroraSystemMenuShared;

_Static_assert(offsetof(AuroraSystemMenuShared, menu_open) == 32,
               "ARM11 menu visibility must have its own cache line");
_Static_assert(offsetof(AuroraSystemMenuShared, action) == 64,
               "ARM11 menu action must have its own cache line");
_Static_assert(offsetof(AuroraSystemMenuShared, mod_flush_seq) == 96,
               "ARM11 mod sync must have its own cache line");
_Static_assert(sizeof(AuroraSystemMenuShared) <= 0x4000u,
               "AuroraSystemMenuShared must stay clear of CAM_SHARED_ADDR");

static inline volatile AuroraSystemMenuShared *aurora_system_menu_shared(void) {
  return (volatile AuroraSystemMenuShared *)AURORA_SYSTEM_MENU_ADDR;
}

#endif
