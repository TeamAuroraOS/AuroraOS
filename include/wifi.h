/* SPDX-License-Identifier: GPL-2.0 */
/* LICENSE: The Wi-Fi code is GPL-2.0, separately from the rest of AuroraOS. It
 * derives register facts and the BMI/HIF bring-up sequence from the ath6kl
 * legacy driver as ported to the 3DS by Octoblimp, and more from the Linux 3DS
 * port (techflashYT, Peugeot205GTI). Credit: Octoblimp; ath6kl (GPL-2.0); the
 * Linux 3DS port. See docs/wifi.md "License and credits". */
#ifndef AURORA_WIFI_H
#define AURORA_WIFI_H

#include <stdint.h>

#define WIFI_SHARED_ADDR 0x233B0000u
#define WIFI_SDIO_BASE   0x10122000u /* logical; = physical 0x1EC22000 */

/* Firmware staging, between the audio PCM buffer and the app-launch stage.
 * The blobs are Nintendo copyright: loaded from SD at runtime, never
 * embedded. */
#define WIFI_FW_ADDR      0x23E00000u
#define WIFI_FW_MAGIC     0x46574631u /* "1FWF": ARM9 staged all four blobs */
#define WIFI_FW_STUBDATA (WIFI_FW_ADDR + 0x00001000u)
#define WIFI_FW_STUBCODE (WIFI_FW_ADDR + 0x00002000u)
#define WIFI_FW_DATABASE (WIFI_FW_ADDR + 0x00003000u)
#define WIFI_FW_MAIN     (WIFI_FW_ADDR + 0x00010000u)

enum {
  WIFI_FW_TYPE1 = 1,
  WIFI_FW_TYPE4 = 4,
};

typedef struct {
  volatile uint32_t magic;
  volatile uint32_t stubdata_len;
  volatile uint32_t stubcode_len;
  volatile uint32_t database_len;
  volatile uint32_t main_len;
  volatile uint32_t main_type;
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
  WIFI_EXT_FC,
  WIFI_EXT_FE,
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
  volatile uint32_t cis_addr;
  volatile uint8_t cis[48];
  volatile uint16_t manf;       /* CISTPL_MANFID: 0x0271 = Atheros */
  volatile uint16_t card;       /* CISTPL_MANFID: 0x0201 = AR6014 */
  volatile uint8_t ior;         /* CCCR 0x03 I/O Ready: bit1 = fn1 ready */
  volatile uint8_t hif[16];     /* function-1 HIF regs 0x400..0x40F */
  volatile int32_t bmi_wr;      /* BMI CMD53 write result (0 ok, neg err) */
  volatile int32_t bmi_rd;
  volatile uint32_t bmi_ver;
  volatile uint32_t bmi_type;
  volatile uint32_t bmi_look;   /* first word of the target-info reply */
  volatile uint16_t bmi_s0;
  volatile uint16_t bmi_s1;
  volatile uint16_t bmi_ctl;
  volatile uint16_t bmi_idx;
  volatile uint32_t bmi_credit; /* first credit read from 0x450 */
  volatile uint8_t cnt[8];      /* COUNT regs 0x420..0x43F low bytes */
  volatile uint32_t diag_a;     /* diag-window read of target 0x00500400 */
  volatile uint32_t diag_b;     /* diag-window read of target 0x00520100 (BMI-write test) */
  volatile uint32_t boot_step;  /* WIFI_BOOT_* */
  volatile uint32_t boot_exec;  /* BMIExecute return param from the NWM stub */
  volatile uint32_t boot_ready; /* HI+0x58 poll: 1 = NWM firmware signalled ready */
  volatile uint32_t htc_ready;  /* 1 = an HTC control message was found on mailbox 0 */
  volatile uint32_t htc_look;   /* RX lookahead 0x408 (HTC frame header of a pending msg) */
  volatile uint32_t htc_msgid;  /* HTC message ID (1 = HTC_MSG_READY) */
  volatile uint32_t htc_credits;/* HTC_READY CreditCount */
  volatile uint32_t htc_credsz; /* HTC_READY CreditSize  */
  volatile uint32_t htc_regs;   /* 0x400 | 0x401<<8 | 0x402<<16 | 0x405<<24 */
  volatile uint32_t bmi_sends;
  volatile uint32_t bmi_nocred; /* sends that ran out the credit wait with no credit */
  volatile uint32_t fw_chk;     /* unused */
  volatile uint32_t fw_dbrd;    /* stub_code word 0 read back via diag */
  volatile uint32_t fw_dbex;    /* stub_code word 0 as staged */
  volatile uint32_t fw_type;    /* selected NWM Main.type image */
  volatile uint32_t boot_opts;  /* WIFI_OPT_* the last boot ran with */
  volatile uint32_t wificnt;    /* CFG11_WIFICNT before<<8 | after, bit0 = on */
  volatile uint32_t htc_ep;     /* endpoint the target gave WMI_CONTROL_SVC */
  volatile uint32_t htc_status; /* HTC connect-service status, 0 = connected */
  volatile uint32_t htc_maxmsg; /* largest message that endpoint accepts */
  volatile uint32_t htc_stage;  /* WIFI_HTC_* */
  volatile uint32_t wmi_event;  /* first WMI event id (0x1001 = WMI_READY) */
  volatile uint32_t wmi_mac0;   /* MAC bytes 0-3 */
  volatile uint32_t wmi_mac1;   /* MAC bytes 4-5 */
  volatile uint32_t wmi_swver;  /* firmware version from WMI_READY */
  volatile uint32_t htc_regs2;  /* CCCR after boot: IOE | IOR<<8 | width<<16 | blksz<<24 */
  volatile uint32_t htc_look2;  /* lookahead (0x408) after HTC_READY was read */
  volatile uint32_t htc_drained;/* frames dropped before connect | last id << 16 */
  volatile uint32_t htc_msg0;   /* first 4 bytes of the frame the connect read got */
  volatile uint32_t htc_try;    /* how many write styles the connect tried */
  volatile uint32_t htc_err;    /* bit per style whose transfer the controller refused */
  /* Per write style: host int | err<<8 | lookahead<<16 | frame<<24, or, when
   * the controller refused the write, bit 31 | STAT0 | STAT1<<16. */
  volatile uint32_t htc_snap[5];
  volatile uint32_t boot_tries;  /* 1, or 2 when the upload had to be redone */
  volatile uint32_t fw_sum;      /* rolling sum of the staged blobs */
  volatile uint32_t clkcnt;      /* CFG11_MPCORE_CLKCNT, the ARM11 clock mode */
  volatile uint32_t trace;       /* boot step << 24 | sends, published every send */
  /* The first command that failed: the controller command word, 0x0053 or
   * 0x1053 for a CMD53 write or read, 0x0405 for a BMI reply that never came. */
  volatile uint32_t fail_cmd;
  volatile uint32_t fail_arg;    /* its argument; for a CMD53 the address | -err << 28 */
  volatile uint32_t fail_stat;   /* STAT0 | STAT1 << 16 at that moment */
  volatile uint32_t fail_at;     /* attempt << 28 | boot step << 24 | sends done */
  volatile uint32_t bmi_polls;   /* credit polls this attempt, published live */
  volatile uint32_t bmi_extra;   /* reply words left in the mailbox and drained */
  volatile uint32_t bmi_bc;      /* target-info byte count (ath6kl expects 12) */
  volatile uint32_t soc_sleep;   /* SYSTEM_SLEEP (0x40c4) as the ROM had it */
  volatile uint32_t soc_scratch; /* LOCAL_SCRATCH (0x180c0) as the ROM had it */
  volatile uint32_t hb;          /* WIFI_HB_* << 28 | register << 16 | command */
} WifiShared;

