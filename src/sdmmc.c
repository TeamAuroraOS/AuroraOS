#include "sdmmc.h"

static mmcdevice handleSD;

/* Coarse millisecond wait, enough for the controller polling loops. */
static void sdmmc_wait_ms(u32 ms) {
    while (ms--)
        delay(40000);
}

static void setckl(u32 data) {
    sdmmc_write16(REG_SDCLKCTL, data & 0xFF);
    sdmmc_write16(REG_SDCLKCTL, (1u << 8) | (data & 0x2FF));
}

static int get_error(mmcdevice *ctx) {
    return (int)((ctx->error << 29) >> 31);
}

static void set_target(mmcdevice *ctx) {
    sdmmc_mask16(REG_SDPORTSEL, 0x3, (u16)ctx->devicenumber);
    setckl(ctx->clk);
    if (ctx->SDOPT == 0)
        sdmmc_mask16(REG_SDOPT, 0, 0x8000); /* 1-bit bus */
    else
        sdmmc_mask16(REG_SDOPT, 0x8000, 0); /* 4-bit bus */
}

/* Send an SD command and copy any read/write data through the FIFO. */
static void sdmmc_send_command(mmcdevice *ctx, u32 cmd, u32 args) {
    const bool getSDRESP = (cmd << 15) >> 31;
    u16 flags = (u16)((cmd << 15) >> 31);
    const bool readdata = cmd & 0x20000;
    const bool writedata = cmd & 0x40000;

    if (readdata || writedata)
        flags |= TMIO_STAT0_DATAEND;

    ctx->error = 0;
    while (sdmmc_read16(REG_SDSTATUS1) & TMIO_STAT1_CMD_BUSY)
        ; /* wait for the controller to be idle */
    sdmmc_write16(REG_SDIRMASK0, 0);
    sdmmc_write16(REG_SDIRMASK1, 0);
    sdmmc_write16(REG_SDSTATUS0, 0);
    sdmmc_write16(REG_SDSTATUS1, 0);
    sdmmc_mask16(REG_DATACTL32, 0x1800, 0x400); /* clear FIFO, disable 32-bit IRQs */
    sdmmc_write16(REG_SDCMDARG0, args & 0xFFFF);
    sdmmc_write16(REG_SDCMDARG1, args >> 16);
    sdmmc_write16(REG_SDCMD, cmd & 0xFFFF);

    u32 size = ctx->size;
    const u16 blkSize = sdmmc_read16(REG_SDBLKLEN32);
    u32 *rDataPtr32 = (u32 *)(void *)ctx->rData;
    u8 *rDataPtr8 = ctx->rData;
    const u32 *tDataPtr32 = (const u32 *)(const void *)ctx->tData;
    const u8 *tDataPtr8 = ctx->tData;

    bool rUseBuf = (NULL != rDataPtr32);
    bool tUseBuf = (NULL != tDataPtr32);

    u16 status0 = 0;
    while (1) {
        volatile u16 status1 = sdmmc_read16(REG_SDSTATUS1);
        volatile u16 ctl32 = sdmmc_read16(REG_DATACTL32);
        if (ctl32 & 0x100) { /* RX32RDY: a block is waiting in the FIFO */
            if (readdata && rUseBuf) {
                sdmmc_mask16(REG_SDSTATUS1, TMIO_STAT1_RXRDY, 0);
                if (size >= blkSize) {
                    if (!((u32)rDataPtr32 & 3)) {
                        for (u32 i = 0; i < blkSize; i += 4)
                            *rDataPtr32++ = sdmmc_read32(REG_SDFIFO32);
                    } else { /* unaligned destination */
                        for (u32 i = 0; i < blkSize; i += 4) {
                            u32 data = sdmmc_read32(REG_SDFIFO32);
                            *rDataPtr8++ = data;
                            *rDataPtr8++ = data >> 8;
                            *rDataPtr8++ = data >> 16;
                            *rDataPtr8++ = data >> 24;
                        }
                    }
                    size -= blkSize;
                }
            }
            sdmmc_mask16(REG_DATACTL32, 0x800, 0);
        }
        if (!(ctl32 & 0x200)) { /* TX32RQ: room to push a block */
            if (writedata && tUseBuf) {
                sdmmc_mask16(REG_SDSTATUS1, TMIO_STAT1_TXRQ, 0);
                if (size >= blkSize) {
                    if (!((u32)tDataPtr32 & 3)) {
                        for (u32 i = 0; i < blkSize; i += 4)
                            sdmmc_write32(REG_SDFIFO32, *tDataPtr32++);
                    } else {
                        for (u32 i = 0; i < blkSize; i += 4) {
                            u32 data = *tDataPtr8++;
                            data |= (u32)*tDataPtr8++ << 8;
                            data |= (u32)*tDataPtr8++ << 16;
                            data |= (u32)*tDataPtr8++ << 24;
                            sdmmc_write32(REG_SDFIFO32, data);
                        }
                    }
                    size -= blkSize;
                }
            }
            sdmmc_mask16(REG_DATACTL32, 0x1000, 0);
        }
        if (status1 & TMIO_MASK_GW) { /* a hard error bit -> abort */
            ctx->error |= 4;
            break;
        }
        if (!(status1 & TMIO_STAT1_CMD_BUSY)) {
            status0 = sdmmc_read16(REG_SDSTATUS0);
            if (sdmmc_read16(REG_SDSTATUS0) & TMIO_STAT0_CMDRESPEND)
                ctx->error |= 0x1;
            if (status0 & TMIO_STAT0_DATAEND)
                ctx->error |= 0x2;
            if ((status0 & flags) == flags)
                break;
        }
    }
    ctx->stat0 = sdmmc_read16(REG_SDSTATUS0);
    ctx->stat1 = sdmmc_read16(REG_SDSTATUS1);
    sdmmc_write16(REG_SDSTATUS0, 0);
    sdmmc_write16(REG_SDSTATUS1, 0);

    if (getSDRESP) {
        ctx->ret[0] = sdmmc_read16(REG_SDRESP0) | (sdmmc_read16(REG_SDRESP1) << 16);
        ctx->ret[1] = sdmmc_read16(REG_SDRESP2) | (sdmmc_read16(REG_SDRESP3) << 16);
        ctx->ret[2] = sdmmc_read16(REG_SDRESP4) | (sdmmc_read16(REG_SDRESP5) << 16);
        ctx->ret[3] = sdmmc_read16(REG_SDRESP6) | (sdmmc_read16(REG_SDRESP7) << 16);
    }
}

