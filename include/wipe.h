#ifndef AURORA_WIPE_H
#define AURORA_WIPE_H

/* Settings > Wipe Aurora in the firm (src/wipe.c): deletes SD:/Aurora,
 * SD:/AURORAOS.BIN and the firm's own file. Firm only. */

#include "aurora.h"

#define WIPE_PATH_MAX 512

enum { WIPE_FOLDER, WIPE_OS, WIPE_FIRM, WIPE_DONE };

typedef struct {
  int step;           /* WIPE_*: the one running, failed, or WIPE_DONE */
  u32 deleted;        /* files and folders removed so far */
  int fr;             /* the FatFs error that stopped it, 0 if none */
  int firm_missing;   /* the firm's file was not found (nothing to delete) */
  const char *current;
  char where[WIPE_PATH_MAX]; /* what could not be deleted */
} WipeStatus;

/* The firm's file: the path its loader passed, or the usual install path
 * when the loader passed none (wipe_firm_known() 0). */
const char *wipe_firm_path(void);
int wipe_firm_known(void);

/* Runs the three steps in order, calling `tick` after every item and step.
 * Returns 1 when nothing failed (firm_missing may still be set), 0 when a
 * step stopped at an error, leaving the steps after it undone. */
int wipe_run(WipeStatus *st, void (*tick)(const WipeStatus *, void *),
             void *ctx);

#endif
