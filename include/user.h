#ifndef AURORA_USER_H
#define AURORA_USER_H

#include "aurora.h"

#define USER_DAT_PATH     "Aurora/USER.dat"
#define USER_DAT_DIR      "Aurora"
#define USER_DAT_MAGIC    "ADAT"    /* 4 bytes, not NUL-terminated on disk */
#define USER_DAT_VERSION  1
#define USER_DAT_SIZE     64        /* fixed on-disk record size            */
#define USER_NAME_MAX     24        /* incl. NUL terminator                 */

enum {
  LANG_ENGLISH = 0,
  LANG_ESPANOL = 1,
  LANG_FRANCAIS = 2,
  LANG_COUNT
};

/* Runtime view of USER.dat. `valid` is set by user_config_load() when a well
 * formed "ADAT" file was read; the on-disk record never stores it. */
typedef struct {
  u8   valid;
  u8   setup_done;
  u8   language;      /* LANG_* */
  u8   accent;        /* index into aurora_accent_presets[] */
  u8   birth_day;     /* 1..31 */
  u8   birth_month;   /* 1..12 */
  u16  birth_year;    /* e.g. 2000 */
  char name[USER_NAME_MAX];
} UserConfig;

void user_config_defaults(UserConfig *cfg);

/* Returns 1 when SD:\Aurora\USER.dat exists and starts with "ADAT"; otherwise
 * 0, with cfg set to defaults. */
int  user_config_load(UserConfig *cfg);

/* Returns 1 on success, 0 on any SD / FatFs error. */
int  user_config_save(const UserConfig *cfg);

/* Returns once the user reaches the final Welcome screen. */
void setup_run(UserConfig *cfg);

/* Defined in os_setup.c. Index 0, Aurora teal, is the default. */
#define AURORA_ACCENT_COUNT 14
extern const Color aurora_accent_presets[AURORA_ACCENT_COUNT];
extern const char *aurora_accent_names[AURORA_ACCENT_COUNT];

#endif
