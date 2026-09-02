#ifndef AURORA_LOADER_H
#define AURORA_LOADER_H

#include <stdint.h>

#define AOS_MAGIC "AOS1" /* first 4 bytes of the file, not NUL-terminated */
#define AOS_ARM9_LOAD_ADDR  0x22000000u /* FCRAM, 32 MB clear of ARM11 slot */
#define AOS_ARM11_LOAD_ADDR 0x24000000u /* FCRAM */
#define AOS_ARM11_MAILBOX   0x27000000u
#define AURORA_APP_STAGE_ADDR       0x24000000u /* staged app ARM9 payload   */
#define AURORA_APP_TRAMPOLINE_ADDR  0x25000000u /* relocated copy+jump stub  */
#define AURORA_RETURN_STUB_ADDR     0x25004000u /* persistent return stub    */
#define AURORA_RETURN_DESC_ADDR     0x25008000u /* [magic, os_image_size]    */
#define AURORA_OS_SNAPSHOT_ADDR     0x26000000u /* saved OS image for return */
#define AURORA_RETURN_READY_MAGIC   0x52544E31u /* "RTN1": return installed  */
#define AURORA_ICON_MAGIC           "AURICON1"  /* 8 bytes, not NUL-terminated */
#define AURORA_ICON_MAGIC_OFFSET    4u          /* bytes into the ARM9 payload */
#define AURORA_ICON_DATA_OFFSET     12u
#define AURORA_ICON_BYTES           128u        /* 32 rows * 4 bytes/row       */

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
