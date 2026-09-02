/*
 * AuroraOS user configuration (SD:\Aurora\USER.dat).
 *
 * The first time the OS boots without a valid config it runs the setup wizard
 * (see src/os/os_setup.c) and writes the user's choices here. On every later
 * boot the OS loads this file and applies the saved settings, so setup only
 * happens once.
 *
 * The file starts with the 4-byte magic "ADAT"; the OS ignores any file that
 * does not. On-disk layout is a fixed 64-byte little-endian record (see
 * user_config_load / user_config_save in os_setup.c) -- serialised field by
 * field so struct padding never leaks into the file.
 */
#ifndef AURORA_USER_H
#define AURORA_USER_H

#include "aurora.h"

#define USER_DAT_PATH     "Aurora/USER.dat"
#define USER_DAT_DIR      "Aurora"
#define USER_DAT_MAGIC    "ADAT"    /* 4 bytes, not NUL-terminated on disk */
#define USER_DAT_VERSION  1
#define USER_DAT_SIZE     64        /* fixed on-disk record size            */
#define USER_NAME_MAX     24        /* incl. NUL terminator                 */

/* Languages offered on the first setup screen. */
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

/* Fill cfg with sane defaults (English, Aurora accent, not set up). */
void user_config_defaults(UserConfig *cfg);

/* Load SD:\Aurora\USER.dat. Returns 1 and fills cfg when the file exists and
 * begins with the "ADAT" magic; returns 0 otherwise (cfg is set to defaults). */
int  user_config_load(UserConfig *cfg);

/* Serialise cfg (stamping the "ADAT" magic) to SD:\Aurora\USER.dat.
 * Returns 1 on success, 0 on any SD / FatFs error. */
int  user_config_save(const UserConfig *cfg);

/* Run the first-time setup wizard on both screens, filling cfg with the user's
 * selections. Returns once the user reaches the final Welcome screen. */
void setup_run(UserConfig *cfg);

/* Shared accent palette (defined in os_setup.c). Index 0 is Aurora teal, the
 * default. Used by both the setup Personalise screen and the Settings menu. */
#define AURORA_ACCENT_COUNT 14
extern const Color aurora_accent_presets[AURORA_ACCENT_COUNT];
extern const char *aurora_accent_names[AURORA_ACCENT_COUNT];

#endif /* AURORA_USER_H */
