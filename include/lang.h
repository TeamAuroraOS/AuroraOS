#ifndef AURORA_LANG_H
#define AURORA_LANG_H

#include "user.h" /* LANG_COUNT */

typedef enum {
  /* Progress-bar labels -- must stay first and in step order (0..4). */
  STR_LANGUAGE = 0,
  STR_NETWORK,
  STR_DETAILS,
  STR_PERSONAL,
  STR_WELCOME,

  STR_GET_STARTED,

  STR_NET_L1,
  STR_NET_L2,
  STR_WIFI_UNAVAIL,
  STR_SKIP,
  STR_NET_HINT,

  STR_USER_L1,
  STR_USER_L2,
  STR_USER_NAME,
  STR_DAY,
  STR_MONTH,
  STR_YEAR,
  STR_BACK,
  STR_NEXT,
  STR_USER_HINT,

  STR_KB_ENTER_NAME,

  STR_PERS_L1,
  STR_PERS_HINT,

  STR_PRESS_A_START,
  STR_B_BACK,

  /* Home Menu / Settings */
  STR_SETTINGS,
  STR_WIFI,
  STR_ACCENT_COLOR,
  STR_BRIGHTNESS,
  STR_ABOUT,
  STR_OFF,
  STR_PICK_ACCENT,
  STR_A_APPLY_B_BACK,
  STR_POWER_OFF,
  STR_SYSTEM,
  STR_EMPTY_SLOT,
  STR_LOADING,
  STR_SOUND_TEST,
  STR_MUSIC,
  STR_NO_TRACKS,
  STR_PLAYING,
  STR_STOPPED,
  STR_DEBUG_CRASH,
  STR_TOUCH_TEST,
  STR_WIFI_TEST,

  STR_COUNT
} StringId;

/* Active language (a LANG_* value). Set from cfg->language. */
extern int g_lang;

/* Look up a localized string for the active language. */
const char *L(StringId id);

#endif /* AURORA_LANG_H */
