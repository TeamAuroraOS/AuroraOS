/* SPDX-License-Identifier: GPL-2.0 */
/*
 * AuroraOS Wi-Fi SDIO probe (ARM9 <-> ARM11). The chip is an Atheros AR6014 on a
 * 16-bit TMIO/SDHC host controller ("controller 2", logical 0x10122000) reached
 * from the ARM11. The probe runs in the ARM11 core and reports back through the
 * shared block below. See docs/wifi.md for the full state of the bring-up.
 *
 * LICENSE: This Wi-Fi code is licensed GPL-2.0 (not the rest of AuroraOS). It
 * derives register facts and the BMI/HIF bring-up sequence from the ath6kl
 * legacy driver as ported to the 3DS by Octoblimp. Credit: Octoblimp; ath6kl
 * (GPL-2.0). See docs/wifi.md "License and credits".
 */
#ifndef AURORA_WIFI_H
#define AURORA_WIFI_H

#include <stdint.h>

#define WIFI_SHARED_ADDR 0x233B0000u
#define WIFI_SDIO_BASE   0x10122000u /* logical; = physical 0x1EC22000 */

/* Firmware staging in shared FCRAM (past the 10 MB audio PCM buffer that ends at
 * 0x23E00000, before the app-launch stage at 0x24000000). The ARM9 loads the
 * copyright NWM blobs from SD into these slots and the ARM11 uploads them over
 * BMI. The blobs are Nintendo copyright: SD-loaded at runtime, never embedded. */
#define WIFI_FW_ADDR      0x23E00000u
#define WIFI_FW_MAGIC     0x46574631u /* "1FWF": ARM9 staged all four blobs */
#define WIFI_FW_STUBDATA (WIFI_FW_ADDR + 0x00001000u)
#define WIFI_FW_STUBCODE (WIFI_FW_ADDR + 0x00002000u)
#define WIFI_FW_DATABASE (WIFI_FW_ADDR + 0x00003000u)
#define WIFI_FW_MAIN4    (WIFI_FW_ADDR + 0x00010000u) /* ~42 KB, largest blob */

typedef struct {
  volatile uint32_t magic; /* WIFI_FW_MAGIC once the ARM9 has staged the blobs */
  volatile uint32_t stubdata_len;
  volatile uint32_t stubcode_len;
  volatile uint32_t database_len;
  volatile uint32_t main4_len;
} WifiFw;

/* Phase the ARM11 probe reached, so a hang localises to the last phase set. */
enum {
  WIFI_PH_NONE = 0,
  WIFI_PH_REGS = 1,
  WIFI_PH_CLK  = 2,
  WIFI_PH_CMD  = 3,
  WIFI_PH_DONE = 4,
};

#define WIFI_LOG_MAX 12
typedef struct {
  volatile uint16_t cmd;   /* command word low 16 bits (0 = empty slot) */
  volatile uint16_t ok;    /* 0 empty, 1 CMDRESPEND, 2 error/timeout */
  volatile uint32_t arg;
  volatile uint32_t resp;  /* RESP0 | RESP1<<16 */
  volatile uint16_t stat0;
  volatile uint16_t stat1;
} WifiCmd;

/* Extended controller registers (outside the 0x00..0x3e block) captured raw. */
enum {
  WIFI_EXT_D8 = 0, /* 0xD8  SD_DATACTL */
  WIFI_EXT_E0,     /* 0xE0  SD_RESET */
  WIFI_EXT_FC,     /* 0xFC */
  WIFI_EXT_FE,     /* 0xFE */
  WIFI_EXT_100,    /* 0x100 SD_DATACTL32 low */
  WIFI_EXT_102,    /* 0x102 SD_DATACTL32 high */
  WIFI_EXT_COUNT
};