static u32 sdmmc_calc_size(u8 *csd, int type) {
    u32 result = 0;
    if (type == -1)
        type = csd[14] >> 6;
    switch (type) {
        case 0: {
            u32 block_len = csd[9] & 0xf;
            block_len = 1u << block_len;
            u32 mult = (u32)((csd[4] >> 7) | ((csd[5] & 3) << 1));
            mult = 1u << (mult + 2);
            result = csd[8] & 3;
            result = (result << 8) | csd[7];
            result = (result << 2) | (csd[6] >> 6);
            result = (result + 1) * mult * block_len / 512;
        } break;
        case 1:
            result = csd[7] & 0x3f;
            result = (result << 8) | csd[6];
            result = (result << 8) | csd[5];
            result = (result + 1) * 1024;
            break;
        default:
            break;
    }
    return result;
}

static void sdmmc_controller_init(void) {
    handleSD.isSDHC = 0;
    handleSD.SDOPT = 0;
    handleSD.initarg = 0;
    handleSD.clk = 0x20; /* ~523 KHz for identification */
    handleSD.devicenumber = 0;

    /* Raw controller configuration -- magic values straight from GodMode9. */
    *(vu16 *)0x10006100 &= 0xF7FFu;
    *(vu16 *)0x10006100 &= 0xEFFFu;
    *(vu16 *)0x10006100 |= 0x402u;
    *(vu16 *)0x100060D8 = (*(vu16 *)0x100060D8 & 0xFFDD) | 2;
    *(vu16 *)0x10006100 &= 0xFFFFu;
    *(vu16 *)0x100060D8 &= 0xFFDFu;
    *(vu16 *)0x10006104 = 512; /* SDBLKLEN32 */
    *(vu16 *)0x10006108 = 1;   /* SDBLKCOUNT32 */
    *(vu16 *)0x100060E0 &= 0xFFFEu;
    *(vu16 *)0x100060E0 |= 1u;
    *(vu16 *)0x10006020 |= (u16)TMIO_MASK_ALL;
    *(vu16 *)0x10006022 |= (u16)(TMIO_MASK_ALL >> 16);
    *(vu16 *)0x100060FC |= 0xDBu;
    *(vu16 *)0x100060FE |= 0xDBu;
    *(vu16 *)0x10006002 &= 0xFFFCu;
    *(vu16 *)0x10006024 = 0x20;
    *(vu16 *)0x10006028 = 0x40E9;
    *(vu16 *)0x10006002 &= 0xFFFCu;
    *(vu16 *)0x10006026 = 512; /* SDBLKLEN */
    *(vu16 *)0x10006008 = 0;   /* SDSTOP */
}

