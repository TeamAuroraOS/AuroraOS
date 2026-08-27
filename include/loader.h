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

/* App-launch scratch (used by the AuroraOS Home Menu, src/os/os_main.c).
 * The running OS lives at AOS_ARM9_LOAD_ADDR, so it cannot copy an app onto
 * itself in place. Instead it stages the app payload here, relocates a small
 * copy+jump stub clear of both regions, and the stub does the final copy. */
#define AURORA_APP_STAGE_ADDR       0x24000000u /* staged app ARM9 payload   */
#define AURORA_APP_TRAMPOLINE_ADDR  0x25000000u /* relocated copy+jump stub  */

/* HOME-button return contract. Before launching an app the Home Menu snapshots
 * its own image to AURORA_OS_SNAPSHOT_ADDR and installs a persistent return
 * stub at AURORA_RETURN_STUB_ADDR. An app's runtime polls the MCU for a HOME
 * press and branches to that stub, which restores the OS image and restarts it.
 * A magic word in the descriptor gates this: it is only valid when the Home
 * Menu installed it (an app booted directly as AURORAOS.BIN has no OS to return
 * to, so HOME is ignored). These addresses are a shared contract between
 * AuroraOS and the Auric runtime -- both must agree. */
#define AURORA_RETURN_STUB_ADDR     0x25004000u /* persistent return stub    */
#define AURORA_RETURN_DESC_ADDR     0x25008000u /* [magic, os_image_size]    */
#define AURORA_OS_SNAPSHOT_ADDR     0x26000000u /* saved OS image for return */
#define AURORA_RETURN_READY_MAGIC   0x52544E31u /* "RTN1": return installed  */

/* Per-app icon block. Every Auric app payload begins with a small header the
 * Home Menu reads to show a custom icon: a branch to the real entry, then this
 * 8-byte magic at payload offset 4, then a 32x32 1bpp icon (ICON_SIZE bytes)
 * at payload offset 12. */
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
