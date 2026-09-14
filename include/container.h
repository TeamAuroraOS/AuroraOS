#ifndef AURORA_CONTAINER_H
#define AURORA_CONTAINER_H

#include "ff.h"
#include "loader.h"

typedef enum {
  AURORA_OK = 0,
  AURORA_ERR_READ,     /* header could not be read */
  AURORA_ERR_MAGIC,    /* magic is neither "AOS1" nor "AUR1" */
  AURORA_ERR_PAYLOAD,  /* ARM9 payload could not be read */
} aurora_status_t;

/* Read the 36-byte header from `fp` (at its current position) into `hdr` and
 * validate the magic. Returns AURORA_OK on success. */
aurora_status_t aurora_parse_header(FIL *fp, aos_header_t *hdr);

/* Reads the ARM9 payload described by `hdr` into `dst`: the firm passes the
 * load address, the Home Menu a staging buffer, since it cannot overwrite
 * itself. */
aurora_status_t aurora_load_arm9(FIL *fp, const aos_header_t *hdr, void *dst);

#endif
