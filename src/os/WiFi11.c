/* SPDX-License-Identifier: GPL-2.0 */
/* LICENSE: The Wi-Fi code in this file is GPL-2.0, separately from the rest of
 * AuroraOS. It derives register facts and the BMI/HIF bring-up sequence from
 * the ath6kl legacy driver as ported to the 3DS by Octoblimp, and the subsystem
 * power, boot finish and HTC details from the Linux 3DS port (techflashYT,
 * Peugeot205GTI). Credit: Octoblimp; ath6kl (GPL-2.0); the Linux 3DS port. See
 * docs/wifi.md "License and credits".
 *
 * The host controller uses the TMIO register layout of src/sdmmc.h. */
#include "core11.h"
#include "wifi.h"

#define WB16(off) MMIO16(WIFI_SDIO_BASE + (off))

#define WR_CMD     0x00
#define WR_PORTSEL 0x02
#define WR_ARG0    0x04
#define WR_ARG1    0x06
#define WR_STOP    0x08
#define WR_RESP0   0x0C
#define WR_RESP1   0x0E
#define WR_STAT0   0x1C
#define WR_STAT1   0x1E
#define WR_IRM0    0x20
#define WR_IRM1    0x22
#define WR_CLKCTL  0x24
#define WR_BLKLEN  0x26
#define WR_OPT     0x28
#define WR_DATACTL 0xD8
#define WR_RESET   0xE0
#define WR_DATACTL32  0x100
#define WR_BLKLEN32   0x104
#define WR_BLKCOUNT32 0x108

#define WSTAT0_CMDRESPEND 0x0001u
#define WSTAT1_CMDBUSY    0x4000u
#define WMASK_GW          0x807Fu /* sdmmc.h TMIO_MASK_GW */
#define WMASK_ALL         0x837F031Du

static void wifi_setckl(uint32_t data) {
  WB16(WR_CLKCTL) = (uint16_t)(data & 0xFF);
  WB16(WR_CLKCTL) = (uint16_t)((1u << 8) | (data & 0x2FF));
}

/* sdmmc.c controller init and set_target against the Wi-Fi base, without the
 * SD-slot mount fix. */
static void wifi_ctrl_init(void) {
  WB16(WR_DATACTL32) &= 0xF7FFu;
  WB16(WR_DATACTL32) &= 0xEFFFu;
  WB16(WR_DATACTL32) |= 0x402u;
  WB16(WR_DATACTL) = (uint16_t)((WB16(WR_DATACTL) & 0xFFDD) | 2);
  WB16(WR_DATACTL32) &= 0xFFFFu;
  WB16(WR_DATACTL) &= 0xFFDFu;
  WB16(WR_BLKLEN32) = 512;
  WB16(WR_BLKCOUNT32) = 1;
  WB16(WR_RESET) &= 0xFFFEu;
  WB16(WR_RESET) |= 1u;
  WB16(WR_IRM0) |= (uint16_t)WMASK_ALL;
  WB16(WR_IRM1) |= (uint16_t)(WMASK_ALL >> 16);
  WB16(0xFC) |= 0xDBu;
  WB16(0xFE) |= 0xDBu;
  WB16(WR_PORTSEL) &= 0xFFFCu;
  WB16(WR_CLKCTL) = 0x20;
  WB16(WR_OPT) = 0x40E9;
  WB16(WR_PORTSEL) &= 0xFFFCu;
  WB16(WR_BLKLEN) = 512;
  WB16(WR_STOP) = 0;

  /* set_target: port 0, ~523 kHz identification clock, 1-bit bus. */
  WB16(WR_PORTSEL) &= 0xFFFCu;
  wifi_setckl(0x20);
  WB16(WR_OPT) |= 0x8000u;
}

/* Command word: index | response type, encoded as in sdmmc.c. */
#define WCMD5  0x0705u /* IO_SEND_OP_COND, R4 */
#define WCMD3  0x0403u /* SEND_RELATIVE_ADDR, R6 */
#define WCMD7  0x0507u /* SELECT_CARD, R1b */
#define WCMD52 0x0434u /* IO_RW_DIRECT, R5 */

/* CMD52 argument: bit31 R/W, bits30-28 func, bit27 RAW (read-after-write),
 * bits25-9 addr, bits7-0 data. */
#define WCMD52_RD(func, addr) \
  (((uint32_t)(func) << 28) | (((uint32_t)(addr) & 0x1FFFFu) << 9))
#define WCMD52_WR(func, addr, data)                                   \
  ((1u << 31) | ((uint32_t)(func) << 28) |                            \
   (((uint32_t)(addr) & 0x1FFFFu) << 9) | ((uint32_t)(data) & 0xFFu))
#define WCMD52_WRR(func, addr, data) (WCMD52_WR(func, addr, data) | (1u << 27))

static void wifi_note_fail(WifiShared *w, uint16_t cmd, uint32_t arg,
                           uint16_t s0, uint16_t s1) {
  if (w->fail_cmd)
    return;
  w->fail_cmd = cmd;
  w->fail_arg = arg;
  w->fail_stat = (uint32_t)s0 | ((uint32_t)s1 << 16);
  w->fail_at = w->bmi_sends | (w->boot_step << 24) | (w->boot_tries << 28);
  dcache_clean();
}

/* Set when the upload has to stop; the remaining sends become no-ops. */
static int g_bmi_abort = 0;

static void wifi_ms(uint32_t ms) {
  for (volatile uint32_t d = 0; d < ms * 10000u; d++)
    ;
}

/* The controller access about to be made, in its own cache line, so an access
 * that never returns leaves its position on screen. */
static void wifi_hb(WifiShared *w, uint32_t step, uint16_t cmd, uint32_t arg) {
  w->hb = (step << 28) | (((arg >> 9) & 0xFFFu) << 16) | cmd;
  dcache_clean_line((const void *)&w->hb);
}

static void wifi_timeout(WifiShared *w) {
  w->timeouts++;
  dcache_clean_line((const void *)&w->timeouts);
}

static void wifi_cmd(WifiShared *w, WifiCmd *e, uint16_t cmd16, uint32_t arg) {
  uint32_t g = 0;
  wifi_hb(w, WIFI_HB_CMD_WAIT, cmd16, arg);
  while ((WB16(WR_STAT1) & WSTAT1_CMDBUSY) && ++g < 100000u)
    ;
  if (g >= 100000u)
    wifi_timeout(w);
  WB16(WR_IRM0) = 0;
  WB16(WR_IRM1) = 0;
  WB16(WR_STAT0) = 0;
  WB16(WR_STAT1) = 0;
  WB16(WR_DATACTL32) = (uint16_t)((WB16(WR_DATACTL32) & ~0x1800u) | 0x400u);
  WB16(WR_ARG0) = (uint16_t)(arg & 0xFFFF);
  WB16(WR_ARG1) = (uint16_t)(arg >> 16);
  wifi_hb(w, WIFI_HB_CMD_SENT, cmd16, arg);
  WB16(WR_CMD) = cmd16;

  g = 0;
  for (;;) {
    uint16_t s1 = WB16(WR_STAT1);
    if (s1 & WMASK_GW)
      break; /* hard error (timeout/CRC/etc.) */
    if (!(s1 & WSTAT1_CMDBUSY) && (WB16(WR_STAT0) & WSTAT0_CMDRESPEND))
      break;
    if (++g >= 300000u) {
      wifi_timeout(w);
      break;
    }
  }
  wifi_hb(w, WIFI_HB_CMD_DONE, cmd16, arg);
  uint16_t s0 = WB16(WR_STAT0);
  uint16_t s1 = WB16(WR_STAT1);
  e->cmd = cmd16;
  e->arg = arg;
  e->stat0 = s0;
  e->stat1 = s1;
  e->resp = (uint32_t)WB16(WR_RESP0) | ((uint32_t)WB16(WR_RESP1) << 16);
  e->ok = (s0 & WSTAT0_CMDRESPEND) ? 1u : 2u;
  if (e->ok == 2u)
    wifi_note_fail(w, cmd16, arg, s0, s1);
}

