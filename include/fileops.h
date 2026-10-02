#ifndef AURORA_FILEOPS_H
#define AURORA_FILEOPS_H

#include "aurora.h"

/* File Explorer operations on a mounted card. Paths are FatFs paths in UTF-8,
 * at most FO_PATH bytes including the NUL. */

#define FO_PATH  1024
#define FO_DEPTH 24 /* folder nesting a copy or delete will walk into */

typedef enum {
  FO_OK = 0,
  FO_CANCELLED,
  FO_EXISTS,    /* the destination name is taken                  */
  FO_INTO_SELF, /* a folder into itself or one of its own folders */
  FO_TOO_DEEP,  /* nested past FO_DEPTH, or a path past FO_PATH   */
  FO_FULL,      /* the card ran out of space                      */
  FO_GONE,      /* the source no longer exists                    */
  FO_BAD_NAME,  /* FatFs refused the name                         */
  FO_DENIED,    /* read-only, or a folder that is not empty       */
  FO_FAILED,    /* any other error                                */
} FoResult;

/* Called as the work goes: for a copy with the file being copied and its
 * bytes done and total, for a delete with each item and the count so far (total
 * 0). Returning 0 cancels. */
typedef int (*FoProgress)(const char *name, u32 done, u32 total);

/* A file, or a folder with everything in it. A cancelled delete leaves what it
 * had not reached yet. */
FoResult fo_delete(const char *path, FoProgress cb);

/* `dst` must not exist. A copy that fails or is cancelled removes what it made. */
FoResult fo_copy(const char *src, const char *dst, FoProgress cb);

/* Moves or renames within the card; `dst` must not exist, except as the same
 * item under a different letter case. */
FoResult fo_move(const char *src, const char *dst);

FoResult fo_mkdir(const char *path);

/* When `path` is taken, rewrites its last part as "name (2).ext", "(3)" and so
 * on, until one is free. Returns 0 if none is. Folders keep dots in the name. */
int fo_unique(char *path, int is_dir);

/* Whether `path` is `folder` or somewhere inside it, ignoring letter case. */
int fo_within(const char *path, const char *folder);

const char *fo_error(FoResult r);

#endif
