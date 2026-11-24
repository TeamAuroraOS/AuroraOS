#ifndef AURORA_THEME_H
#define AURORA_THEME_H

#include "aurora.h"

#define THEME_DIR       "Aurora/themes"
#define THEME_CONFIG    "Aurora/theme.txt"
#define THEME_BUILTIN   "@default"
#define THEME_MAX       24
#define THEME_NAME_MAX  64

#define THEME_CAP_TOP_IMG  (1u << 0)
#define THEME_CAP_BOT_IMG  (1u << 1)
#define THEME_CAP_BGM      (1u << 2)
#define THEME_CAP_SFX      (1u << 3)
#define THEME_CAP_COLORS   (1u << 4)
#define THEME_CAP_ACCENT   (1u << 5)

typedef struct {
  int active;
  u32 caps;
  char id[THEME_NAME_MAX]; /* folder name, "" for built-in Aurora */
  char title[48];          /* display name (theme.ini name= or folder) */
  char author[32];         /* optional author= from theme.ini */
  char bgm_path[128];      /* Aurora/themes/<id>/bgm.wav */
  char sfx_select[128];    /* Aurora/themes/<id>/select.wav */
  char sfx_back[128];      /* Aurora/themes/<id>/back.wav */
  char sfx_move[128];      /* Aurora/themes/<id>/move.wav */

  Color accent;
  Color bg_top;
  Color bg_bot;
  Color panel_top;
  Color panel_bot;
  Color slot_top;
  Color slot_bot;
  Color slot_empty_top;
  Color slot_empty_bot;
  Color folder_top;
  Color folder_bot;
  Color status_bar;
  Color text;
  Color text_dim;
} HomeTheme;

typedef struct {
  char id[THEME_NAME_MAX];
  char title[48];
  char author[32];
  u32 caps;
} ThemeEntry;

extern HomeTheme g_theme;

/* Reset g_theme to Aurora's built-in palette and clear custom media paths. */
void theme_defaults(HomeTheme *t);

/* Load saved theme from SD:/Aurora/theme.txt at boot. */
void theme_boot_load(Color fallback_accent);

/* Apply `id` ("" for built-in Aurora) and persist to Aurora/theme.txt.
 * Updates wallpaper cache, g_theme palette, g_accent (if theme specifies one,
 * else restores fallback_accent), and reloads Home Menu BGM/SFX. */
int theme_select(const char *id, Color fallback_accent);

/* Scan SD:/Aurora/themes/ for valid themes; entry 0 is always built-in Aurora.
 * Returns number of entries populated (1..max). */
int theme_scan_entries(ThemeEntry *out, int max);

#endif