static WifiCmd *wifi_logcmd(WifiShared *w, uint16_t cmd16, uint32_t arg) {
  if (w->nlog >= WIFI_LOG_MAX)
    return &w->log[WIFI_LOG_MAX - 1];
  WifiCmd *e = (WifiCmd *)&w->log[w->nlog];
  wifi_cmd(w, e, cmd16, arg);
  w->nlog++;
  dcache_clean();
  return e;
}

/* GPIO_DATA4 bit0: 0 holds the Wi-Fi chip in reset. */
#define WIFI_GPIO_WIFI 0x10147028u

/* CFG11_WIFICNT bit0 powers the Wi-Fi subsystem (u8). */
#define CFG11_WIFICNT 0x10140180u

#define WR_FIFO     0x30
#define WR_SDFIFO32 0x10C
#define WSTAT1_RXRDY 0x0100u
#define WSTAT1_TXRQ  0x0200u

/* STAT0 | STAT1 << 16 when the last CMD53 ended, success or not. */
static uint32_t g_cmd53_stat;

/* CMD53 (IO_RW_EXTENDED) through the 16-bit FIFO. blksz 0 is byte mode, which
 * the BMI bootloader needs; otherwise len/blksz blocks. `buf` must be 4-byte
 * aligned. Returns 0, or negative on error or timeout. */
static int wifi_cmd53(WifiShared *w, uint32_t func, uint32_t addr,
                      uint32_t *buf, uint32_t len, int is_write, int incr,
                      uint32_t blksz, int secure) {
  uint32_t bytes = (len + 3u) & ~3u;
  int nhalf = (int)(bytes / 2u);
  uint16_t *h = (uint16_t *)buf;
  uint32_t g = 0;
  wifi_hb(w, WIFI_HB_DATA_WAIT, 0x0035u, addr << 9);
  while ((WB16(WR_STAT1) & WSTAT1_CMDBUSY) && ++g < 100000u)
    ;
  if (g >= 100000u)
    wifi_timeout(w);
  WB16(WR_IRM0) = 0;
  WB16(WR_IRM1) = 0;
  WB16(WR_STAT0) = 0;
  WB16(WR_STAT1) = 0;

  /* DATA16 mode (bit1 clear). */
  WB16(WR_DATACTL) &= ~0x0002u;
  WB16(WR_DATACTL32) &= ~0x0002u;
  uint32_t blocks = blksz ? (bytes + blksz - 1u) / blksz : 0;
  WB16(WR_STOP) = 0;
  WB16(0x0A) = (uint16_t)(blocks ? blocks : 1u);       /* DATA16 BLK_CNT */
  WB16(WR_BLKLEN) = (uint16_t)(blksz ? blksz : bytes);
  if (blocks) {
    /* Only block mode sets the DATA32 pair, as the Linux driver does.
     * Writing it for byte-mode BMI transfers stalls the upload. */
    WB16(0x108) = (uint16_t)blocks; /* DATA32 BLK_CNT */
    WB16(0x104) = (uint16_t)blksz;  /* DATA32 BLK_LEN */
  }

  uint32_t count = blocks ? blocks : len;
  uint32_t arg = ((uint32_t)(is_write & 1) << 31) | ((func & 7u) << 28) |
                 ((blocks ? 1u : 0u) << 27) | ((uint32_t)(incr & 1) << 26) |
                 ((addr & 0x1FFFFu) << 9) | (count & 0x1FFu);
  WB16(WR_ARG0) = (uint16_t)(arg & 0xFFFF);
  WB16(WR_ARG1) = (uint16_t)(arg >> 16);
  /* CMD53 | R5 | data; 0x4000 is the SDIO-command ("secure") bit the Linux
   * driver sets. */
  uint16_t cmd = 0x0035u | 0x0400u | 0x0800u | (secure ? 0x4000u : 0u);
  if (!is_write)
    cmd |= 0x1000u; /* read direction */
  if (blocks > 1u)
    cmd |= 0x2000u; /* multi-block */
  wifi_hb(w, WIFI_HB_DATA_SENT, cmd, addr << 9);
  WB16(WR_CMD) = cmd;

  int idx = 0, done_data = 0, err = 0;
  uint16_t s0 = 0, s1 = 0;
  g = 0;
  for (;;) {
    s1 = WB16(WR_STAT1);
    if (!is_write && (s1 & WSTAT1_RXRDY) && idx < nhalf) {
      WB16(WR_STAT1) = (uint16_t)(s1 & ~WSTAT1_RXRDY);
      wifi_hb(w, WIFI_HB_DATA_FIFO, cmd, addr << 9);
      for (int i = 0; i < nhalf; i++)
        h[idx++] = WB16(WR_FIFO);
      done_data = 1;
    }
    if (is_write && (s1 & WSTAT1_TXRQ) && idx < nhalf) {
      WB16(WR_STAT1) = (uint16_t)(s1 & ~WSTAT1_TXRQ);
      wifi_hb(w, WIFI_HB_DATA_FIFO, cmd, addr << 9);
      for (int i = 0; i < nhalf; i++)
        WB16(WR_FIFO) = h[idx++];
      done_data = 1;
      wifi_hb(w, WIFI_HB_DATA_END, cmd, addr << 9);
    }
    s0 = WB16(WR_STAT0);
    if (s1 & WMASK_GW) {
      err = -1;
      break;
    }
    if (!(s1 & WSTAT1_CMDBUSY) && done_data && (s0 & 0x0004u)) { /* DATAEND */
      wifi_hb(w, WIFI_HB_DATA_DONE, cmd, addr << 9);
      g_cmd53_stat = (uint32_t)s0 | ((uint32_t)s1 << 16);
      return 0;
    }
    if (++g >= 800000u) {
      err = -2;
      break;
    }
  }
  g_cmd53_stat = (uint32_t)s0 | ((uint32_t)s1 << 16);
  wifi_note_fail(w, is_write ? 0x0053u : 0x1053u,
                 addr | ((uint32_t)(-err) << 28), s0, s1);
  /* A data phase that did not finish leaves the controller mid-transfer, and
   * the next command would go out on top of it: start it clean. */
  wifi_timeout(w);
  wifi_ctrl_init();
  return err;
}

/* The bus width, on the card (CCCR 0x07 bits 1:0) and then on the controller
 * (WR_OPT bit 15 set = 1-bit). */
static void wifi_bus_width(WifiShared *w, int wide) {
  WifiCmd t;
  wifi_cmd(w, &t, WCMD52, WCMD52_RD(0, 0x07));
  uint8_t bic = (uint8_t)((t.resp & 0xFCu) | (wide ? 0x02u : 0x00u));
  wifi_cmd(w, &t, WCMD52, WCMD52_WR(0, 0x07, bic));
  if (wide)
    WB16(WR_OPT) &= (uint16_t)~0x8000u;
  else
    WB16(WR_OPT) |= 0x8000u;
}

/* Mailbox 0 spans 0x800-0xFFF. A write ending at 0xFFF (EOM) completes a
 * message, so every send is end-aligned: base = 0x1000 - len. */
#define WIFI_MBOX0 0x800u

/* Diagnostic window: reads and writes any target address, independent of the
 * target CPU. WINDOW_DATA 0x474, WINDOW_WRITE_ADDR 0x478, WINDOW_READ_ADDR
 * 0x47C. Writing the address LSB starts the access, so it goes last. */
