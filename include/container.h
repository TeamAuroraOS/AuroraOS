#ifndef AURORA_CONTAINER_H
#define AURORA_CONTAINER_H

#include "ff.h"
#include "loader.h"

typedef enum {
  AURORA_OK = 0,
  AURORA_ERR_READ,
  AURORA_ERR_MAGIC,    /* magic is neither "AOS1" nor "AUR1" */
  AURORA_ERR_PAYLOAD,
} aurora_status_t;

/* Reads the 36-byte header at the file's current position and checks the
 * magic. */
aurora_status_t aurora_parse_header(FIL *fp, aos_header_t *hdr);

/* Reads the ARM9 payload described by `hdr` into `dst`: the firm passes the
 * load address, the Home Menu a staging buffer, since it cannot overwrite
 * itself. */
aurora_status_t aurora_load_arm9(FIL *fp, const aos_header_t *hdr, void *dst);

#endif
