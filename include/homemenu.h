#ifndef AURORA_HOMEMENU_H
#define AURORA_HOMEMENU_H

#include "aurora.h"

/* Something on the Home Menu that opens: a built-in screen or an SD app. */
typedef struct {
  const char *name;
  const char *dev;           /* the line under its name */
  const char *key;           /* names it in the layout file */
  const unsigned char *icon; /* built-in 32x32 bits, for without the pack */
  u32 asset_lg, asset_sm, asset_xs; /* pack art at 64, 32 and 16, or UI_NO_ASSET */
  Color tint;
  int action;       /* the caller's, for its open hook */
  const char *path; /* an SD app's container */
  u32 asset_art;    /* full-colour pack art the top screen shows alone in place
                       of the icon and name card, or UI_NO_ASSET */
} HomeApp;

typedef struct {
  void (*open)(const HomeApp *app); /* returns when the user is back */
  void (*settings)(void);
  void (*terminal)(void);
  void (*power_off)(void); /* does not return */
  void (*tick)(void);      /* every pass of the menu's loop */
} HomeHooks;

/* Arranges `n` apps as the layout file says (see homelayout.h). The table
 * must outlive the menu. */
void home_init(const HomeApp *apps, int n, const HomeHooks *hooks);
/* After the app table changed (an app was installed): re-reads the layout
 * file for the new table and keeps the folder and page shown. Call from the
 * open hook; the menu redraws when the hook returns. */
void home_reload(const HomeApp *apps, int n);
/* Both screens as they stand. Presents. */
void home_draw(void);
/* The menu's loop; never returns. */
void home_run(void);

#endif