static void wifi_diag_setwin(WifiShared *w, uint32_t reg, uint32_t addr) {
  WifiCmd t;
  wifi_cmd(w, &t, WCMD52, WCMD52_WR(1, reg + 1, (addr >> 8) & 0xFFu));
  wifi_cmd(w, &t, WCMD52, WCMD52_WR(1, reg + 2, (addr >> 16) & 0xFFu));
  wifi_cmd(w, &t, WCMD52, WCMD52_WR(1, reg + 3, (addr >> 24) & 0xFFu));
  wifi_cmd(w, &t, WCMD52, WCMD52_WR(1, reg, addr & 0xFFu));
}

static uint32_t wifi_diag_read(WifiShared *w, uint32_t addr) {
  wifi_diag_setwin(w, 0x47Cu, addr);
  uint32_t v = 0;
  WifiCmd t;
  for (int i = 0; i < 4; i++) {
    wifi_cmd(w, &t, WCMD52, WCMD52_RD(1, 0x474u + i));
    v |= (t.resp & 0xFFu) << (i * 8);
  }
  return v;
}

/* 1 ms apart: the Linux 3DS port raised ath6kl's BMI timeout to 3 s. */
#define BMI_POLLS 3000

/* Reads `len` bytes of a BMI reply from mailbox 0 once RX_LOOKAHEAD_VALID
 * (0x405 bit 0) says a word is there. Returns 0, and ends the boot, when none
 * comes: reading an empty mailbox would leave the late reply in it and shift
 * every later reply by one. */
static int wifi_bmi_recv_bytes(WifiShared *w, uint8_t *buf, int len) {
  WifiCmd t;
  int have = 0;
  for (int i = 0; i < BMI_POLLS && !have; i++) {
    wifi_cmd(w, &t, WCMD52, WCMD52_RD(1, 0x405));
    if (t.ok == 1u && (t.resp & 0x01u))
      have = 1;
    else
      wifi_ms(1);
  }
  if (!have) {
    for (int i = 0; i < len; i++)
      buf[i] = 0;
    wifi_note_fail(w, 0x0405u, 0, 0, 0);
    g_bmi_abort = 1;
    return 0;
  }
  for (int i = 0; i < len; i++) {
    wifi_cmd(w, &t, WCMD52, WCMD52_RD(1, WIFI_MBOX0 + i));
    buf[i] = (uint8_t)(t.resp & 0xFFu);
  }
  return 1;
}

