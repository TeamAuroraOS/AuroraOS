#ifndef AURORA_USER_H
#define AURORA_USER_H

#include "aurora.h"
#include "touch.h"

#define USER_DAT_PATH     "Aurora/USER.dat"
#define USER_DAT_DIR      "Aurora"
#define USER_DAT_MAGIC    "ADAT"    /* 4 bytes, not NUL-terminated on disk */
#define USER_DAT_VERSION  3
#define USER_DAT_SIZE     64        /* fixed on-disk record size */
#define USER_NAME_MAX     24        /* incl. NUL terminator */

/* Offset of the touch calibration: a flag byte, then x_min, x_max, y_min and
 * y_max as little-endian int16. Version 1 files have zeros here, which read as
 * no calibration. */
#define USER_DAT_TOUCH    40

/* Offset of the clock offset (Settings > Clock): minutes as a little-endian
 * int16. Older files have zeros here, which is no offset. */
#define USER_DAT_CLOCK    49

/* Offset of the brightness level (0..4). Older files have a zero here, the
 * darkest level, so it is read only from version 3 files. */
#define USER_DAT_BRIGHTNESS 51

/* Offset of the sound switches, USER_SOUND_* bits for the sounds turned off.
 * Older files have a zero here, which is every sound on. */
#define USER_DAT_SOUND 52
#define USER_SOUND_STARTUP_OFF 0x01u
#define USER_SOUND_SHOP_OFF    0x02u /* aShop's music and its chime */
#define USER_SOUND_MENU_OFF    0x04u /* select, back and move */
#define USER_SOUND_HOME_OFF    0x08u
#define USER_SOUND_MASK        0x0Fu

/* Offset of the New 3DS's full power switch, 1 for off. Older files have a zero
 * here, which is on. */
#define USER_DAT_POWER 53

enum {
  LANG_ENGLISH = 0,
  LANG_ESPANOL = 1,
  LANG_FRANCAIS = 2,
  LANG_PORTUGUESE = 3,
  LANG_COUNT
};

/* Runtime view of USER.dat. `valid` is set by user_config_load() when a well
 * formed "ADAT" file was read; the on-disk record never stores it. */
typedef struct {
  u8   valid;
  u8   setup_done;
  u8   language;      /* LANG_* */
  u8   accent;        /* index into aurora_accent_presets[] */
  u8   birth_day;
  u8   birth_month;   /* 1..12 */
  u16  birth_year;    /* full year */
  char name[USER_NAME_MAX];
  u8   touch_set;     /* touch holds a measured calibration */
  TouchCal touch;
  s16  clock_offset;  /* minutes added to the RTC; see g_rtc_offset */
  u8   brightness;    /* 0..4, applied by the ARM11 LCD driver */
  u8   sound_off;     /* USER_SOUND_* */
  u8   power_off;     /* the New 3DS stays at 268 MHz, without its L2 cache */
} UserConfig;

void user_config_defaults(UserConfig *cfg);

/* Returns 1 when SD:\Aurora\USER.dat exists and starts with "ADAT"; otherwise
 * 0, with cfg set to defaults. */
int  user_config_load(UserConfig *cfg);

/* Returns 1 on success, 0 on any SD / FatFs error. */
int  user_config_save(const UserConfig *cfg);

/* What the network step reaches in os_main.c: Settings > Wi-Fi (the firmware
 * check and copy, the search, the join) and the name of the network saved,
 * "" for none. */
typedef struct {
  void (*wifi_screen)(void);
  const char *(*wifi_saved)(void);
} SetupWifi;

/* Returns once the user reaches the final Welcome screen. */
void setup_run(UserConfig *cfg, const SetupWifi *wifi);

/* Defined in os_setup.c. Index 0, Aurora teal, is the default. */
#define AURORA_ACCENT_COUNT 14
extern const Color aurora_accent_presets[AURORA_ACCENT_COUNT];
extern const char *aurora_accent_names[AURORA_ACCENT_COUNT];

#endif
