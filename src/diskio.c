#include "ff.h"
#include "diskio.h"
#include "sdmmc.h"

/* Cached drive status. STA_NOINIT until disk_initialize() succeeds. */
static DSTATUS sd_status = STA_NOINIT;

DSTATUS disk_status(BYTE pdrv) {
  if (pdrv != 0)
    return STA_NOINIT;
  return sd_status;
}

DSTATUS disk_initialize(BYTE pdrv) {
  if (pdrv != 0)
    return STA_NOINIT;

  if (sdmmc_sdcard_init() == 0)
    sd_status = 0; /* ready */
  else
    sd_status = STA_NOINIT | STA_NODISK;

  return sd_status;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count) {
  if (pdrv != 0)
    return RES_PARERR;
  if (sd_status & STA_NOINIT)
    return RES_NOTRDY;

  /* LBA_t is 32-bit here (exFAT/LBA64 disabled), matching the driver. */
  if (sdmmc_sdcard_readsectors((u32)sector, (u32)count, buff) == 0)
    return RES_OK;
  return RES_ERROR;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count) {
  if (pdrv != 0)
    return RES_PARERR;
  if (sd_status & STA_NOINIT)
    return RES_NOTRDY;

  if (sdmmc_sdcard_writesectors((u32)sector, (u32)count, buff) == 0)
    return RES_OK;
  return RES_ERROR;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff) {
  if (pdrv != 0)
    return RES_PARERR;

  switch (cmd) {
    case CTRL_SYNC: /* nothing buffered -- always in sync */
      return RES_OK;
    case GET_SECTOR_SIZE:
      *(WORD *)buff = 512;
      return RES_OK;
    case GET_SECTOR_COUNT:
      *(LBA_t *)buff = (LBA_t)sdmmc_sdcard_size();
      return RES_OK;
    case GET_BLOCK_SIZE:
      *(DWORD *)buff = 1; /* erase block size unknown -> 1 */
      return RES_OK;
    default:
      return RES_PARERR;
  }
}