static uint32_t rd32le(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static uint32_t wifi_bmi_recv_word(WifiShared *w) {
  uint8_t b[4];
  wifi_bmi_recv_bytes(w, b, 4);
  return rd32le(b);
}

/* Discards words left in the mailbox after a reply, so a reply longer than
 * expected cannot shift the next one. */
static void wifi_bmi_drain(WifiShared *w) {
  WifiCmd t;
  for (int n = 0; n < 16; n++) {
    wifi_cmd(w, &t, WCMD52, WCMD52_RD(1, 0x405));
    if (t.ok != 1u || !(t.resp & 0x01u))
      return;
    for (int i = 0; i < 4; i++)
      wifi_cmd(w, &t, WCMD52, WCMD52_RD(1, WIFI_MBOX0 + i));
    w->bmi_extra++;
  }
}

/* The BMI credit counter is COUNT_DEC[4] at 0x450 (0x440 + 4 * 4), and reading
 * it takes the credit. ath6kl's 4-byte read is one access; four byte reads of
 * 0x450..0x453 are four, and a credit granted between them is lost, so this
 * reads one byte per poll. Returns 1 with a credit, 0 if none came, -1 if the
 * chip stopped answering. */
static int wifi_bmi_credit(WifiShared *w) {
  WifiCmd t;
  uint32_t t0 = w->timeouts;
  int bad = 0;

  for (int i = 0; i < BMI_POLLS; i++) {
    wifi_cmd(w, &t, WCMD52, WCMD52_RD(1, 0x450));
    w->bmi_polls++;
    dcache_clean_line((const void *)&w->bmi_polls);
    if (t.ok == 1u) {
      bad = 0;
      if (t.resp & 0xFFu)
        return 1;
    } else if (++bad > 8) {
      return -1;
    }
    if (w->timeouts - t0 > 4u)
      return -1;
    wifi_ms(1);
  }
  return 0;
}

static void wifi_bmi_send(WifiShared *w, const uint8_t *buf, int len) {
  static uint32_t sbuf[80];
  if (g_bmi_abort)
    return;
  int cr = wifi_bmi_credit(w);
  if (cr <= 0) {
    /* ath6kl never writes without a credit: the target holds one command,
     * and a second one lands on top of it. The boot is redone instead. */
    if (cr == 0)
      w->bmi_nocred++;
    g_bmi_abort = 1;
    dcache_clean();
    return;
  }
  for (int i = 0; i < len; i++)
    ((uint8_t *)sbuf)[i] = buf[i];
  uint32_t wbase = 0x1000u - (uint32_t)len;
  if (wifi_cmd53(w, 1, wbase, sbuf, (uint32_t)len, 1, 1, 0, 0) != 0) {
    wifi_note_fail(w, 0x0053u, wbase, (uint16_t)WB16(WR_STAT0),
                   (uint16_t)WB16(WR_STAT1));
    g_bmi_abort = 1;
    dcache_clean();
    return;
  }
  w->bmi_sends++;
  w->trace = (w->boot_step << 24) | (w->bmi_sends & 0xFFFFu);
  dcache_clean();
}

/* BMI_GET_TARGET_INFO: 0xFFFFFFFF, then a byte count, version and type. The
 * count is honoured, so a longer reply cannot leave words in the mailbox. */
static void wifi_bmi_target_info(WifiShared *w) {
  WifiCmd t;
  static uint32_t cmdw;
  uint8_t info[64];

  wifi_ms(100); /* the Linux port: not ready for BMI the instant it enumerates */
  for (int i = 0; i < 8; i++) {
    wifi_cmd(w, &t, WCMD52, WCMD52_RD(1, 0x420 + i * 4));
    w->cnt[i] = (uint8_t)(t.resp & 0xFFu);
  }
  w->bmi_ver = 0;
  w->bmi_type = 0;
  w->bmi_bc = 0;
  w->bmi_look = 0;
  w->bmi_credit = (uint32_t)wifi_bmi_credit(w);
  if (w->bmi_credit != 1u) {
    w->bmi_wr = -1;
    return;
  }
  cmdw = 8; /* BMI_GET_TARGET_INFO */
  w->bmi_wr = wifi_cmd53(w, 1, 0x1000u - 4u, &cmdw, 4, 1, 1, 0, 0);
  if (w->bmi_wr != 0)
    return;

  uint32_t v0 = wifi_bmi_recv_word(w);
  if (v0 == 0xFFFFFFFFu) {
    uint32_t bc = wifi_bmi_recv_word(w);
    w->bmi_bc = bc;
    if (bc < 12u || bc > 4u + sizeof(info))
      bc = 12u; /* the layout ath6kl expects */
    wifi_bmi_recv_bytes(w, info, (int)(bc - 4u));
    w->bmi_ver = rd32le(info);
    w->bmi_type = rd32le(info + 4);
    if (bc >= 16u)
      w->bmi_look = rd32le(info + 8);
  } else {
    w->bmi_ver = v0;
    w->bmi_type = wifi_bmi_recv_word(w);
  }
  wifi_bmi_drain(w);
  w->bmi_rd = g_bmi_abort ? -1 : 0;
}

/* BMI_WRITE_MEMORY of one 32-bit word: [cid=3][address][length=4][data]. */
static void wifi_bmi_write_word(WifiShared *w, uint32_t addr, uint32_t val) {
  uint32_t words[4] = {3u, addr, 4u, val};
  uint8_t cmd[16];
  for (int i = 0; i < 4; i++)
    for (int b = 0; b < 4; b++)
      cmd[i * 4 + b] = (uint8_t)((words[i] >> (b * 8)) & 0xFFu);
  wifi_bmi_send(w, cmd, 16);
}

/* BMI command IDs (ath6kl bmi.h). A command must fit the 128-byte target
 * FIFO. */
#define BMI_DONE              1u
#define BMI_WRITE_MEMORY      3u
#define BMI_EXECUTE          4u
#define BMI_READ_SOC_REGISTER 6u
#define BMI_WRITE_SOC_REGISTER 7u
#define BMI_LZ_STREAM_START  13u
#define BMI_LZ_DATA          14u
#define BMI_MBOX_FIFO        128u

static void wr32le(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

static void wifi_bmi_write_mem(WifiShared *w, uint32_t addr, const uint8_t *data,
                               uint32_t len) {
  const uint32_t maxdata = (BMI_MBOX_FIFO - 12u) & ~3u; /* 116 bytes */
  uint32_t off = 0;
  while (off < len) {
    uint32_t chunk = len - off;
    if (chunk > maxdata)
      chunk = maxdata;
    uint32_t txlen = (chunk + 3u) & ~3u;
    uint8_t cmd[12 + 116];
    wr32le(cmd + 0, BMI_WRITE_MEMORY);
    wr32le(cmd + 4, addr + off);
    wr32le(cmd + 8, txlen);
    for (uint32_t i = 0; i < txlen; i++)
      cmd[12 + i] = (off + i < len) ? data[off + i] : 0u;
    wifi_bmi_send(w, cmd, (int)(12u + txlen));
    off += chunk;
  }
}

static uint32_t wifi_bmi_execute(WifiShared *w, uint32_t addr, uint32_t param) {
  uint8_t cmd[12];
  wr32le(cmd + 0, BMI_EXECUTE);
  wr32le(cmd + 4, addr);
  wr32le(cmd + 8, param);
  wifi_bmi_send(w, cmd, 12);
  uint32_t v = wifi_bmi_recv_word(w);
  wifi_bmi_drain(w);
  return v;
}

/* Leaves BMI and starts the loaded firmware. */
static void wifi_bmi_done(WifiShared *w) {
  uint8_t cmd[4];
  wr32le(cmd, BMI_DONE);
  wifi_bmi_send(w, cmd, 4);
}

static uint32_t wifi_bmi_read_soc(WifiShared *w, uint32_t addr) {
  uint8_t cmd[8];
  wr32le(cmd + 0, BMI_READ_SOC_REGISTER);
  wr32le(cmd + 4, addr);
  wifi_bmi_send(w, cmd, 8);
  uint32_t v = wifi_bmi_recv_word(w);
  wifi_bmi_drain(w);
  return v;
}

static void wifi_bmi_write_soc(WifiShared *w, uint32_t addr, uint32_t val) {
  uint8_t cmd[12];
  wr32le(cmd + 0, BMI_WRITE_SOC_REGISTER);
  wr32le(cmd + 4, addr);
  wr32le(cmd + 8, val);
  wifi_bmi_send(w, cmd, 12);
}

static void wifi_bmi_lz_start(WifiShared *w, uint32_t addr) {
  uint8_t cmd[8];
  wr32le(cmd + 0, BMI_LZ_STREAM_START);
  wr32le(cmd + 4, addr);
  wifi_bmi_send(w, cmd, 8);
}

static void wifi_bmi_lz_data(WifiShared *w, const uint8_t *data, uint32_t len) {
  const uint32_t maxdata = (BMI_MBOX_FIFO - 8u) & ~3u; /* 120 bytes */
  uint32_t off = 0;
  while (off < len) {
    uint32_t chunk = len - off;
    if (chunk > maxdata)
      chunk = maxdata;
    uint8_t cmd[8 + 120];
    wr32le(cmd + 0, BMI_LZ_DATA);
    wr32le(cmd + 4, chunk);
    for (uint32_t i = 0; i < chunk; i++)
      cmd[8 + i] = data[off + i];
    wifi_bmi_send(w, cmd, (int)(8u + chunk));
    off += chunk;
  }
}

static void wifi_bmi_fast_download(WifiShared *w, uint32_t addr,
                                   const uint8_t *data, uint32_t len) {
  uint32_t aligned = len & ~3u;
  uint32_t rem = len & 3u;
  wifi_bmi_lz_start(w, addr);
  wifi_bmi_lz_data(w, data, aligned);
  if (rem) {
    uint8_t last[4] = {0, 0, 0, 0};
    for (uint32_t i = 0; i < rem; i++)
      last[i] = data[aligned + i];
    wifi_bmi_lz_data(w, last, 4);
  }
  wifi_bmi_lz_start(w, 0); /* a second stream start flushes the target caches */
}

/* Powers, resets and enumerates the chip. On return BMI and the diagnostic
 * window work. */
static void wifi_bringup(WifiShared *w, int cold) {
  w->phase = WIFI_PH_NONE;
  w->timeouts = 0;
  w->nlog = 0;

  /* Subsystem power. The chip enumerates and answers BMI without it, so it is
   * easy to miss; the Linux 3DS port sets it before enumerating. A cold start
   * takes it away first, under reset, for a chip that stopped mid-upload. */
  {
    uint8_t before = MMIO8(CFG11_WIFICNT);
    if (cold) {
      MMIO16(WIFI_GPIO_WIFI) = (uint16_t)(MMIO16(WIFI_GPIO_WIFI) & ~1u);
      MMIO8(CFG11_WIFICNT) = (uint8_t)(before & ~0x01u);
      wifi_ms(50);
    }
    MMIO8(CFG11_WIFICNT) = (uint8_t)(before | 0x01u);
    w->wificnt = ((uint32_t)before << 8) | MMIO8(CFG11_WIFICNT);
    wifi_ms(cold ? 50u : 4u); /* let the rail come up */
  }
  w->boot_step = WIFI_BOOT_NONE;
  w->boot_exec = 0;
  w->boot_ready = 0;
  dcache_clean();

  /* A full reset pulse: the chip only re-enters BMI from reset, and it stops
   * answering once the ARM9 has used the SD controller. */
  w->gpio_before = MMIO16(WIFI_GPIO_WIFI);
  MMIO16(WIFI_GPIO_WIFI) = (uint16_t)(w->gpio_before & ~1u);
  dcache_clean();
  for (volatile int d = 0; d < 2000000; d++)
    ;
  MMIO16(WIFI_GPIO_WIFI) = (uint16_t)((w->gpio_before & ~1u) | 1u);
  dcache_clean();
  for (volatile int d = 0; d < 8000000; d++) /* generous boot-ROM settle */
    ;
  w->gpio_after = MMIO16(WIFI_GPIO_WIFI);

  w->phase = WIFI_PH_REGS;
  dcache_clean();
  wifi_ctrl_init();

  w->phase = WIFI_PH_CLK;
  w->clk = WB16(WR_CLKCTL);
  dcache_clean();
  for (volatile int d = 0; d < 300000; d++) /* >=74 SD clocks before CMD5 */
    ;

  w->phase = WIFI_PH_CMD;
  dcache_clean();

  WifiCmd *e = wifi_logcmd(w, WCMD5, 0);
  uint32_t ocr = e->resp & 0x00FFFFFFu;
  if (ocr == 0)
    ocr = 0x00FF8000u;

  for (int i = 0; i < 4; i++) { /* until the OCR ready bit */
    e = wifi_logcmd(w, WCMD5, ocr);
    if (e->resp & 0x80000000u)
      break;
    for (volatile int d = 0; d < 200000; d++)
      ;
  }

  e = wifi_logcmd(w, WCMD3, 0);
  uint32_t rca = e->resp & 0xFFFF0000u;
  wifi_logcmd(w, WCMD7, rca);

  /* CCCR 0x00 SDIO revision and 0x08 card capability, for the log. */
  wifi_logcmd(w, WCMD52, WCMD52_RD(0, 0x00));
  wifi_logcmd(w, WCMD52, WCMD52_RD(0, 0x08));

  /* CIS pointer (CCCR 0x09-0x0B); its MANFID tuple identifies the chip. */
  WifiCmd tmp;
  uint32_t cis = 0;
  wifi_cmd(w, &tmp, WCMD52, WCMD52_RD(0, 0x09));
  cis |= (tmp.resp & 0xFFu);
  wifi_cmd(w, &tmp, WCMD52, WCMD52_RD(0, 0x0A));
  cis |= (tmp.resp & 0xFFu) << 8;
  wifi_cmd(w, &tmp, WCMD52, WCMD52_RD(0, 0x0B));
  cis |= (tmp.resp & 0xFFu) << 16;
  w->cis_addr = cis;
  for (int i = 0; i < 48; i++) {
    wifi_cmd(w, &tmp, WCMD52, WCMD52_RD(0, cis + i));
    w->cis[i] = (uint8_t)(tmp.resp & 0xFFu);
  }

  /* Enable function 1 (CCCR 0x02 IOE bit1), wait for CCCR 0x03 IOR bit1. */
  WifiCmd *e2 = wifi_logcmd(w, WCMD52, WCMD52_RD(0, 0x02));
  uint8_t ioe = (uint8_t)(e2->resp & 0xFFu);
  wifi_logcmd(w, WCMD52, WCMD52_WRR(0, 0x02, ioe | 0x02u));
  for (int i = 0; i < 8; i++) {
    wifi_cmd(w, &tmp, WCMD52, WCMD52_RD(0, 0x03));
    if (tmp.resp & 0x02u)
      break;
    for (volatile int d = 0; d < 200000; d++)
      ;
  }
  e2 = wifi_logcmd(w, WCMD52, WCMD52_RD(0, 0x03));
  w->ior = (uint8_t)(e2->resp & 0xFFu);

  /* Function 1 block size 128 (FBR 0x110/0x111), before any mailbox access. */
  wifi_logcmd(w, WCMD52, WCMD52_WR(0, 0x110, 128 & 0xFF));
  wifi_logcmd(w, WCMD52, WCMD52_WR(0, 0x111, (128 >> 8) & 0xFF));

  /* CCCR 0x04: master and function 1 interrupt enable. Some Atheros targets
   * only latch the RX lookahead once it is on. */
  {
    WifiCmd ci;
    wifi_cmd(w, &ci, WCMD52, WCMD52_WR(0, 0x04, 0x03));
  }

  for (int i = 0; i < 16; i++) {
    wifi_cmd(w, &tmp, WCMD52, WCMD52_RD(1, 0x400 + i));
    w->hif[i] = (uint8_t)(tmp.resp & 0xFFu);
  }

  w->diag_a = wifi_diag_read(w, 0x00500400u);
}

static void wifi_finish(WifiShared *w) {
  for (int i = 0; i < 32; i++)
    w->reg[i] = WB16(i * 2);
  w->ext[WIFI_EXT_D8] = WB16(WR_DATACTL);
  w->ext[WIFI_EXT_E0] = WB16(WR_RESET);
  w->ext[WIFI_EXT_FC] = WB16(0xFC);
  w->ext[WIFI_EXT_FE] = WB16(0xFE);
  w->ext[WIFI_EXT_100] = WB16(WR_DATACTL32);
  w->ext[WIFI_EXT_102] = WB16(WR_DATACTL32 + 2);

  w->phase = WIFI_PH_DONE;
  w->seq++;
  dcache_clean();
}

static void wifi_probe_run(void) {
  WifiShared *w = (WifiShared *)WIFI_SHARED_ADDR;
  g_bmi_abort = 0;
  w->bmi_polls = 0;
  w->bmi_extra = 0;
  wifi_bringup(w, 0);
  wifi_bmi_target_info(w);

  /* diag_b reads back 0xCAFEBABE when BMI writes land. */
  wifi_bmi_write_word(w, 0x00520100u, 0xCAFEBABEu);
  w->diag_b = wifi_diag_read(w, 0x00520100u);

  wifi_finish(w);
}

/* AR6014 memory map for the four-blob boot, from Octoblimp's port. */
#define AR6014_VERSION       0x2300006Fu
#define AR6014_TYPE          2u          /* TARGET_TYPE_AR6002 */
#define AR6014_HI            0x00520000u
#define AR6014_PARTA_DST     0x00524C00u /* main firmware (LZ)  */
#define AR6014_PARTB_DST     0x0053FE18u /* database            */
#define AR6014_PARTC_DST     0x00527000u /* stub code           */
#define AR6014_PARTD_DST     0x00524C00u /* stub data           */
#define AR6014_EXEC_MIRROR   0x00400000u
#define HTC_PROTOCOL_VERSION 0x0002u

#define HTC_HDR_LENGTH   6u
#define HTC_MSG_READY_ID 1u

/* ath6kl's cadence: one poll every 10 ms, 2 s for an answer. Polling much
 * faster starves the target. */
#define HTC_TICK_MS    10u
#define HTC_TICKS(ms)  ((int)((ms) / HTC_TICK_MS))

/* Reads 0x400-0x40B in one pass, as ath6kl does: host int, cpu int, error,
 * counter, frame, lookahead valid, two more, then the lookahead. Returns 1 with
 * the lookahead when mailbox 0 holds a frame. */
static int wifi_htc_pending(WifiShared *w, uint32_t *look) {
  WifiCmd t;
  uint8_t r[12];

  for (int i = 0; i < 12; i++) {
    wifi_cmd(w, &t, WCMD52, WCMD52_RD(1, 0x400 + i));
    r[i] = (uint8_t)(t.resp & 0xFFu);
  }
  if (look)
    *look = (uint32_t)r[8] | ((uint32_t)r[9] << 8) | ((uint32_t)r[10] << 16) |
            ((uint32_t)r[11] << 24);
  return (r[0] & 0x01u) && (r[5] & 0x01u);
}

/* INT_STATUS_ENABLE (0x418-0x41B), zeroed as ath6kl does. */
static void wifi_htc_disable_ints(WifiShared *w) {
  WifiCmd t;
  for (int i = 0; i < 4; i++)
    wifi_cmd(w, &t, WCMD52, WCMD52_WR(1, 0x418 + i, 0));
}

/* An HTC frame is the 6-byte header (endpoint, flags, payload length, two
 * control bytes) followed by the payload. A frame the target sends can carry a
 * trailer of credit and lookahead records at the end, counted inside the
 * payload length, with its size in the first control byte. */
#define HTC_EP0                    0u
#define HTC_MSG_CONNECT_SERVICE_ID 2u
#define HTC_MSG_CONNECT_RESP_ID    3u
#define HTC_MSG_SETUP_COMPLETE_ID  4u
#define HTC_FLAGS_RECV_TRAILER     0x02u
#define HTC_FRAME_MAX              256u
/* Every send and receive is padded to the block size: the target only
 * releases a frame once a whole block has moved. */
#define HTC_BLOCK 128u

static uint32_t htc_pad(uint32_t n) {
  return (n + (HTC_BLOCK - 1u)) & ~(HTC_BLOCK - 1u);
}
#define WMI_CONTROL_SVC            0x0100u
#define WMI_READY_EVENTID          0x1001u

/* Ways to write a frame. HTC_W_BYTE is ath6kl's (the mmc core uses byte mode
 * for a single block); the others are tried if it goes unanswered. The last
 * two switch the bus to 4-bit first, which is how the Linux port runs. */
enum {
  HTC_W_BYTE = 0,
  HTC_W_BLOCK,
  HTC_W_CMD52,
  HTC_W_BYTE4,
  HTC_W_BLOCK4,
  HTC_W_COUNT
};

static int wifi_htc_send(WifiShared *w, uint8_t ep, const uint8_t *payload,
                         uint32_t len, int style) {
  static uint32_t frame[HTC_FRAME_MAX / 4];
  uint8_t *b = (uint8_t *)frame;
  uint32_t padded = htc_pad(HTC_HDR_LENGTH + len);
  int block = (style == HTC_W_BLOCK || style == HTC_W_BLOCK4);
  int secure = 0;

  if (padded > HTC_FRAME_MAX)
    return -1;
  for (uint32_t i = 0; i < padded; i++)
    b[i] = 0;
  b[0] = ep;
  b[1] = 0;
  b[2] = (uint8_t)len;
  b[3] = (uint8_t)(len >> 8);
  for (uint32_t i = 0; i < len; i++)
    b[HTC_HDR_LENGTH + i] = payload[i];
  if (style == HTC_W_CMD52) {
    WifiCmd t;
    for (uint32_t i = 0; i < padded; i++)
      wifi_cmd(w, &t, WCMD52, WCMD52_WR(1, 0x1000u - padded + i, b[i]));
    return 0;
  }
  return wifi_cmd53(w, 1, 0x1000u - padded, frame, padded, 1, 1,
                    block ? HTC_BLOCK : 0u, secure);
}

/* Waits for a frame, then drains it from the mailbox. Returns the payload
 * length with any trailer removed, or 0 if nothing arrived. */
static uint32_t wifi_htc_recv(WifiShared *w, uint8_t *buf, uint32_t max,
                              int polls) {
  WifiCmd t;
  static uint8_t frame[HTC_FRAME_MAX];
  uint32_t look = 0;
  int have = 0;

  for (int i = 0; i < polls; i++) {
    if (wifi_htc_pending(w, &look)) {
      have = 1;
      break;
    }
    wifi_ms(HTC_TICK_MS);
  }
  if (!have)
    return 0;

  uint32_t len = (look >> 16) & 0xFFFFu; /* payload length */
  uint32_t total = htc_pad(HTC_HDR_LENGTH + len);
  if (total > HTC_FRAME_MAX)
    return 0; /* not a length this handshake ever produces */
  for (uint32_t i = 0; i < total; i++) {
    wifi_cmd(w, &t, WCMD52, WCMD52_RD(1, WIFI_MBOX0 + i));
    frame[i] = (uint8_t)(t.resp & 0xFFu);
  }
  w->htc_look = look;
  if ((frame[1] & HTC_FLAGS_RECV_TRAILER) && frame[4] <= len)
    len -= frame[4];
  uint32_t n = len < max ? len : max;
  for (uint32_t i = 0; i < n; i++)
    buf[i] = frame[HTC_HDR_LENGTH + i];
  return len;
}

static void wifi_htc_wait_ready(WifiShared *w) {
  WifiCmd t;
  w->htc_ready = 0;
  w->htc_look = 0;
  w->htc_msgid = 0;
  w->htc_credits = 0;
  w->htc_credsz = 0;
  w->htc_regs = 0;
  w->boot_ready = 0;

  /* 2 s for HTC_READY, restarted when the firmware sets its ready flag
   * (HI+0x58). */
  uint32_t t0 = w->timeouts;
  int have = 0;
  int budget = HTC_TICKS(2000);
  for (int i = 0; i < budget; i++) {
    if (wifi_htc_pending(w, 0)) {
      have = 1;
      break;
    }
    if (!w->boot_ready && (i % 5) == 0 &&
        wifi_diag_read(w, AR6014_HI + 0x58u) == 1u) {
      w->boot_ready = 1;
      if (budget - i < HTC_TICKS(2000))
        budget = i + HTC_TICKS(2000);
      dcache_clean();
    }
    if (w->timeouts - t0 > 8u)
      break; /* chip no longer acking register reads */
    wifi_ms(HTC_TICK_MS);
  }

  {
    uint32_t his, cpu, err, lav;
    wifi_cmd(w, &t, WCMD52, WCMD52_RD(1, 0x400));
    his = t.resp & 0xFFu;
    wifi_cmd(w, &t, WCMD52, WCMD52_RD(1, 0x401));
    cpu = t.resp & 0xFFu;
    wifi_cmd(w, &t, WCMD52, WCMD52_RD(1, 0x402));
    err = t.resp & 0xFFu;
    wifi_cmd(w, &t, WCMD52, WCMD52_RD(1, 0x405));
    lav = t.resp & 0xFFu;
    w->htc_regs = his | (cpu << 8) | (err << 16) | (lav << 24);
  }
  uint8_t hdr[4];
  for (int i = 0; i < 4; i++) {
    wifi_cmd(w, &t, WCMD52, WCMD52_RD(1, 0x408 + i));
    hdr[i] = (uint8_t)(t.resp & 0xFFu);
  }
  w->htc_look = (uint32_t)hdr[0] | ((uint32_t)hdr[1] << 8) |
                ((uint32_t)hdr[2] << 16) | ((uint32_t)hdr[3] << 24);
  if (!have) {
    /* Nothing flagged: keep what mailbox 0 and MBOX_FRAME (0x404) hold, in the
     * fw_db* fields. */
    uint32_t mb = 0;
    for (int i = 0; i < 4; i++) {
      wifi_cmd(w, &t, WCMD52, WCMD52_RD(1, WIFI_MBOX0 + i));
      mb |= (t.resp & 0xFFu) << (i * 8);
    }
    w->fw_dbrd = mb;
    wifi_cmd(w, &t, WCMD52, WCMD52_RD(1, 0x404));
    w->fw_dbex = t.resp & 0xFFu;
    return;
  }
  w->htc_ready = 1;

  /* HTC_READY: message id, credit count, credit size, endpoint count. */
  uint8_t msg[32];
  uint32_t len = wifi_htc_recv(w, msg, sizeof(msg), 4);
  if (len >= 6u) {
    w->htc_msgid = (uint32_t)msg[0] | ((uint32_t)msg[1] << 8);
    w->htc_credits = (uint32_t)msg[2] | ((uint32_t)msg[3] << 8);
    w->htc_credsz = (uint32_t)msg[4] | ((uint32_t)msg[5] << 8);
    if (w->htc_msgid == HTC_MSG_READY_ID)
      w->htc_stage = WIFI_HTC_READY;
  }
}

static void wifi_htc_snapshot(WifiShared *w) {
  WifiCmd t;
  uint32_t his, cpu, err, lav;
  wifi_cmd(w, &t, WCMD52, WCMD52_RD(1, 0x400));
  his = t.resp & 0xFFu;
  wifi_cmd(w, &t, WCMD52, WCMD52_RD(1, 0x401));
  cpu = t.resp & 0xFFu;
  wifi_cmd(w, &t, WCMD52, WCMD52_RD(1, 0x402));
  err = t.resp & 0xFFu;
  wifi_cmd(w, &t, WCMD52, WCMD52_RD(1, 0x405));
  lav = t.resp & 0xFFu;
  w->htc_regs = his | (cpu << 8) | (err << 16) | (lav << 24);
}

/* Connects the WMI control service, sends setup complete and reads WMI_READY,
 * which carries the MAC. Layouts are ath6kl's legacy HTC ones. */
static void wifi_htc_connect(WifiShared *w) {
  uint8_t msg[64];
  uint8_t req[8];
  uint32_t len;

  /* Clear error bits left latched by BMI (write the bits back, as ath6kl
   * does). */
  {
    WifiCmd t;
    wifi_cmd(w, &t, WCMD52, WCMD52_RD(1, 0x402));
    uint8_t err = (uint8_t)(t.resp & 0xFFu);
    if (err)
      wifi_cmd(w, &t, WCMD52, WCMD52_WR(1, 0x402, err));
  }

  {
    WifiCmd t;
    uint32_t his, err, lav, look = 0;
    wifi_cmd(w, &t, WCMD52, WCMD52_RD(1, 0x400));
    his = t.resp & 0xFFu;
    wifi_cmd(w, &t, WCMD52, WCMD52_RD(1, 0x402));
    err = t.resp & 0xFFu;
    wifi_cmd(w, &t, WCMD52, WCMD52_RD(1, 0x405));
    lav = t.resp & 0xFFu;
    for (int i = 0; i < 4; i++) {
      wifi_cmd(w, &t, WCMD52, WCMD52_RD(1, 0x408 + i));
      look |= (t.resp & 0xFFu) << (i * 8);
    }
    w->htc_look2 = look;
    (void)his;
    (void)err;
    (void)lav;
  }

  /* CCCR after boot: I/O enable, I/O ready, bus width and the function-1 block
   * size low byte. Enumeration leaves 0x02, 0x02, 0x00, 0x80. */
  {
    WifiCmd t;
    uint32_t v = 0;
    wifi_cmd(w, &t, WCMD52, WCMD52_RD(0, 0x02));
    v |= t.resp & 0xFFu;
    wifi_cmd(w, &t, WCMD52, WCMD52_RD(0, 0x03));
    v |= (t.resp & 0xFFu) << 8;
    wifi_cmd(w, &t, WCMD52, WCMD52_RD(0, 0x07));
    v |= (t.resp & 0xFFu) << 16;
    wifi_cmd(w, &t, WCMD52, WCMD52_RD(0, 0x110));
    v |= (t.resp & 0xFFu) << 24;
    w->htc_regs2 = v;
    dcache_clean();
  }

  /* ath6kl does host-side work here; give the firmware the same time. */
  wifi_ms(100);

  /* Drain leftovers so the reply cannot be confused with one. */
  {
    uint32_t dropped = 0, last = 0;
    for (int i = 0; i < 4; i++) {
      uint32_t n = wifi_htc_recv(w, msg, sizeof(msg), 2);
      if (!n)
        break;
      dropped++;
      last = (uint32_t)msg[0] | ((uint32_t)msg[1] << 8);
    }
    w->htc_drained = dropped | (last << 16);
    dcache_clean();
  }

  /* HTC_CONNECT_SERVICE: message id, service id, flags, meta length, pad. */
  req[0] = (uint8_t)HTC_MSG_CONNECT_SERVICE_ID;
  req[1] = 0;
  req[2] = (uint8_t)WMI_CONTROL_SVC;
  req[3] = (uint8_t)(WMI_CONTROL_SVC >> 8);
  req[4] = 0;
  req[5] = 0;
  req[6] = 0;
  req[7] = 0;
  w->htc_stage = WIFI_HTC_CONNECT;
  dcache_clean();

  /* Try each write style until one is answered. The response is message id,
   * service id, status, endpoint and max message size. */
  int mode = -1;
  for (int style = 0; style < HTC_W_COUNT && mode < 0; style++) {
    WifiCmd t;
    uint32_t his, err, lav, frm;
    int refused;

    if (style == HTC_W_BYTE4)
      wifi_bus_width(w, 1);
    for (uint32_t i = 0; i < sizeof(msg); i++)
      msg[i] = 0;
    refused = wifi_htc_send(w, HTC_EP0, req, 8, style) != 0;
    if (refused)
      w->htc_err |= 1u << style;
    len = wifi_htc_recv(w, msg, sizeof(msg),
                        (style == HTC_W_BYTE || style == HTC_W_BYTE4)
                            ? HTC_TICKS(2000)
                            : HTC_TICKS(500));
    w->htc_msg0 = (uint32_t)msg[0] | ((uint32_t)msg[1] << 8) |
                  ((uint32_t)msg[2] << 16) | ((uint32_t)msg[3] << 24);
    w->htc_try = (uint32_t)(style + 1);

    wifi_cmd(w, &t, WCMD52, WCMD52_RD(1, 0x400));
    his = t.resp & 0xFFu;
    wifi_cmd(w, &t, WCMD52, WCMD52_RD(1, 0x402));
    err = t.resp & 0xFFu;
    wifi_cmd(w, &t, WCMD52, WCMD52_RD(1, 0x405));
    lav = t.resp & 0xFFu;
    wifi_cmd(w, &t, WCMD52, WCMD52_RD(1, 0x404));
    frm = t.resp & 0xFFu;
    w->htc_snap[style] = refused ? (0x80000000u | g_cmd53_stat)
                                 : (his | (err << 8) | (lav << 16) | (frm << 24));
    dcache_clean();

    if (len >= 8u &&
        ((uint32_t)msg[0] | ((uint32_t)msg[1] << 8)) == HTC_MSG_CONNECT_RESP_ID)
      mode = style;
  }
  if (mode < 0) {
    w->htc_status = 0xFFu; /* nothing usable came back any way */
    wifi_htc_snapshot(w);
    dcache_clean();
    return;
  }
  w->htc_status = msg[4];
  w->htc_ep = msg[5];
  w->htc_maxmsg = (uint32_t)msg[6] | ((uint32_t)msg[7] << 8);
  if (w->htc_status != 0) {
    wifi_htc_snapshot(w);
    dcache_clean();
    return;
  }
  w->htc_stage = WIFI_HTC_CONNECTED;
  dcache_clean();

  /* HTC_SETUP_COMPLETE hands the mailbox from the control channel to the
   * endpoints, after which the firmware starts WMI. */
  req[0] = (uint8_t)HTC_MSG_SETUP_COMPLETE_ID;
  req[1] = 0;
  wifi_htc_send(w, HTC_EP0, req, 2, mode);
  w->htc_stage = WIFI_HTC_SETUP;
  dcache_clean();

  /* WMI_READY with the AR6014's short header: event id, MAC, PHY capability, a
   * reserved byte, firmware version. */
  len = wifi_htc_recv(w, msg, sizeof(msg), HTC_TICKS(2000));
  if (len >= 2u) {
    w->wmi_event = (uint32_t)msg[0] | ((uint32_t)msg[1] << 8);
    w->htc_stage = WIFI_HTC_WMI;
    if (w->wmi_event == WMI_READY_EVENTID && len >= 14u) {
      w->wmi_mac0 = (uint32_t)msg[2] | ((uint32_t)msg[3] << 8) |
                    ((uint32_t)msg[4] << 16) | ((uint32_t)msg[5] << 24);
      w->wmi_mac1 = (uint32_t)msg[6] | ((uint32_t)msg[7] << 8);
      w->wmi_swver = (uint32_t)msg[10] | ((uint32_t)msg[11] << 8) |
                     ((uint32_t)msg[12] << 16) | ((uint32_t)msg[13] << 24);
    }
  }

  wifi_htc_snapshot(w);
  dcache_clean();
}

static void wifi_boot_firmware(WifiShared *w, uint32_t opts) {
  const WifiFw *fw = (const WifiFw *)WIFI_FW_ADDR;
  uint32_t old_sleep = 0, old_scratch = 0;
  dcache_clean_inval(); /* pull the ARM9-staged blobs + header from RAM */
  if (fw->magic != WIFI_FW_MAGIC) {
    w->boot_step = WIFI_BOOT_NOFW;
    dcache_clean();
    return;
  }
  if ((fw->main_type != WIFI_FW_TYPE1 && fw->main_type != WIFI_FW_TYPE4) ||
      fw->main_len == 0) {
    w->boot_step = WIFI_BOOT_NOFW;
    dcache_clean();
    return;
  }
  w->fw_type = fw->main_type;

  {
    const uint8_t *sd = (const uint8_t *)WIFI_FW_STUBDATA;
    const uint8_t *sc = (const uint8_t *)WIFI_FW_STUBCODE;
    const uint8_t *mn = (const uint8_t *)WIFI_FW_MAIN;
    uint32_t sum = fw->stubdata_len + fw->stubcode_len + fw->main_len;
    for (uint32_t i = 0; i < fw->stubdata_len; i++)
      sum = (sum << 1 | sum >> 31) + sd[i];
    for (uint32_t i = 0; i < fw->stubcode_len; i++)
      sum = (sum << 1 | sum >> 31) + sc[i];
    for (uint32_t i = 0; i < fw->main_len; i++)
      sum = (sum << 1 | sum >> 31) + mn[i];
    w->fw_sum = sum;
  }
  w->clkcnt = MMIO32(0x10141300u); /* CFG11_MPCORE_CLKCNT: ARM11 clock mode */
  dcache_clean();

  wifi_bmi_target_info(w);
  if (w->bmi_ver != AR6014_VERSION || w->bmi_type != AR6014_TYPE) {
    w->boot_step = WIFI_BOOT_BADVER;
    dcache_clean();
    return;
  }

  /* 1. HTC protocol version into host interest, then the clock/sleep setup. */
  w->boot_step = WIFI_BOOT_HI;
  dcache_clean();
  wifi_bmi_write_word(w, AR6014_HI, HTC_PROTOCOL_VERSION);
  old_scratch = wifi_bmi_read_soc(w, 0x000180C0u);
  wifi_bmi_write_soc(w, 0x000180C0u, old_scratch | 0x8u);
  old_sleep = wifi_bmi_read_soc(w, 0x000040C4u);
  wifi_bmi_write_soc(w, 0x000040C4u, old_sleep | 0x1u);
  w->soc_scratch = old_scratch;
  w->soc_sleep = old_sleep;
  dcache_clean();
  wifi_bmi_write_soc(w, 0x00004028u, 0x5u);
  wifi_bmi_write_soc(w, 0x00004020u, 0x0u);
  for (volatile int d = 0; d < 2000000; d++) /* clock stabilise (~20 ms) */
    ;
  if (g_bmi_abort) {
    dcache_clean();
    return;
  }

  /* 2. Upload and run the NWM stub. */
  w->boot_step = WIFI_BOOT_STUB;
  dcache_clean();
  wifi_bmi_write_mem(w, AR6014_PARTD_DST, (const uint8_t *)WIFI_FW_STUBDATA,
                     fw->stubdata_len);
  wifi_bmi_write_mem(w, AR6014_PARTC_DST, (const uint8_t *)WIFI_FW_STUBCODE,
                     fw->stubcode_len);
  /* fw_dbrd == fw_dbex shows the stub arrived intact. */
  {
    const uint32_t *sc = (const uint32_t *)WIFI_FW_STUBCODE;
    w->fw_dbrd = wifi_diag_read(w, AR6014_PARTC_DST);
    w->fw_dbex = sc[0];
  }
  dcache_clean();
  w->boot_exec = wifi_bmi_execute(w, AR6014_PARTC_DST + AR6014_EXEC_MIRROR,
                                  AR6014_PARTC_DST);
  for (volatile int d = 0; d < 10000000; d++) /* ~100 ms for the stub */
    ;
  if (g_bmi_abort) {
    dcache_clean();
    return;
  }

  /* 3. Main firmware, then (unless NO_POST_LZ) stub data and database. */
  w->boot_step = WIFI_BOOT_MAIN;
  dcache_clean();
  wifi_bmi_fast_download(w, AR6014_PARTA_DST, (const uint8_t *)WIFI_FW_MAIN,
                         fw->main_len);
  if (!(opts & WIFI_OPT_NO_POST_LZ)) {
    /* stub_data goes back over the start of what the LZ stream just wrote:
     * both land at 0x524C00. The Linux port writes neither this nor the
     * database, and says the stub reads the calibration EEPROM itself. */
    wifi_bmi_write_mem(w, AR6014_PARTD_DST, (const uint8_t *)WIFI_FW_STUBDATA,
                       fw->stubdata_len);
    wifi_bmi_write_mem(w, AR6014_PARTB_DST, (const uint8_t *)WIFI_FW_DATABASE,
                       fw->database_len);
    wifi_bmi_write_word(w, AR6014_HI + 0x18u, AR6014_PARTB_DST);
  }
  if (opts & WIFI_OPT_RESTORE_SOC) {
    /* nocash's sdio_bmi_finish restores the scratch register and lets the chip
     * sleep (bit 0 clear). The ROM leaves sleep disabled, and a chip that
     * dozes between our slow-bus commands stops answering, so it stays awake
     * unless WIFI_OPT_SLEEP_ON asks for nocash's value. */
    uint32_t sleep = (opts & WIFI_OPT_SLEEP_ON) ? (old_sleep & ~0x1u)
                                                : (old_sleep | 0x1u);
    wifi_bmi_write_soc(w, 0x000040C4u, sleep);
    wifi_bmi_write_soc(w, 0x000180C0u, old_scratch);
  }
  wifi_bmi_write_word(w, AR6014_HI + 0x6Cu, 0x80u);
  wifi_bmi_write_word(w, AR6014_HI + 0x74u, 0x63u);
  if (g_bmi_abort) {
    dcache_clean();
    return;
  }

  if (!(opts & WIFI_OPT_NO_INT_EN))
    wifi_htc_disable_ints(w);

  /* 4. Hand over, then watch for HTC_READY at once: the AR6014 gives up if the
   * host answers late. */
  wifi_bmi_done(w);
  w->boot_step = WIFI_BOOT_DONE;
  dcache_clean();
  wifi_htc_wait_ready(w);
  dcache_clean();

  if (w->htc_msgid == HTC_MSG_READY_ID)
    wifi_htc_connect(w);
  dcache_clean();
}

static void wifi_boot_run(uint32_t opts) {
  WifiShared *w = (WifiShared *)WIFI_SHARED_ADDR;
  w->boot_opts = opts;
  g_bmi_abort = 0;
  w->bmi_polls = 0;
  w->bmi_extra = 0;
  w->bmi_bc = 0;
  w->soc_sleep = 0;
  w->soc_scratch = 0;
  w->hb = 0;
  w->boot_step = WIFI_BOOT_NONE;
  w->boot_exec = 0;
  w->boot_ready = 0;
  w->bmi_sends = 0;
  w->bmi_nocred = 0;
  w->fw_chk = 0;
  w->fw_dbrd = 0;
  w->fw_dbex = 0;
  w->fw_type = 0;
  w->htc_ready = 0;
  w->htc_look = 0;
  w->htc_msgid = 0;
  w->htc_credits = 0;
  w->htc_credsz = 0;
  w->htc_regs = 0;
  w->htc_ep = 0;
  w->htc_status = 0;
  w->htc_maxmsg = 0;
  w->htc_stage = WIFI_HTC_NONE;
  w->wmi_event = 0;
  w->wmi_mac0 = 0;
  w->wmi_mac1 = 0;
  w->wmi_swver = 0;
  w->htc_regs2 = 0;
  w->htc_look2 = 0;
  w->htc_drained = 0;
  w->htc_msg0 = 0;
  w->htc_try = 0;
  w->htc_err = 0;
  w->boot_tries = 1;
  w->trace = 0;
  w->fail_cmd = 0;
  w->fail_arg = 0;
  w->fail_stat = 0;
  w->fail_at = 0;
  for (int i = 0; i < 5; i++)
    w->htc_snap[i] = 0;
  wifi_bringup(w, 0);
  wifi_boot_firmware(w, opts);

  /* A stalled upload is redone once, from a cold start. */
  if (w->boot_ready != 1) {
    w->boot_tries = 2;
    g_bmi_abort = 0;
    w->bmi_sends = 0;
    w->bmi_nocred = 0;
    w->bmi_polls = 0;
    w->bmi_extra = 0;
    dcache_clean();
    wifi_bringup(w, 1);
    wifi_boot_firmware(w, opts);
  }
  wifi_finish(w);
}


void wifi11_probe(void) { wifi_probe_run(); }
void wifi11_boot(uint32_t opts) { wifi_boot_run(opts); }
