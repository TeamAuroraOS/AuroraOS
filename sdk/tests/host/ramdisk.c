/* The SD card as a RAM disk under the real FatFs (diskio.h), formatted with
 * f_mkfs, which the harness's copy of ffconf.h turns on. */
#include "ff.h"
#include "diskio.h"
#include "hostrt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static BYTE *g_disk;
static LBA_t g_sectors;

void host_ramdisk(uint32_t mb) {
  static BYTE work[FF_MAX_SS * 8];
  static const MKFS_PARM opt = {FM_FAT32, 0, 0, 0, 0};
  FATFS fs;
  free(g_disk);
  g_sectors = (LBA_t)mb * 2048u;
  g_disk = calloc(g_sectors, 512u);
  if (!g_disk || f_mkfs("0:", &opt, work, sizeof(work)) != FR_OK) {
    printf("ramdisk: f_mkfs failed\n");
    exit(2);
  }
  /* A check that it mounts; the code under test mounts it itself. */
  if (f_mount(&fs, "0:", 1) != FR_OK) {
    printf("ramdisk: f_mount failed\n");
    exit(2);
  }
  f_mount(NULL, "0:", 0);
}

DSTATUS disk_status(BYTE pdrv) { return pdrv == 0 && g_disk ? 0 : STA_NOINIT; }
DSTATUS disk_initialize(BYTE pdrv) { return disk_status(pdrv); }

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count) {
  if (pdrv || !g_disk || sector + count > g_sectors)
    return RES_PARERR;
  memcpy(buff, g_disk + sector * 512u, count * 512u);
  return RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count) {
  if (pdrv || !g_disk || sector + count > g_sectors)
    return RES_PARERR;
  memcpy(g_disk + sector * 512u, buff, count * 512u);
  return RES_OK;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff) {
  if (pdrv || !g_disk)
    return RES_PARERR;
  switch (cmd) {
  case CTRL_SYNC:
    return RES_OK;
  case GET_SECTOR_COUNT:
    *(LBA_t *)buff = g_sectors;
    return RES_OK;
  case GET_SECTOR_SIZE:
    *(WORD *)buff = 512;
    return RES_OK;
  case GET_BLOCK_SIZE:
    *(DWORD *)buff = 1;
    return RES_OK;
  default:
    return RES_PARERR;
  }
}