/* The controller access in progress (WifiShared.hb), published before each. */
enum {
  WIFI_HB_CMD_WAIT = 1, /* waiting for the command line before a command */
  WIFI_HB_CMD_SENT,     /* a command out, waiting for its response       */
  WIFI_HB_CMD_DONE,
  WIFI_HB_DATA_WAIT,    /* the same three for a data command (CMD53)     */
  WIFI_HB_DATA_SENT,
  WIFI_HB_DATA_FIFO,    /* moving the data through the FIFO              */
  WIFI_HB_DATA_END,     /* data moved, waiting for the transfer to end   */
  WIFI_HB_DATA_DONE,
};

/* Boot options. With all three set the sequence matches the Linux 3DS port
 * (which follows nocash's wifiboot); with none, Octoblimp's ath6kl port. */
#define WIFI_OPT_RESTORE_SOC 0x01u /* put 0x40c4 / 0x180c0 back before BMIDone */
#define WIFI_OPT_NO_POST_LZ  0x02u /* no stub_data/database/HI+0x18 after the LZ */
#define WIFI_OPT_NO_INT_EN   0x04u /* leave INT_STATUS_ENABLE alone (ath6kl zeroes it) */
#define WIFI_OPT_SLEEP_ON    0x08u /* let the chip sleep again at the finish (nocash) */
#define WIFI_OPT_LINUX       (WIFI_OPT_RESTORE_SOC | WIFI_OPT_NO_POST_LZ | \
                              WIFI_OPT_NO_INT_EN)

enum {
  WIFI_HTC_NONE = 0,
  WIFI_HTC_READY,    /* the firmware posted HTC_READY                     */
  WIFI_HTC_CONNECT,  /* connect-service sent                              */
  WIFI_HTC_CONNECTED,/* the target answered with an endpoint              */
  WIFI_HTC_SETUP,    /* setup-complete sent                               */
  WIFI_HTC_WMI,      /* a WMI event came back                             */
};

enum {
  WIFI_BOOT_NONE = 0,
  WIFI_BOOT_NOFW,   /* firmware not staged in FCRAM (ARM9 SD load failed) */
  WIFI_BOOT_BADVER, /* target version/type is not AR6014, refused to boot */
  WIFI_BOOT_HI,     /* wrote HTC protocol version + SOC clock setup */
  WIFI_BOOT_STUB,   /* uploaded stub_data + stub_code, executed the NWM stub */
  WIFI_BOOT_MAIN,   /* fast-downloaded main firmware + database, set host interest */
  WIFI_BOOT_DONE,   /* BMIDone issued, polled the ready flag */
};

void wifi_probe(void);

/* opts: WIFI_OPT_* */
void wifi_boot(uint32_t opts);
void wifi_get(WifiShared *out); /* Invalidates the cache first. */

#endif
