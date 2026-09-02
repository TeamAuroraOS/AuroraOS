/*
 * Minimal read-only SD card driver (ARM9).
 *
 * Register map, TMIO status bits, command encodings and the init/read
 * sequences are referenced from GodMode9's SD driver
 * (arm9/source/nand/sdmmc.{c,h}), originally by Normmatt (c) 2014-2015,
 * MPL-2.0 / GPLv2. This is a trimmed, SD-only rewrite adapted to AuroraOS's
 * MMIO helpers -- NAND/eMMC and the write path were dropped because the loader
 * only needs to read blocks off the SD card.
 *   Reference: https://github.com/d0k3/GodMode9
 */
#ifndef AURORA_SDMMC_H
#define AURORA_SDMMC_H

#include "aurora.h"
#include <stdbool.h>

typedef volatile u16 vu16;
typedef volatile u32 vu32;

/* SD/MMC controller 1 (the physical SD card slot). -- from GodMode9 */
#define SDMMC_BASE        (0x10006000)

/* 16-bit register offsets from SDMMC_BASE. -- from GodMode9 */
#define REG_SDCMD         (0x00)
#define REG_SDPORTSEL     (0x02)
#define REG_SDCMDARG0     (0x04)
#define REG_SDCMDARG1     (0x06)
#define REG_SDSTOP        (0x08)
#define REG_SDBLKCOUNT    (0x0a)
#define REG_SDRESP0       (0x0c)
#define REG_SDRESP1       (0x0e)
#define REG_SDRESP2       (0x10)
#define REG_SDRESP3       (0x12)
#define REG_SDRESP4       (0x14)
#define REG_SDRESP5       (0x16)
#define REG_SDRESP6       (0x18)
#define REG_SDRESP7       (0x1a)
#define REG_SDSTATUS0     (0x1c)
#define REG_SDSTATUS1     (0x1e)
#define REG_SDIRMASK0     (0x20)
#define REG_SDIRMASK1     (0x22)
#define REG_SDCLKCTL      (0x24)
#define REG_SDBLKLEN      (0x26)
#define REG_SDOPT         (0x28)
#define REG_SDFIFO        (0x30)
#define REG_DATACTL       (0xd8)
#define REG_SDRESET       (0xe0)
#define REG_DATACTL32     (0x100)
#define REG_SDBLKLEN32    (0x104)
#define REG_SDBLKCOUNT32  (0x108)
#define REG_SDFIFO32      (0x10C)

/* TMIO status-register bits. -- from GodMode9 */
#define TMIO_STAT0_CMDRESPEND     (0x0001)
#define TMIO_STAT0_DATAEND        (0x0004)
#define TMIO_STAT0_CARD_REMOVE    (0x0008)
#define TMIO_STAT0_CARD_INSERT    (0x0010)
#define TMIO_STAT0_SIGSTATE       (0x0020)
#define TMIO_STAT1_CMD_IDX_ERR    (0x0001)
#define TMIO_STAT1_CRCFAIL        (0x0002)
#define TMIO_STAT1_STOPBIT_ERR    (0x0004)
#define TMIO_STAT1_DATATIMEOUT    (0x0008)
#define TMIO_STAT1_RXOVERFLOW     (0x0010)
#define TMIO_STAT1_TXUNDERRUN     (0x0020)
#define TMIO_STAT1_CMDTIMEOUT     (0x0040)
#define TMIO_STAT1_RXRDY          (0x0100)
#define TMIO_STAT1_TXRQ           (0x0200)
#define TMIO_STAT1_ILL_FUNC       (0x2000)
#define TMIO_STAT1_CMD_BUSY       (0x4000)
#define TMIO_STAT1_ILL_ACCESS     (0x8000)

#define TMIO_MASK_ALL             (0x837F031D)

/* "Got worse" -- any error bit that aborts a command. -- from GodMode9 */
#define TMIO_MASK_GW (TMIO_STAT1_ILL_ACCESS | TMIO_STAT1_CMDTIMEOUT | \
                      TMIO_STAT1_TXUNDERRUN | TMIO_STAT1_RXOVERFLOW | \
                      TMIO_STAT1_DATATIMEOUT | TMIO_STAT1_STOPBIT_ERR | \
                      TMIO_STAT1_CRCFAIL | TMIO_STAT1_CMD_IDX_ERR)

/* SD controller context. Layout from GodMode9's mmcdevice. */
typedef struct mmcdevice {
    u8       *rData;
    const u8 *tData;
    u32       size;
    u32       error;
    u16       stat0;
    u16       stat1;
    u32       ret[4];
    u32       initarg;
    u32       isSDHC;
    u32       clk;
    u32       SDOPT;
    u32       devicenumber;
    u32       total_size; /* size in 512-byte sectors */
} mmcdevice;

/* Bring the SD card up. Returns 0 on success, negative on failure. */
int sdmmc_sdcard_init(void);

/* Read numsectors 512-byte sectors starting at sector_no into out.
   Returns 0 on success, non-zero on error. */
int sdmmc_sdcard_readsectors(u32 sector_no, u32 numsectors, u8 *out);
int sdmmc_sdcard_readsector(u32 sector_no, u8 *out);

/* Write numsectors 512-byte sectors starting at sector_no from in.
   Returns 0 on success, non-zero on error. */
int sdmmc_sdcard_writesectors(u32 sector_no, u32 numsectors, const u8 *in);

/* Total capacity of the SD card in 512-byte sectors (valid after init). */
u32 sdmmc_sdcard_size(void);

/* --- MMIO helpers (from GodMode9 sdmmc.h) --- */
static inline u16 sdmmc_read16(u16 reg) {
    return *(volatile u16 *)(SDMMC_BASE + reg);
}
static inline void sdmmc_write16(u16 reg, u16 val) {
    *(volatile u16 *)(SDMMC_BASE + reg) = val;
}
static inline u32 sdmmc_read32(u16 reg) {
    return *(volatile u32 *)(SDMMC_BASE + reg);
}
static inline void sdmmc_write32(u16 reg, u32 val) {
    *(volatile u32 *)(SDMMC_BASE + reg) = val;
}
static inline void sdmmc_mask16(u16 reg, u16 clear, u16 set) {
    u16 val = sdmmc_read16(reg);
    val &= ~clear;
    val |= set;
    sdmmc_write16(reg, val);
}

#endif /* AURORA_SDMMC_H */
