#ifndef AURORA_UPDATE_H
#define AURORA_UPDATE_H

/* System updates (docs/update.md). The OS asks the update server whether a
 * newer release exists, downloads the updater (updater/, a C SDK app) and
 * starts it; the updater downloads the release's files, swaps them in and
 * starts the new OS. Only #defines and plain C here: the updater includes
 * this file too, beside the SDK's headers. */

#define UPDATE_SERVER "https://update.aurora3ds.xyz"
/* Asked as UPDATE_LATEST "?current=<AURORA_VERSION_NUM>&build=<YYYYMMDD>". */
#define UPDATE_LATEST "/v1/latest"

/* Everything the update keeps on the card. */
#define UPDATE_DIR        "/Aurora/Update"
#define UPDATE_APP        "/Aurora/Update/Update.bin"
/* Written by the OS for the updater: key=value lines current, build, from,
 * firm. */
#define UPDATE_PLAN       "/Aurora/Update/plan.txt"
/* Written by the updater for the next OS start: ok=1|0, to, why, note. */
#define UPDATE_RESULT     "/Aurora/Update/result.txt"
/* The files being downloaded, until they are swapped in. */
#define UPDATE_NEW_DIR    "/Aurora/Update/new"
/* What the last update replaced, and info.txt (name=, firm=) about it.
 * Settings > System Update > Restore swaps them back. */
#define UPDATE_BACKUP_DIR "/Aurora/Update/backup"
#define UPDATE_BACKUP_INFO "/Aurora/Update/backup/info.txt"
/* The firm's loader boots this when AURORAOS.BIN is missing or damaged. */
#define UPDATE_BACKUP_OS  "Aurora/Update/backup/AURORAOS.BIN"

#define UPDATE_OS_PATH     "/AURORAOS.BIN"
#define UPDATE_ASSETS_PATH "/Aurora/assets.pak"
/* Where the firm is replaced when the firm that booted did not say where it
 * is (a firm from before updates; see AURORA_FIRM_INFO_ADDR in loader.h). */
#define UPDATE_DEFAULT_FIRM "/luma/payloads/Aurora.firm"

/* An update starts only above this charge, or on the charger. */
#define UPDATE_MIN_BATTERY 20

/* The OS's side, src/os/Update.c. */
#include <stdint.h>

typedef struct {
  int update;            /* 1 when a newer release is offered */
  char name[48];         /* the newest release, "Beta 9 v0.1.8" */
  char published[12];    /* "2026-12-01" */
  uint32_t size;         /* what an update downloads at most, in bytes */
  char app_url[192];     /* the updater */
  char app_sha256[65];
  uint32_t app_size;
} UpdateInfo;

/* Asks the server, on a joined network (wifi_direct_on around it). 1 when it
 * answered; `why` says what went wrong otherwise. */
int update_check(UpdateInfo *u, char *why, uint32_t max);
/* The newest release's notes, plain text, after an update_check that found
 * one; "" otherwise. */
const char *update_notes(void);
/* Downloads the updater to UPDATE_APP, checks it against the server's
 * SHA-256 and writes UPDATE_PLAN. The caller then starts UPDATE_APP. */
int update_fetch(const UpdateInfo *u,
                 void (*progress)(uint32_t done, uint32_t total), char *why,
                 uint32_t max);
/* 1 above UPDATE_MIN_BATTERY or on the charger. */
int update_battery_ok(void);
/* 1 when the last update left a backup; its version into `name`. */
int update_backup(char *name, uint32_t max);
/* Swaps the backup in (and the replaced files into the backup). The caller
 * then starts UPDATE_OS_PATH. */
int update_restore(char *why, uint32_t max);
/* At every OS start, on an unmounted card: removes the updater and its
 * downloads, and reads what it reported. 1 with a message to show once. */
int update_at_boot(const char **title, const char **line);
/* Settings > System Update. `launch` starts an app, `boot` an AURORAOS.BIN
 * in place of the running one; each returns only when it failed. */
void update_screen(void (*launch)(const char *path),
                   void (*boot)(const char *path));

#endif