typedef struct {
  volatile uint32_t phase;
  volatile uint32_t seq;
  volatile uint32_t timeouts;
  volatile uint32_t clk;
  volatile uint32_t nlog;
  WifiCmd log[WIFI_LOG_MAX];
  volatile uint16_t reg[32];  /* raw controller regs 0x00..0x3e */
  volatile uint16_t ext[WIFI_EXT_COUNT];
  volatile uint32_t sdmmcctl; /* CFG9 SDMMCCTL 0x10000020 (ARM9-filled) */
  volatile uint16_t gpio_before;
  volatile uint16_t gpio_after; /* GPIO_DATA4 0x10147028: bit0 = Wi-Fi reset */
  volatile uint32_t cis_addr;   /* common CIS pointer (CCCR 0x09-0x0B) */
  volatile uint8_t cis[48];
  volatile uint16_t manf;       /* CISTPL_MANFID: 0x0271 = Atheros */
  volatile uint16_t card;       /* CISTPL_MANFID: 0x0201 = AR6014 */
  volatile uint8_t ior;         /* CCCR 0x03 I/O Ready: bit1 = fn1 ready */
  volatile uint8_t hif[16];     /* function-1 HIF regs 0x400..0x40F */
  volatile int32_t bmi_wr;      /* BMI CMD53 write result (0 ok, neg err) */
  volatile int32_t bmi_rd;      /* BMI CMD53 read result */
  volatile uint32_t bmi_ver;    /* target version word */
  volatile uint32_t bmi_type;   /* target type word */
  volatile uint32_t bmi_look;   /* HOST_INT_STATUS(0x400)|COUNTER(0x403)<<8|... */
  volatile uint16_t bmi_s0;
  volatile uint16_t bmi_s1;
  volatile uint16_t bmi_ctl;
  volatile uint16_t bmi_idx;
  volatile uint32_t bmi_credit; /* BMI command credit (COUNT_DEC counter 1) */
  volatile uint8_t cnt[8];      /* COUNT regs 0x420..0x43F low bytes */
  volatile uint32_t diag_a;     /* diag-window read of target 0x00500400 (pre-reset) */
  volatile uint32_t diag_b;     /* diag-window read of target 0x00520100 (BMI-write test) */
  volatile uint32_t boot_step;  /* firmware boot sequence: last WIFI_BOOT_* reached */
  volatile uint32_t boot_exec;  /* BMIExecute return param from the NWM stub */
  volatile uint32_t boot_ready; /* HI+0x58 poll: 1 = NWM firmware signalled ready */
  volatile uint32_t htc_ready;  /* 1 = an HTC control message was found on mailbox 0 */
  volatile uint32_t htc_look;   /* RX lookahead 0x408 (HTC frame header of a pending msg) */
  volatile uint32_t htc_msgid;  /* HTC message ID (1 = HTC_MSG_READY) */
  volatile uint32_t htc_credits;/* HTC_READY CreditCount */
  volatile uint32_t htc_credsz; /* HTC_READY CreditSize  */
  volatile uint32_t htc_regs;   /* HIF ints: 0x400 | 0x403<<8 | 0x404<<16 | 0x405<<24 */
  volatile uint32_t bmi_sends;  /* count of BMI mailbox sends during a boot */
  volatile uint32_t bmi_nocred; /* sends that ran out the credit wait with no credit */
  volatile uint32_t fw_chk;     /* upload-integrity bits: b0 database word0 ok, b1 HI+0x6c==0x80, b2 HI+0x74==0x63 */
  volatile uint32_t fw_dbrd;    /* database word0 read back from target via diag */
  volatile uint32_t fw_dbex;    /* database word0 expected (from the SD-loaded blob) */
} WifiShared;

/* Firmware boot sequence progress (WifiShared.boot_step). */
enum {
  WIFI_BOOT_NONE = 0,
  WIFI_BOOT_NOFW,   /* firmware not staged in FCRAM (ARM9 SD load failed) */
  WIFI_BOOT_BADVER, /* target version/type is not AR6014, refused to boot */
  WIFI_BOOT_HI,     /* wrote HTC protocol version + SOC clock setup */
  WIFI_BOOT_STUB,   /* uploaded stub_data + stub_code, executed the NWM stub */
  WIFI_BOOT_MAIN,   /* fast-downloaded main firmware + database, set host interest */
  WIFI_BOOT_DONE,   /* BMIDone issued, polled the ready flag */
};

void wifi_probe(void);          /* trigger the ARM11 SDIO/BMI probe */
void wifi_boot(void);           /* trigger the ARM11 firmware upload + boot */
void wifi_get(WifiShared *out); /* read results (invalidates cache first) */

#endif /* AURORA_WIFI_H */
