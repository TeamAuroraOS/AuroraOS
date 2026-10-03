#ifndef AURORA_HOMELAYOUT_H
#define AURORA_HOMELAYOUT_H

#include "aurora.h"

/* How the Home Menu is arranged: Home and its folders, each an ordered list
 * shown fifteen to a page. See docs/home.md. */

#define HOME_COLS     5
#define HOME_ROWS     3
#define HOME_PER_PAGE (HOME_COLS * HOME_ROWS)
#define HOME_PAGES    12
#define HOME_ITEMS    (HOME_PER_PAGE * HOME_PAGES) /* in Home, or in a folder */
#define HOME_FOLDERS  64
#define HOME_NAME     25 /* a folder's name, with its NUL */
/* Home > folder > folder, and no further. */
#define HOME_DEPTH    2

#define HOME_FILE "0:/Aurora/HomeMenu.txt"

/* A container is Home or a folder's number. */
#define HOME_ROOT (-1)

/* An entry is an app's index (0 up) or a folder. */
#define HL_NONE         (-1)
#define HL_FOLDER(f)    (-2 - (f))
#define HL_IS_FOLDER(e) ((e) <= -2)
#define HL_FOLDER_ID(e) (-2 - (e))

enum {
  HL_OK = 0,
  HL_FULL = -1,
  HL_TOO_DEEP = -2,   /* it would put a folder three deep */
  HL_NO_FOLDERS = -3, /* all HOME_FOLDERS are in use */
  HL_INSIDE = -4,     /* a folder into itself */
};

/* Every one of `napps` apps in Home, in order, and no folders. */
void hl_reset(int napps);

int hl_count(int c);
int hl_at(int c, int i);
/* Where `e` is in `c`, or -1. */
int hl_index(int c, int e);
/* Home 0, a folder in Home 1, a folder in that 2. */
int hl_depth(int c);
/* 0 for an app, 1 for a folder, 2 for a folder holding a folder. */
int hl_height(int e);
int hl_parent(int f);
const char *hl_name(int f);
int hl_folders_free(void);

/* HL_OK if `e`, held by no container, may go into `c`. */
int hl_fits(int c, int e);
/* Takes the entry at `i` out of `c`: it is then held nowhere until put. */
int hl_take(int c, int i);
/* Puts `e` at `i` in `c` (clamped to the end), if it fits. */
int hl_put(int c, int i, int e);

/* A new, empty folder at `i` in `c`: its number, or an HL_ error. */
int hl_new_folder(int c, int i, const char *name);
/* The entry at `i` in `c` moved into a new folder that takes its place. */
int hl_wrap(int c, int i, const char *name);
/* `e`, held nowhere, and the entry at `i` in `c` together in a new folder
 * that takes that entry's place: the folder's number, or an HL_ error, with
 * nothing changed. */
int hl_merge(int c, int i, int e, const char *name);
/* Removes folder `f`; what it held goes to its parent, where it was. */
int hl_unfold(int f);
void hl_rename(int f, const char *name);

/* HOME_FILE on the card, mounting it for the call. `keys[i]` names app i.
 * Apps the file does not place go at the end of Home. 1 if the file was
 * read. */
int hl_load(const char *const *keys, int napps);
int hl_save(const char *const *keys, int napps);

#endif
