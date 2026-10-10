#ifndef AURORA_CONTAINER_H
#define AURORA_CONTAINER_H

#include "ff.h"
#include "loader.h"

typedef enum {
  AURORA_OK = 0,
  AURORA_ERR_READ,
  AURORA_ERR_MAGIC,    /* magic is not "AOS1", "AUR1" or "AURC" */
  AURORA_ERR_PAYLOAD,
} aurora_status_t;

/* What the first four bytes of a file make it: 'A' an OS image ("AOS1"),
 * 'U' an Auric app ("AUR1"), 'C' a C app ("AURC", sdk/), 0 none of them. */
int aurora_magic_kind(const char *magic);

/* Reads the 36-byte header at the file's current position and checks the
 * magic. */
aurora_status_t aurora_parse_header(FIL *fp, aos_header_t *hdr);

/* Reads the ARM9 payload described by `hdr` into `dst`: the firm passes the
 * load address, the Home Menu a staging buffer, since it cannot overwrite
 * itself. */
aurora_status_t aurora_load_arm9(FIL *fp, const aos_header_t *hdr, void *dst);

/* The folder a C app keeps its data in, "Aurora/Apps/C/<name>" for the app
 * file "<name>.bin" at `path`, wherever that is. 0 if the path names no file
 * or `out` is too small. */
int aurora_c_data_dir(const char *path, char *out, unsigned max);

/* Makes that folder, and the ones above it; folders already there are fine.
 * The OS does this before it starts a C app. */
int aurora_c_data_make(const char *path);

#endif
