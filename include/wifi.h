/*
 * AuroraOS Wi-Fi SDIO probe (ARM9 <-> ARM11). The chip is an Atheros AR6014 on a
 * 16-bit TMIO/SDHC host controller ("controller 2", logical 0x10122000) reached
 * from the ARM11. The probe runs in the ARM11 core and reports back through the
 * shared block below. See docs/wifi.md for the full state of the bring-up.
 */
#ifndef AURORA_WIFI_H
#define AURORA_WIFI_H

#include <stdint.h>

#define WIFI_SHARED_ADDR 0x233B0000u
#define WIFI_SDIO_BASE   0x10122000u /* logical; = physical 0x1EC22000 */

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
} WifiShared;

void wifi_probe(void);          /* trigger the ARM11 probe (posts to audio core) */
void wifi_get(WifiShared *out); /* read results (invalidates cache first) */

#endif /* AURORA_WIFI_H */