/* from GodMode9 sdmmc.c (SD_Init). Runs the SD identification/selection flow:
   CMD0 -> CMD8 -> ACMD41 loop -> CMD2 -> CMD3 -> CMD9 -> CMD7 -> ACMD6 ->
   (optional CMD6 high-speed) -> CMD13 -> CMD16. Returns 0 on success, negative
   at the stage that failed. */
static int SD_Init(void) {
    handleSD.isSDHC = 0;
    handleSD.SDOPT = 0;
    handleSD.initarg = 0;
    handleSD.clk = 0x20;
    handleSD.devicenumber = 0;

    /* Need at least 74 SD clocks before the first command. */
    set_target(&handleSD);
    delay(20000);

    sdmmc_send_command(&handleSD, 0, 0);              /* CMD0  GO_IDLE_STATE */
    sdmmc_send_command(&handleSD, 0x10408, 0x1AA);    /* CMD8  SEND_IF_COND  */
    u32 temp = (handleSD.error & 0x1) << 0x1E;

    u32 temp2 = 0;
    do {
        do {
            sdmmc_send_command(&handleSD, 0x10437, handleSD.initarg << 0x10); /* CMD55 */
            sdmmc_send_command(&handleSD, 0x10769, 0x10100000 | temp);        /* ACMD41 */
            temp2 = 1;
        } while (!(handleSD.error & 1));
    } while ((handleSD.ret[0] & 0x80000000) == 0); /* wait until powered up */

    if (!((handleSD.ret[0] >> 30) & 1) || !temp)
        temp2 = 0;
    handleSD.isSDHC = temp2;

    sdmmc_send_command(&handleSD, 0x10602, 0);       /* CMD2  ALL_SEND_CID */
    if (handleSD.error & 0x4) return -1;

    sdmmc_send_command(&handleSD, 0x10403, 0);       /* CMD3  SEND_RELATIVE_ADDR */
    if (handleSD.error & 0x4) return -2;
    handleSD.initarg = handleSD.ret[0] >> 0x10;      /* remember the RCA */

    sdmmc_send_command(&handleSD, 0x10609, handleSD.initarg << 0x10); /* CMD9 SEND_CSD */
    if (handleSD.error & 0x4) return -3;

    const bool cmd6Supported = ((u8 *)handleSD.ret)[10] & 0x40; /* command class 10 */
    handleSD.total_size = sdmmc_calc_size((u8 *)&handleSD.ret[0], -1);
    setckl(0x201); /* ~16.7 MHz */

    sdmmc_send_command(&handleSD, 0x10507, handleSD.initarg << 0x10); /* CMD7 SELECT */
    if (handleSD.error & 0x4) return -4;

    sdmmc_send_command(&handleSD, 0x10437, handleSD.initarg << 0x10); /* CMD55 */
    if (handleSD.error & 0x4) return -5;
    sdmmc_send_command(&handleSD, 0x1076A, 0x0);     /* ACMD42 clear card-detect pull-up */
    if (handleSD.error & 0x4) return -6;

    sdmmc_send_command(&handleSD, 0x10437, handleSD.initarg << 0x10); /* CMD55 */
    if (handleSD.error & 0x4) return -7;
    handleSD.SDOPT = 1;
    sdmmc_send_command(&handleSD, 0x10446, 0x2);     /* ACMD6 SET_BUS_WIDTH (4-bit) */
    if (handleSD.error & 0x4) return -8;
    sdmmc_mask16(REG_SDOPT, 0x8000, 0);              /* controller to 4-bit */

    if (cmd6Supported) { /* CMD6 SWITCH_FUNC -> high speed */
        sdmmc_write16(REG_SDSTOP, 0);
        sdmmc_write16(REG_SDBLKLEN32, 64);
        sdmmc_write16(REG_SDBLKLEN, 64);
        handleSD.rData = NULL;
        handleSD.size = 64;
        sdmmc_send_command(&handleSD, 0x31C06, 0x80FFFFF1);
        sdmmc_write16(REG_SDBLKLEN, 512);
        if (handleSD.error & 0x4) return -9;

        handleSD.clk = 0x200; /* ~33.5 MHz */
        setckl(0x200);
    } else {
        handleSD.clk = 0x201; /* ~16.7 MHz */
    }

    sdmmc_send_command(&handleSD, 0x1040D, handleSD.initarg << 0x10); /* CMD13 SEND_STATUS */
    if (handleSD.error & 0x4) return -10;

    sdmmc_send_command(&handleSD, 0x10410, 0x200);   /* CMD16 SET_BLOCKLEN 512 */
    if (handleSD.error & 0x4) return -11;

    return 0;
}

