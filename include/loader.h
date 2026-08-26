#ifndef AURORA_LOADER_H
#define AURORA_LOADER_H

#include <stdint.h>

/*
 * AuroraOS bootable container format ("AOS1").
 *
 * A packaged payload is simply:
 *     [ aos_header_t ][ arm9 payload ][ arm11 payload ]
 *
 * The header records where each payload sits in the file (offset/size) and
 * where it must be copied and jumped to in memory. This layout MUST stay in
 * sync with the host packer, tools/aos_pack.py.
 */

#define AOS_MAGIC "AOS1" /* first 4 bytes of the file, not NUL-terminated */

/*
 * Phase 4 -- load addresses for a booted payload.
 *
 * The running loader lives in ARM9 internal RAM (0x08006800..0x08100000, see
 * arm9.ld) and ARM11 AXIWRAM (0x1FF80000..0x20000000, see arm11.ld). FCRAM
 * (0x20000000+) is untouched by the loader, so payloads are copied there. These
 * must match the defaults in tools/aos_pack.py and the test payload's linker
 * script. The loader still copies to whatever the packed header says; these are
 * the known-good values we pack with.
 */
#define AOS_ARM9_LOAD_ADDR  0x22000000u /* FCRAM, 32 MB clear of ARM11 slot */
#define AOS_ARM11_LOAD_ADDR 0x24000000u /* FCRAM */

typedef struct {
  char magic[4];          /* "AOS1" */
  uint32_t arm9_offset;   /* byte offset of the ARM9 payload within the file */
  uint32_t arm9_size;     /* ARM9 payload size in bytes */
  uint32_t arm9_load_addr; /* physical address to copy the ARM9 payload to */
  uint32_t arm9_entry;    /* ARM9 address to branch to */
  uint32_t arm11_offset;  /* byte offset of the ARM11 payload (0 if none) */
  uint32_t arm11_size;    /* ARM11 payload size in bytes (0 if none) */
  uint32_t arm11_load_addr;
  uint32_t arm11_entry;
} aos_header_t;

/* The packer writes exactly this many bytes for the header. */
_Static_assert(sizeof(aos_header_t) == 36,
               "aos_header_t must be 36 bytes to match tools/aos_pack.py");

/* Load "AURORAOS.BIN" from the SD card, copy its payloads into place and jump.
   (Implemented in Phase 6.) The 8.3 file name is required because FatFs is
   built with long file names disabled. */
void boot_aurora(void);

#endif /* AURORA_LOADER_H */
