#ifndef AURORA_LOADER_H
#define AURORA_LOADER_H

#include <stdint.h>

/* AuroraOS bootable container format ("AOS1"). */

#define AOS_MAGIC "AOS1" /* first 4 bytes of the file, not NUL-terminated */

/* Loader memory ranges are kept clear of payload loads. */
#define AOS_ARM9_LOAD_ADDR  0x22000000u /* FCRAM, 32 MB clear of ARM11 slot */
#define AOS_ARM11_LOAD_ADDR 0x24000000u /* FCRAM */

/* ARM11 wakeup mailbox. Must match src/arm11_start.s. */
#define AOS_ARM11_MAILBOX   0x27000000u

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

_Static_assert(sizeof(aos_header_t) == 36,
               "aos_header_t must be 36 bytes to match tools/aos_pack.py");

/* Load AURORAOS.BIN from the SD card and jump into the ARM9 payload. */
void boot_aurora(void);

/* ARM9 cache-flush + branch stub. */
void aurora_jump_arm9(uint32_t entry);

#endif /* AURORA_LOADER_H */