int sdmmc_sdcard_readsectors(u32 sector_no, u32 numsectors, u8 *out) {
    /* from GodMode9 sdmmc.c -- byte-address non-SDHC cards, sector-address SDHC. */
    if (handleSD.isSDHC == 0)
        sector_no <<= 9;
    set_target(&handleSD);
    sdmmc_write16(REG_SDSTOP, 0x100);
    sdmmc_write16(REG_SDBLKCOUNT32, (u16)numsectors);
    sdmmc_write16(REG_SDBLKLEN32, 0x200);
    sdmmc_write16(REG_SDBLKCOUNT, (u16)numsectors);
    handleSD.rData = out;
    handleSD.size = numsectors << 9;
    sdmmc_send_command(&handleSD, 0x33C12, sector_no); /* CMD18 READ_MULTIPLE_BLOCK */
    return get_error(&handleSD);
}

int sdmmc_sdcard_readsector(u32 sector_no, u8 *out) {
    return sdmmc_sdcard_readsectors(sector_no, 1, out);
}

int sdmmc_sdcard_writesectors(u32 sector_no, u32 numsectors, const u8 *in) {
    /* Mirror of readsectors using CMD25 WRITE_MULTIPLE_BLOCK. Non-SDHC cards
       are byte-addressed, SDHC cards are sector-addressed. -- from GodMode9. */
    if (handleSD.isSDHC == 0)
        sector_no <<= 9;
    set_target(&handleSD);
    sdmmc_write16(REG_SDSTOP, 0x100);
    sdmmc_write16(REG_SDBLKCOUNT32, (u16)numsectors);
    sdmmc_write16(REG_SDBLKLEN32, 0x200);
    sdmmc_write16(REG_SDBLKCOUNT, (u16)numsectors);
    handleSD.tData = in;
    handleSD.size = numsectors << 9;
    sdmmc_send_command(&handleSD, 0x52C19, sector_no); /* CMD25 WRITE_MULTIPLE_BLOCK */
    return get_error(&handleSD);
}

u32 sdmmc_sdcard_size(void) {
    return handleSD.total_size;
}

int sdmmc_sdcard_init(void) {
    /* "SD mount fix": CFG register that routes the SD card to the ARM9
       controller. -- from GodMode9 sdmmc_sdcard_init() */
    *((vu16 *)0x10000020) = 0x340;

    sdmmc_controller_init();

    /* Wait for the card to signal it is present/ready (~100 ms max). */
    u32 timeout = 50;
    do {
        if (sdmmc_read16(REG_SDSTATUS0) & TMIO_STAT0_SIGSTATE)
            break;
        sdmmc_wait_ms(2);
    } while (--timeout);
    if (timeout == 0)
        return -100; /* no card detected */

    return SD_Init();
}
