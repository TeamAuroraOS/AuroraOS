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
#include "crypto.h"
#include "net11.h"
#include "wifi.h"
#include "wpa11.h"

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

/* This core's MPCore private timer runs free for the length of a Wi-Fi
 * command, which nothing else needs meanwhile. With a prescaler of 134 it
 * counts microseconds at 268 MHz. */
#define PTIMER_LOAD  0x17E00600u
#define PTIMER_COUNT 0x17E00604u
#define PTIMER_CNT   0x17E00608u
#define PTIMER_STAT  0x17E0060Cu
#define PTIMER_RUN_US ((133u << 8) | 2u | 1u) /* prescaler, reload, enable */

static uint32_t wifi_now(void) { return ~MMIO32(PTIMER_COUNT); }

static void wifi_wait_us(uint32_t us) {
  uint32_t t0 = wifi_now();
  while (wifi_now() - t0 < us)
    ;
}

static void wifi_timer_start(void) {
  MMIO32(PTIMER_CNT) = 0;
  MMIO32(PTIMER_STAT) = 1;
  MMIO32(PTIMER_LOAD) = 0xFFFFFFFFu;
  MMIO32(PTIMER_CNT) = PTIMER_RUN_US;
}

static void wifi_timer_stop(void) {
  MMIO32(PTIMER_CNT) = 0;
  MMIO32(PTIMER_STAT) = 1;
}

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

/* Commands that failed (a timeout, a CRC error), since the core started. */
static uint32_t g_bus_errs;

static void wifi_note_fail(WifiShared *w, uint16_t cmd, uint32_t arg,
                           uint16_t s0, uint16_t s1) {
  g_bus_errs++;
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
  /* A command timeout ends with CMDRESPEND set too, and RESP reads 0. */
  e->ok = ((s0 & WSTAT0_CMDRESPEND) && !(s1 & WMASK_GW)) ? 1u : 2u;
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
  if (w->bmi_wr != 0) {
    return;
  }

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

static void wifi_step(WifiShared *w, uint32_t step) {
  w->boot_step = step;
}

static void wifi_stage(WifiShared *w, uint32_t stage) {
  w->htc_stage = stage;
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

  /* CCCR 0x00 SDIO revision and 0x08 card capability, for the command log
   * on the Wi-Fi Test screen. */
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
  w->phase = WIFI_PH_DONE;
  wifi_timer_stop();
  w->seq++;
  dcache_clean();
}

static void wifi_wmi_reset(WifiShared *w);

static void wifi_probe_run(void) {
  WifiShared *w = (WifiShared *)WIFI_SHARED_ADDR;
  wifi_timer_start();
  wifi_wmi_reset(w);
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
/* Counter 0 is the firmware's debug interrupt: it asserted (ath6kl's
 * ATH6KL_TARGET_DEBUG_INTR_MASK). Nothing more comes from it after that. */
static int g_fw_asserted;

static int wifi_htc_pending(WifiShared *w, uint32_t *look) {
  WifiCmd t;
  uint8_t r[12];

  for (int i = 0; i < 12; i++) {
    wifi_cmd(w, &t, WCMD52, WCMD52_RD(1, 0x400 + i));
    r[i] = (uint8_t)(t.resp & 0xFFu);
  }
  if ((r[3] & 0x01u) && t.ok == 1u && !g_fw_asserted) {
    g_fw_asserted = 1;
    w->fw_assert = 1;
    w->fw_assert_t = wifi_now();
    dcache_clean();
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
#define HTC_REC_CREDIT             1u    /* trailer record: (endpoint, credits) pairs */
/* The largest frame: a 1538-byte message, the WMI endpoint's maximum, with
 * its header, padded to whole blocks. */
#define HTC_FRAME_MAX              1664u
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

static int wifi_htc_send(WifiShared *w, uint8_t ep, uint8_t flags,
                         const uint8_t *payload, uint32_t len, int style) {
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
  b[1] = flags;
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

/* Set by wifi_htc_recv when it read a frame, even one with no payload left
 * once its trailer is removed, and the endpoint that frame came on. */
static int g_htc_got;
static uint8_t g_htc_rx_ep;

/* Once joining starts, a poll waits this long by the timer instead of
 * wifi_ms(HTC_TICK_MS), which runs several times longer than it says. */
static uint32_t g_tick_us;

/* Trailer records: id, length, data. A credit report returns credits to the
 * endpoints it names; the WMI endpoint's go to htc_credit. */
static void wifi_htc_trailer(WifiShared *w, const uint8_t *t, uint32_t n) {
  uint32_t i = 0;
  while (i + 2u <= n) {
    uint32_t id = t[i], rl = t[i + 1];
    if (i + 2u + rl > n)
      break;
    if (id == HTC_REC_CREDIT) {
      for (uint32_t k = 0; k + 2u <= rl; k += 2) {
        uint32_t ep = t[i + 2 + k], cr = t[i + 3 + k];
        if (ep != HTC_EP0 && w->htc_stage >= WIFI_HTC_CONNECTED)
          w->htc_credit += cr;
      }
    }
    i += 2u + rl;
  }
}

/* Network operations take frames a block per CMD53 on a faster clock; the
 * boot and the join keep a byte per CMD52 at 523 kHz, about 0.3 s for a full
 * frame. The clock goes as high as the card's registers still read right,
 * and a command failing on it steps it down for the rest of the core's life.
 * A CMD53 read failing on the slow clock goes back to CMD52. */
static const uint8_t bus_divs[] = {0x01, 0x02, 0x04, 0x08}; /* 16.8 to 2.1 MHz */
#define BUS_DIVS sizeof(bus_divs)
#define BUS_SLOW 0x20u
static uint32_t g_bus_step; /* into bus_divs; BUS_DIVS: BUS_SLOW only */
static int g_net_bus;       /* in a network operation, on a 1-bit bus */
static int g_rx53_bad;

static uint32_t bus_div(void) { return WB16(WR_CLKCTL) & 0xFFu; }

static int bus_fast(void) { return bus_div() != BUS_SLOW; }

/* Linux's tmio driver waits 10 ms after starting the clock. */
static void bus_clock(uint32_t div) {
  wifi_setckl(div);
  wifi_wait_us(10000);
}

/* A command failed on the fast clock (a failed CMD53 has already reset the
 * controller to the slow one): the next step down, from now on. */
static void wifi_bus_down(void) {
  if (g_bus_step < BUS_DIVS)
    g_bus_step++;
  bus_clock(g_bus_step < BUS_DIVS ? bus_divs[g_bus_step] : BUS_SLOW);
}

/* The CCCR revision, bus interface and card capability, and function 1's
 * block size (128): a clock the bus cannot take reads them wrong, or fails. */
static uint32_t bus_sig(WifiShared *w) {
  static const uint16_t reg[4] = {0x00, 0x07, 0x08, 0x110};
  WifiCmd t;
  uint32_t s = 0;
  for (int i = 0; i < 4; i++) {
    wifi_cmd(w, &t, WCMD52, WCMD52_RD(0, reg[i]));
    s = (s << 8) | (t.resp & 0xFFu);
  }
  return s;
}

/* `total` bytes of a frame, a block per CMD53 in byte mode, as frames go out.
 * Returns how many left the mailbox: a command that timed out never reached
 * the chip, while after any other failure its block is gone. */
static uint32_t wifi_htc_read53(WifiShared *w, uint8_t *frame,
                                uint32_t total) {
  for (uint32_t off = 0; off < total; off += HTC_BLOCK)
    if (wifi_cmd53(w, 1, WIFI_MBOX0 + off, (uint32_t *)(frame + off),
                   HTC_BLOCK, 0, 1, 0, 0) != 0)
      return (g_cmd53_stat >> 16) & 0x0040u ? off : off + HTC_BLOCK;
  return total;
}

/* Waits for a frame, then drains it from the mailbox. Returns the payload
 * length with any trailer removed, or 0 if nothing arrived. */
static uint32_t wifi_htc_recv(WifiShared *w, uint8_t *buf, uint32_t max,
                              int polls) {
  WifiCmd t;
  static uint32_t frame32[HTC_FRAME_MAX / 4];
  uint8_t *frame = (uint8_t *)frame32;
  uint32_t look = 0, errs = g_bus_errs, got = 0;
  int have = 0;

  g_htc_got = 0;
  for (int i = 0; i < polls; i++) {
    if (wifi_htc_pending(w, &look)) {
      have = 1;
      break;
    }
    if (g_fw_asserted)
      break;
    if (g_tick_us)
      wifi_wait_us(g_tick_us);
    else
      wifi_ms(HTC_TICK_MS);
  }
  /* A lookahead read wrong would drain the wrong number of bytes. */
  if (g_net_bus && g_bus_errs != errs && bus_fast()) {
    wifi_bus_down();
    return 0;
  }
  if (!have)
    return 0;

  uint32_t len = (look >> 16) & 0xFFFFu; /* payload length */
  uint32_t total = htc_pad(HTC_HDR_LENGTH + len);
  if (total > HTC_FRAME_MAX) {
    return 0; /* not a length this handshake ever produces */
  }
  int fast = bus_fast(), r53 = g_net_bus && !g_rx53_bad;
  errs = g_bus_errs;
  if (r53)
    got = wifi_htc_read53(w, frame, total);
  for (uint32_t i = got; i < total; i++) {
    wifi_cmd(w, &t, WCMD52, WCMD52_RD(1, WIFI_MBOX0 + i));
    frame[i] = (uint8_t)(t.resp & 0xFFu);
  }
  /* ath6kl checks a frame's header against its lookahead too. */
  if ((fast || r53) && (g_bus_errs != errs || frame32[0] != look)) {
    if (fast)
      wifi_bus_down();
    else
      g_rx53_bad = 1;
    return 0;
  }
  w->htc_look = look;
  g_htc_got = 1;
  g_htc_rx_ep = frame[0];
  if ((frame[1] & HTC_FLAGS_RECV_TRAILER) && frame[4] <= len) {
    wifi_htc_trailer(w, frame + HTC_HDR_LENGTH + len - frame[4], frame[4]);
    len -= frame[4];
  }
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
      wifi_stage(w, WIFI_HTC_READY);
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

/* WMI on NWM's firmware: every command and event starts with a bare u16 id,
 * with none of ath6kl's info1 field (Octoblimp's port, from NWM's
 * disassembly). Ids and layouts are the legacy ath6kl ones. */
#define WMI_CONNECT_CMDID          1u
#define WMI_DISCONNECT_CMDID       3u
#define WMI_ADD_CIPHER_KEY_CMDID   22u
#define WMI_START_SCAN_CMDID       7u
#define WMI_SET_SCAN_PARAMS_CMDID  8u
#define WMI_SET_BSS_FILTER_CMDID   9u
#define WMI_SET_PROBED_SSID_CMDID  10u
#define WMI_SET_CHANNEL_PARAMS_CMDID 17u
#define WMI_SET_POWER_MODE_CMDID   18u
#define WMI_CONNECT_EVENTID        0x1002u
#define WMI_DISCONNECT_EVENTID     0x1003u
#define WMI_BSSINFO_EVENTID        0x1004u
#define WMI_CMDERROR_EVENTID       0x1005u
#define WMI_SCAN_COMPLETE_EVENTID  0x100Au

static void put16(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v) {
  put16(p, v);
  put16(p + 2, v >> 16);
}

static void wifi_wmi_reset(WifiShared *w) {
  for (uint32_t i = 0; i < 4; i++) {
    w->svc_ep[i] = 0;
    w->svc_status[i] = 0;
  }
  w->fw_assert = 0;
  w->fw_assert_t = 0;
  w->wmi_stage = WIFI_WMI_NONE;
  w->wmi_events = 0;
  w->wmi_last = 0;
  w->wmi_cmderr = 0;
  w->wmi_errors = 0;
  w->scan_status = 0;
  w->htc_credit = 0;
  w->bss_seen = 0;
  w->bss_count = 0;
  w->conn_stage = WIFI_CONN_NONE;
  w->conn_tries = 0;
  w->conn_reason = 0;
  w->conn_channel = 0;
  w->conn_rssi = 0;
  w->conn_caps = 0;
  for (uint32_t i = 0; i < 6; i++) {
    w->conn_bssid[i] = 0;
    w->gw_mac[i] = 0;
  }
  w->ip = 0;
  w->mask = 0;
  w->gw = 0;
  w->dns = 0;
  w->dhcp_server = 0;
  w->lease = 0;
  w->ping_sent = 0;
  w->ping_ok = 0;
  w->ping_best_us = 0;
  w->tx_frames = 0;
  w->rx_frames = 0;
  w->session = 0;
  w->sec = 0;
  w->wpa_group = 0;
  w->wpa_m1 = 0;
  w->wpa_m3 = 0;
  w->wpa_g1 = 0;
  w->wpa_bad_mic = 0;
  w->wpa_variant = 0;
  w->wpa_keys = 0;
}

static void wifi_wmi_stage(WifiShared *w, uint32_t stage) {
  w->wmi_stage = stage;
  dcache_clean();
}

/* The network to join, copied from WifiReq before the scan so that the scan
 * can pick it out: the strongest BSS with that SSID. */
static uint8_t g_want[32];
static uint32_t g_want_len;
static int g_have_pmk; /* a password was given; the PMK stays in WifiReq */
static int g_tgt_ok;
static uint8_t g_tgt_bssid[6];
static uint32_t g_tgt_channel, g_tgt_caps, g_tgt_sec, g_tgt_group;
static int32_t g_tgt_rssi;

/* bss[] full: a network is still parsed here so that the join can find it. */
static WifiBss g_spare;

/* 1 once a CONNECT event came, -1 after a DISCONNECT for any reason but our
 * own WMI_DISCONNECT, which only counts in g_disc. */
static volatile int g_link;
static volatile int g_disc;

static void wifi_join_see(const WifiBss *b, uint32_t group) {
  if (!g_want_len || b->ssid_len != g_want_len)
    return;
  for (uint32_t k = 0; k < g_want_len; k++)
    if (b->ssid[k] != g_want[k])
      return;
  if (g_tgt_ok && b->rssi <= g_tgt_rssi)
    return;
  g_tgt_ok = 1;
  for (uint32_t k = 0; k < 6; k++)
    g_tgt_bssid[k] = b->bssid[k];
  g_tgt_channel = b->channel;
  g_tgt_caps = b->caps;
  g_tgt_rssi = b->rssi;
  g_tgt_sec = b->sec;
  g_tgt_group = group;
}

static uint32_t le16(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }

/* An RSN element's body: version, group cipher, pairwise ciphers, AKMs, each
 * suite 00-0F-AC and a type. Omitted lists default to CCMP and 802.1X. */
static uint32_t wifi_rsn(const uint8_t *v, uint32_t len, uint32_t *group) {
  uint32_t off = 2, ccmp = 1, psk = 0, sae = 0, n;
  *group = 4;
  if (len < 2u || le16(v) != 1u)
    return WIFI_SEC_EAP;
  if (off + 4u <= len) {
    *group = v[off + 3];
    off += 4u;
  }
  if (off + 2u <= len) {
    n = le16(v + off);
    off += 2u;
    ccmp = 0;
    for (uint32_t i = 0; i < n && off + 4u <= len; i++, off += 4u)
      if (v[off] == 0x00 && v[off + 1] == 0x0F && v[off + 2] == 0xAC &&
          v[off + 3] == 4)
        ccmp = 1;
  }
  if (off + 2u <= len) {
    n = le16(v + off);
    off += 2u;
    for (uint32_t i = 0; i < n && off + 4u <= len; i++, off += 4u)
      if (v[off] == 0x00 && v[off + 1] == 0x0F && v[off + 2] == 0xAC) {
        if (v[off + 3] == 2)
          psk = 1;
        else if (v[off + 3] == 8)
          sae = 1;
      }
  }
  if (psk)
    return ccmp ? WIFI_SEC_WPA2 : WIFI_SEC_TKIP;
  return sae ? WIFI_SEC_WPA3 : WIFI_SEC_EAP;
}

/* BSSINFO in NWM's layout: channel, frame type, SNR, RSSI, BSSID and IE mask
 * (16 bytes), then the beacon or probe-response body: timestamp, interval and
 * capability (12 bytes), then the information elements, SSID first. */
static void wifi_bss(WifiShared *w, const uint8_t *p, uint32_t len) {
  w->bss_seen++;
  if (len < 16u + 12u)
    return;
  uint32_t i;
  for (i = 0; i < w->bss_count; i++) {
    uint32_t k = 0;
    while (k < 6 && w->bss[i].bssid[k] == p[6 + k])
      k++;
    if (k == 6)
      break;
  }
  WifiBss *b = i < WIFI_BSS_MAX ? &w->bss[i] : &g_spare;
  if (i == w->bss_count) {
    if (i < WIFI_BSS_MAX)
      w->bss_count++;
    b->seen = 0;
    b->ssid_len = 0;
  }
  for (uint32_t k = 0; k < 6; k++)
    b->bssid[k] = p[6 + k];
  b->channel = (uint16_t)(p[0] | (p[1] << 8));
  b->frame_type = p[2];
  b->snr = p[3];
  b->rssi = (int16_t)(p[4] | (p[5] << 8));
  if (b->seen < 255)
    b->seen++;
  const uint8_t *body = p + 16;
  uint32_t blen = len - 16u, group = 0, rsn = 0, wpa = 0;
  b->caps = (uint16_t)(body[10] | (body[11] << 8));
  b->sec = (b->caps & 0x10u) ? WIFI_SEC_WEP : WIFI_SEC_OPEN;
  for (uint32_t ie = 12; ie + 2u <= blen;) {
    uint32_t id = body[ie], il = body[ie + 1];
    const uint8_t *v = body + ie + 2;
    if (ie + 2u + il > blen)
      break;
    if (id == 0) {
      uint32_t n = il < 32u ? il : 32u;
      b->ssid_len = (uint8_t)n;
      for (uint32_t k = 0; k < n; k++)
        b->ssid[k] = v[k];
    } else if (id == 48) {
      b->sec = (uint8_t)wifi_rsn(v, il, &group);
      rsn = 1;
    } else if (id == 0xDD && il >= 4u && v[0] == 0x00 && v[1] == 0x50 &&
               v[2] == 0xF2 && v[3] == 1) {
      wpa = 1;
    }
    ie += 2u + il;
  }
  if (!rsn && wpa && (b->caps & 0x10u))
    b->sec = WIFI_SEC_WPA1;
  wifi_join_see(b, group);
}

static void wifi_wmi_event(WifiShared *w, const uint8_t *p, uint32_t len) {
  uint32_t id = (uint32_t)p[0] | ((uint32_t)p[1] << 8);
  const uint8_t *d = p + 2;
  uint32_t dl = len - 2u;
  w->wmi_events++;
  w->wmi_last = id;
  if (id == WMI_BSSINFO_EVENTID) {
    wifi_bss(w, d, dl);
  } else if (id == WMI_CMDERROR_EVENTID && dl >= 3u) {
    w->wmi_cmderr = (uint32_t)d[0] | ((uint32_t)d[1] << 8) | ((uint32_t)d[2] << 16);
    w->wmi_errors++;
  } else if (id == WMI_SCAN_COMPLETE_EVENTID) {
    w->scan_status = dl >= 4u ? ((uint32_t)d[0] | ((uint32_t)d[1] << 8) |
                                 ((uint32_t)d[2] << 16) | ((uint32_t)d[3] << 24))
                              : 0u;
  } else if (id == WMI_CONNECT_EVENTID && dl >= 8u) {
    /* channel, BSSID, listen and beacon interval, network type, IE lengths */
    w->conn_channel = (uint32_t)d[0] | ((uint32_t)d[1] << 8);
    for (uint32_t k = 0; k < 6; k++)
      w->conn_bssid[k] = d[2 + k];
    g_link = 1;
  } else if (id == WMI_DISCONNECT_EVENTID && dl >= 9u) {
    /* 802.11 status or reason, BSSID, the firmware's reason */
    uint32_t reason = d[8];
    w->conn_reason = reason | (((uint32_t)d[0] | ((uint32_t)d[1] << 8)) << 16);
    g_disc++;
    if (reason != 3u) /* DISCONNECT_CMD: ours */
      g_link = -1;
  }
  dcache_clean();
}

static void wifi_data_rx(WifiShared *w, const uint8_t *p, uint32_t len);

static int wifi_is_data_ep(const WifiShared *w, uint32_t ep) {
  for (uint32_t i = 0; i < 4; i++)
    if (w->svc_status[i] == 0 && w->svc_ep[i] == ep && ep != 0)
      return 1;
  return 0;
}

/* Takes frames until event `until` arrives (0: until none comes), giving each
 * up to `ticks` polls to show up. Returns 1 when `until` came.
 *
 * A frame on a data endpoint starts with NWM's two-byte data prefix (RSSI,
 * info). The firmware also sends some WMI events that way, large probe
 * responses among them: info bits 7:6 = 2 (the type ath6kl calls ACL), and a
 * WMI event id right behind the prefix. Those are taken as events. Type 0 is
 * an 802.3 frame for the IP stack. */
static int wifi_wmi_events(WifiShared *w, uint32_t until, int ticks,
                           int frames) {
  static uint8_t ev[HTC_FRAME_MAX];
  for (int n = 0; n < frames; n++) {
    uint32_t len = wifi_htc_recv(w, ev, sizeof(ev), ticks);
    if (!g_htc_got)
      return 0;
    const uint8_t *e = ev;
    if (g_htc_rx_ep != w->htc_ep) {
      if (!wifi_is_data_ep(w, g_htc_rx_ep) || len < 4u)
        continue;
      uint32_t id = (uint32_t)ev[2] | ((uint32_t)ev[3] << 8);
      int is_ev = (ev[1] >> 6) == 2u && id >= 0x1001u && id < 0x1040u;
      if (!is_ev) {
        if ((ev[1] >> 6) == 0u && len <= sizeof(ev))
          wifi_data_rx(w, ev + 2, len - 2u);
        continue;
      }
      e = ev + 2;
      len -= 2u;
    } else if (len < 2u) {
      continue;
    }
    wifi_wmi_event(w, e, len);
    if (until && ((uint32_t)e[0] | ((uint32_t)e[1] << 8)) == until)
      return 1;
  }
  return 0;
}

/* One command on the WMI endpoint, after the events that may already be
 * waiting. A command takes one credit; with none left the target has no
 * buffer for it, so it is not sent. With few left the frame asks the target
 * for a credit report (HTC_FLAGS_NEED_CREDIT_UPDATE), as ath6kl does. */
static int wifi_wmi_send(WifiShared *w, uint32_t cmd, const uint8_t *payload,
                         uint32_t len, int style) {
  uint8_t b[2 + 64];
  if (len > 64u || g_fw_asserted)
    return -1;
  wifi_wmi_events(w, 0, 3, 8);
  if (!w->htc_credit) {
    return -1;
  }
  put16(b, cmd);
  for (uint32_t i = 0; i < len; i++)
    b[2 + i] = payload[i];
  int r = wifi_htc_send(w, (uint8_t)w->htc_ep, w->htc_credit <= 2u ? 0x01u : 0u,
                        b, 2u + len, style);
  if (r == 0)
    w->htc_credit--;
  dcache_clean();
  return r;
}

/* A discovery scan of the 13 2.4 GHz 802.11g channels, in the order and with
 * the values Octoblimp's port found to work on this firmware: radio kept at
 * full power, a wildcard probe in slot 1, default dwell times with three
 * probes per SSID, an explicit channel table and every beacon reported. */
static const uint16_t chans[13] = {2412, 2417, 2422, 2427, 2432, 2437, 2442,
                                   2447, 2452, 2457, 2462, 2467, 2472};

static void wifi_wmi_scan(WifiShared *w, int style, int passive) {
  uint8_t c[64];
  int bad = 0;

  w->scan_status = 0xFFFFFFFFu;
  wifi_wmi_stage(w, WIFI_WMI_SETUP);

  c[0] = 2; /* MAX_PERF_POWER */
  bad |= wifi_wmi_send(w, WMI_SET_POWER_MODE_CMDID, c, 1, style);

  for (uint32_t i = 0; i < 35; i++)
    c[i] = 0;
  c[0] = 1; /* entry */
  c[1] = 2; /* ANY_SSID_FLAG */
  bad |= wifi_wmi_send(w, WMI_SET_PROBED_SSID_CMDID, c, 35, style);

  put16(c + 0, 0xFFFF); /* foreground start period, s */
  put16(c + 2, 0xFFFF); /* foreground end period */
  put16(c + 4, 0xFFFF); /* background period */
  put16(c + 6, 0);      /* max active dwell, ms: target default */
  put16(c + 8, 0);      /* passive dwell */
  c[10] = 3;            /* short scans per long */
  /* DEFAULT_SCAN_CTRL_FLAGS; passive drops ACTIVE_SCAN_CTRL_FLAGS (0x04). */
  c[11] = passive ? 0x2B : 0x2F;
  put16(c + 12, 0);     /* min active dwell */
  put16(c + 14, passive ? 0u : 3u); /* probes per SSID */
  put32(c + 16, 0);     /* max DFS channel active time */
  bad |= wifi_wmi_send(w, WMI_SET_SCAN_PARAMS_CMDID, c, 20, style);

  c[0] = 0;
  c[1] = 0;  /* scanParam */
  c[2] = 2;  /* WMI_11G_MODE */
  c[3] = 13; /* channels */
  for (uint32_t i = 0; i < 13; i++)
    put16(c + 4 + i * 2, chans[i]);
  bad |= wifi_wmi_send(w, WMI_SET_CHANNEL_PARAMS_CMDID, c, 30, style);

  c[0] = 1; /* ALL_BSS_FILTER */
  c[1] = 0;
  put16(c + 2, 0);
  put32(c + 4, 0); /* IE mask */
  bad |= wifi_wmi_send(w, WMI_SET_BSS_FILTER_CMDID, c, 8, style);

  put32(c + 0, 0);  /* force foreground scan */
  put32(c + 4, 0);  /* legacy (Cisco) */
  put32(c + 8, 0);  /* home dwell */
  put32(c + 12, 0); /* forced scan interval */
  c[16] = 0;        /* WMI_LONG_SCAN */
  c[17] = 13;
  for (uint32_t i = 0; i < 13; i++)
    put16(c + 18 + i * 2, chans[i]);
  if (bad || wifi_wmi_send(w, WMI_START_SCAN_CMDID, c, 44, style) != 0) {
    wifi_wmi_events(w, 0, 3, 8);
    return;
  }
  wifi_wmi_stage(w, WIFI_WMI_SCAN);

  if (wifi_wmi_events(w, WMI_SCAN_COMPLETE_EVENTID, HTC_TICKS(2000), 96))
    wifi_wmi_stage(w, WIFI_WMI_DONE);
  else
    wifi_wmi_stage(w, WIFI_WMI_TIMEOUT);
}

/* Joining the WifiReq network (open or WPA2-PSK), then DHCP, ARP for
 * the gateway and four pings to it. With WIFI_OPT_STAY the link stays up as a
 * session that AUDIO_CMD_WIFI_NET commands use. */
static int g_style;
static int g_net_on;
static int g_session;
static NetState g_net;
static uint32_t g_net_want;    /* 1 << NET_RX_* that end the wait */
static volatile int g_net_hit; /* one of them came */
static uint32_t g_ping_seq, g_ping_t;
/* What ended the last ping wait: NET_RX_PING_REPLY or NET_RX_PING_ERR, with
 * the round trip and the ICMP error; and the last DNS answer. */
static int g_ping_got;
static uint32_t g_ping_rtt, g_ping_err, g_dns_ip;

/* WPA2: the handshake state, from wpa_begin to the end of the join or the
 * session; g_wpa_hit once message 3 has been answered. */
static WpaState g_wpa;
static int g_wpa_on;
static volatile int g_wpa_hit;

#define NET_BIT(t) (1u << (t))

static void wifi_join_reset(uint32_t opts) {
  const WifiReq *q = (const WifiReq *)WIFI_REQ_ADDR;
  g_want_len = 0;
  g_have_pmk = 0;
  g_tgt_ok = 0;
  if (g_wpa_on)
    wpa_end(&g_wpa);
  g_wpa_on = 0;
  g_link = 0;
  g_disc = 0;
  g_tick_us = 0;
  g_net_on = 0;
  g_session = 0;
  if (!(opts & WIFI_OPT_CONNECT))
    return;
  dcache_clean_inval(); /* the ARM9 wrote the request */
  if (q->magic != WIFI_REQ_MAGIC || q->ssid_len == 0 || q->ssid_len > 32u)
    return;
  g_want_len = q->ssid_len;
  for (uint32_t k = 0; k < g_want_len; k++)
    g_want[k] = q->ssid[k];
  g_have_pmk = q->has_pmk == 1u;
}

static void wifi_conn_stage(WifiShared *w, uint32_t stage) {
  w->conn_stage = stage;
  dcache_clean();
}

/* Takes frames for up to `ms` until *stop is set, the link drops or the
 * firmware asserts. */
static void wifi_pump(WifiShared *w, uint32_t ms, volatile int *stop) {
  uint32_t t0 = wifi_now();
  while (!*stop && g_link >= 0 && !g_fw_asserted &&
         wifi_now() - t0 < ms * 1000u)
    wifi_wmi_events(w, 0, 1, 4);
}

/* An Ethernet II frame out on the best-effort endpoint, the way ath6kl hands
 * data to the firmware: the data prefix (802.3 data, priority 0), then the
 * frame as 802.3 with an LLC/SNAP header carrying the type. */
static int wifi_data_send(WifiShared *w, const uint8_t *f, uint32_t len) {
  static uint8_t b[2u + NET_TX_MAX + 8u];
  if (len < 14u || len > NET_TX_MAX || g_fw_asserted)
    return -1;
  if (!w->htc_credit) {
    return -1;
  }
  uint32_t pl = len - 14u;
  b[0] = 0;
  b[1] = 0;
  for (uint32_t i = 0; i < 12; i++)
    b[2 + i] = f[i];
  b[14] = (uint8_t)((pl + 8u) >> 8);
  b[15] = (uint8_t)(pl + 8u);
  b[16] = 0xAA; /* LLC: SNAP */
  b[17] = 0xAA;
  b[18] = 0x03;
  b[19] = 0; /* OUI 0: the type is an Ethernet type */
  b[20] = 0;
  b[21] = 0;
  b[22] = f[12];
  b[23] = f[13];
  for (uint32_t i = 0; i < pl; i++)
    b[24 + i] = f[14 + i];
  int r = wifi_htc_send(w, w->svc_ep[0], w->htc_credit <= 2u ? 0x01u : 0u, b,
                        24u + pl, g_style);
  if (r == 0) {
    w->htc_credit--;
    w->tx_frames++;
  }
  return r;
}

/* An 802.3 frame from the firmware, after the data prefix: back to Ethernet II
 * for the IP stack, and out with any answer it owes. */
static void wifi_wpa_rx(WifiShared *w, const uint8_t *f, uint32_t n);

static void wifi_data_rx(WifiShared *w, const uint8_t *p, uint32_t len) {
  static uint8_t f[HTC_FRAME_MAX], reply[NET_FRAME_MAX];
  uint32_t n = len, rl, v;
  if (len < 14u)
    return;
  w->rx_frames++;
  if (!g_net_on && !g_wpa_on)
    return;
  for (uint32_t i = 0; i < 12; i++)
    f[i] = p[i];
  if ((((uint32_t)p[12] << 8) | p[13]) < 0x600u) {
    if (len < 22u || p[14] != 0xAA || p[15] != 0xAA || p[16] != 0x03 ||
        p[17] || p[18] || p[19])
      return;
    n = len - 8u;
    f[12] = p[20];
    f[13] = p[21];
    for (uint32_t i = 14; i < n; i++)
      f[i] = p[i + 8];
  } else {
    for (uint32_t i = 12; i < n; i++)
      f[i] = p[i];
  }
  if (f[12] == 0x88 && f[13] == 0x8E) { /* EAPOL */
    wifi_wpa_rx(w, f, n);
    return;
  }
  if (!g_net_on)
    return;

  int t = net_rx(&g_net, f, n, reply, &rl, &v);
  if (t == NET_RX_DNS) {
    g_dns_ip = v;
  } else if (t == NET_RX_PING_REPLY || t == NET_RX_PING_ERR) {
    uint32_t seq = t == NET_RX_PING_REPLY ? v : v >> 16;
    if (seq == g_ping_seq && !g_net_hit) {
      g_ping_got = t;
      g_ping_rtt = wifi_now() - g_ping_t;
      g_ping_err = v & 0xFFFFu;
      if (t == NET_RX_PING_REPLY && !g_session) { /* the join's own pings */
        w->ping_ok++;
        if (!w->ping_best_us || g_ping_rtt < w->ping_best_us)
          w->ping_best_us = g_ping_rtt;
      }
    } else {
      t = NET_RX_OTHER; /* late: not the echo waited for */
    }
  }
  if (NET_BIT(t) & g_net_want)
    g_net_hit = 1;
  if (rl)
    wifi_data_send(w, reply, rl);
  dcache_clean();
}

/* One WMI_CONNECT, after the setup Octoblimp's port found this firmware
 * needs: a plain connect gets NO_NETWORK_AVAIL, while the SSID in probe slot
 * 0, the stock scan parameters, a channel table holding only the network's
 * channel and a connect pinned to its BSSID with CONNECT_PROFILE_MATCH_DONE
 * associate. Unpinned, it is the plain connect over every channel. */
static void wifi_join_try(WifiShared *w, int pinned, int secured) {
  uint8_t c[64];
  int bad = 0;

  for (uint32_t i = 0; i < sizeof(c); i++)
    c[i] = 0;
  c[0] = 0; /* entry */
  c[1] = 1; /* SPECIFIC_SSID_FLAG */
  c[2] = (uint8_t)g_want_len;
  for (uint32_t k = 0; k < g_want_len; k++)
    c[3 + k] = g_want[k];
  bad |= wifi_wmi_send(w, WMI_SET_PROBED_SSID_CMDID, c, 35, g_style);

  for (uint32_t i = 0; i < 20; i++)
    c[i] = 0;
  c[10] = 3;    /* short scans per long */
  c[11] = 0x2F; /* DEFAULT_SCAN_CTRL_FLAGS */
  put16(c + 14, 3);
  bad |= wifi_wmi_send(w, WMI_SET_SCAN_PARAMS_CMDID, c, 20, g_style);

  c[0] = 0;
  c[1] = 0;
  c[2] = 2; /* WMI_11G_MODE */
  c[3] = pinned ? 1 : 13;
  if (pinned)
    put16(c + 4, g_tgt_channel);
  else
    for (uint32_t i = 0; i < 13; i++)
      put16(c + 4 + i * 2, chans[i]);
  bad |= wifi_wmi_send(w, WMI_SET_CHANNEL_PARAMS_CMDID, c, 4u + c[3] * 2u,
                       g_style);

  c[0] = 1; /* ALL_BSS_FILTER */
  c[1] = 0;
  put16(c + 2, 0);
  put32(c + 4, 0);
  bad |= wifi_wmi_send(w, WMI_SET_BSS_FILTER_CMDID, c, 8, g_style);

  for (uint32_t i = 0; i < 52; i++)
    c[i] = 0;
  c[0] = 1; /* INFRA_NETWORK */
  c[1] = 1; /* OPEN_AUTH */
  /* The AR6014's enums (the Linux 3DS port): auth NONE 1 ... WPA2_PSK 5,
   * ciphers NONE 1, WEP 2, TKIP 3, AES 4. */
  c[2] = secured ? 5 : 1;
  c[3] = secured ? 4 : 1;
  c[5] = !secured ? 1 : g_tgt_group == 2u ? 3 : 4;
  c[7] = (uint8_t)g_want_len;
  for (uint32_t k = 0; k < g_want_len; k++)
    c[8 + k] = g_want[k];
  if (pinned) {
    put16(c + 40, g_tgt_channel);
    for (uint32_t k = 0; k < 6; k++)
      c[42 + k] = g_tgt_bssid[k];
    put32(c + 48, 0x08); /* CONNECT_PROFILE_MATCH_DONE */
  }
  if (bad || wifi_wmi_send(w, WMI_CONNECT_CMDID, c, 52, g_style) != 0)
    return;
  g_link = 0;
  w->conn_tries++;
  wifi_conn_stage(w, WIFI_CONN_ASSOC);
  wifi_pump(w, 10000, &g_link);
}

static void wifi_net_run(WifiShared *w) {
  static uint8_t f[NET_FRAME_MAX];
  NetState *n = &g_net;
  uint8_t *z = (uint8_t *)n;
  for (uint32_t i = 0; i < sizeof(*n); i++)
    z[i] = 0;
  n->mac[0] = (uint8_t)w->wmi_mac0;
  n->mac[1] = (uint8_t)(w->wmi_mac0 >> 8);
  n->mac[2] = (uint8_t)(w->wmi_mac0 >> 16);
  n->mac[3] = (uint8_t)(w->wmi_mac0 >> 24);
  n->mac[4] = (uint8_t)w->wmi_mac1;
  n->mac[5] = (uint8_t)(w->wmi_mac1 >> 8);
  n->xid = wifi_now() ^ w->wmi_mac0;
  n->ping_id = (uint16_t)(wifi_now() | 1u);
  g_net_on = 1;

  for (uint32_t k = 0; k < 4u && !n->ip && g_link > 0; k++) {
    wifi_conn_stage(w, WIFI_CONN_DHCP);
    n->xid++;
    g_net_want = NET_BIT(NET_RX_OFFER);
    g_net_hit = 0;
    wifi_data_send(w, f, net_dhcp(n, f, 0));
    wifi_pump(w, 4000, &g_net_hit);
    if (!g_net_hit)
      continue;
    g_net_want = NET_BIT(NET_RX_ACK) | NET_BIT(NET_RX_NAK);
    g_net_hit = 0;
    wifi_data_send(w, f, net_dhcp(n, f, 1));
    wifi_pump(w, 4000, &g_net_hit);
  }
  g_net_want = 0;
  if (!n->ip) {
    wifi_conn_stage(w, WIFI_CONN_NODHCP);
    return;
  }
  if (!n->gw)
    n->gw = n->server;
  w->ip = n->ip;
  w->mask = n->mask;
  w->gw = n->gw;
  w->dns = n->dns;
  w->dhcp_server = n->server;
  w->lease = n->lease;
  wifi_conn_stage(w, WIFI_CONN_IP);

  wifi_conn_stage(w, WIFI_CONN_ARP);
  for (uint32_t k = 0; k < 3u && !n->have_gw_mac && g_link > 0; k++) {
    g_net_want = NET_BIT(NET_RX_ARP_REPLY) | NET_BIT(NET_RX_ARP_ASKED);
    g_net_hit = 0;
    wifi_data_send(w, f, net_arp_request(n, f, n->gw));
    wifi_pump(w, 2000, &g_net_hit);
  }
  g_net_want = 0;
  if (!n->have_gw_mac) {
    wifi_conn_stage(w, WIFI_CONN_NOPING);
    return;
  }
  for (uint32_t k = 0; k < 6; k++)
    w->gw_mac[k] = n->gw_mac[k];

  wifi_conn_stage(w, WIFI_CONN_PING);
  for (uint32_t s = 1; s <= 4u && g_link > 0; s++) {
    g_net_want = NET_BIT(NET_RX_PING_REPLY);
    g_net_hit = 0;
    g_ping_seq = s;
    uint32_t len = net_ping(n, f, n->gw, n->gw_mac, (uint16_t)s);
    g_ping_t = wifi_now();
    if (wifi_data_send(w, f, len) != 0)
      continue;
    w->ping_sent++;
    wifi_pump(w, 2000, &g_net_hit);
  }
  g_net_want = 0;
  wifi_conn_stage(w, w->ping_ok ? WIFI_CONN_DONE : WIFI_CONN_NOPING);
}

/* The RSN element the firmware puts in the association request, which message
 * 2 has to repeat byte for byte. The CONNECT event does not report it, so it
 * is built: PSK and CCMP with the network's group cipher, and in turn RSN
 * capabilities 0, 16 PTKSA replay counters (0x000C), or none. */
static uint32_t wifi_rsn_ie(uint8_t *ie, uint32_t variant, uint32_t group) {
  static const uint8_t base[20] = {48,   18,   1,    0,    0x00, 0x0F, 0xAC,
                                   4,    1,    0,    0x00, 0x0F, 0xAC, 4,
                                   1,    0,    0x00, 0x0F, 0xAC, 2};
  for (uint32_t i = 0; i < 20; i++)
    ie[i] = base[i];
  ie[7] = (uint8_t)group;
  if (variant == 2)
    return 20;
  ie[1] = 20;
  ie[20] = variant == 1 ? 0x0C : 0x00;
  ie[21] = 0;
  return 22;
}

static void wifi_wpa_begin(WifiShared *w, uint32_t variant) {
  const WifiReq *q = (const WifiReq *)WIFI_REQ_ADDR;
  uint8_t pmk[32], seed[32], spa[6], ie[24];
  uint32_t now = wifi_now();
  for (uint32_t i = 0; i < 32; i++) {
    pmk[i] = q->pmk[i];
    seed[i] = q->seed[i];
  }
  for (uint32_t i = 0; i < 4; i++)
    seed[i] ^= (uint8_t)(now >> (8 * i));
  seed[4] ^= (uint8_t)variant;
  for (uint32_t i = 0; i < 4; i++)
    spa[i] = (uint8_t)(w->wmi_mac0 >> (8 * i));
  spa[4] = (uint8_t)w->wmi_mac1;
  spa[5] = (uint8_t)(w->wmi_mac1 >> 8);
  uint32_t n = wifi_rsn_ie(ie, variant, g_tgt_group);
  wpa_begin(&g_wpa, pmk, spa, seed, ie, n, g_tgt_group);
  crypto_wipe(pmk, sizeof(pmk));
  crypto_wipe(seed, sizeof(seed));
  g_wpa_on = 1;
  g_wpa_hit = 0;
  w->wpa_variant = variant;
}

static void wifi_wpa_rx(WifiShared *w, const uint8_t *f, uint32_t n) {
  static uint8_t out[WPA_FRAME_MAX];
  uint32_t ol = 0, info = 0;
  int t = WPA_RX_NONE;
  if (g_wpa_on)
    t = wpa_rx(&g_wpa, f, n, out, &ol, &info);
  if (ol)
    wifi_data_send(w, out, ol);
  w->wpa_m1 = g_wpa.m1;
  w->wpa_m3 = g_wpa.m3;
  w->wpa_g1 = g_wpa.g1;
  w->wpa_bad_mic = g_wpa.bad_mic;
  if (t == WPA_RX_M3)
    g_wpa_hit = 1;
  dcache_clean();
}

/* WMI_ADD_CIPHER_KEY in the AR6014's layout: index, cipher, usage (0
 * pairwise, 1 group), length, RSC, the key in 32 bytes, and KEY_OP_INIT_VAL
 * (3). */
static void wifi_add_key(WifiShared *w, uint32_t idx, uint32_t cipher,
                         uint32_t usage, const uint8_t *key, uint32_t klen,
                         const uint8_t *rsc) {
  uint8_t c[45];
  for (uint32_t i = 0; i < sizeof(c); i++)
    c[i] = 0;
  c[0] = (uint8_t)idx;
  c[1] = (uint8_t)cipher;
  c[2] = (uint8_t)usage;
  c[3] = (uint8_t)klen;
  for (uint32_t i = 0; rsc && i < 8; i++)
    c[4 + i] = rsc[i];
  for (uint32_t i = 0; i < klen && i < 32u; i++)
    c[12 + i] = key[i];
  c[44] = 3;
  int r = wifi_wmi_send(w, WMI_ADD_CIPHER_KEY_CMDID, c, sizeof(c), g_style);
  crypto_wipe(c, sizeof(c));
  if (r == 0)
    w->wpa_keys |= usage ? 2u : 1u;
}

/* Whatever keys the handshake has ready. A TKIP group key goes in with its
 * MIC halves swapped, as wpa_supplicant hands it to a driver. */
static void wifi_wpa_keys(WifiShared *w) {
  if (g_wpa.ptk_ready) {
    wifi_add_key(w, 0, 4, 0, g_wpa.ptk + 32, 16, 0);
    g_wpa.ptk_ready = 0;
  }
  if (g_wpa.gtk_ready) {
    uint8_t k[32];
    uint32_t tkip = g_wpa.group == 2u, len = tkip ? 32u : 16u;
    if (g_wpa.gtk_len >= len) {
      for (uint32_t i = 0; i < 16; i++)
        k[i] = g_wpa.gtk[i];
      for (uint32_t i = 0; tkip && i < 8; i++) {
        k[16 + i] = g_wpa.gtk[24 + i];
        k[24 + i] = g_wpa.gtk[16 + i];
      }
      wifi_add_key(w, g_wpa.gtk_idx, tkip ? 3 : 4, 1, k, len, g_wpa.gtk_rsc);
      crypto_wipe(k, sizeof(k));
    }
    g_wpa.gtk_ready = 0;
  }
  dcache_clean();
}

enum { HS_FAIL = 0, HS_OK, HS_IE, HS_BADPASS };

/* After CONNECT on a secured network: up to 8 s for message 3, then the keys.
 * An access point that takes message 2's MIC but not its RSN element drops
 * the station at once (hostapd: reason 2); one that cannot check the MIC, a
 * wrong password, sends message 1 again until it gives up (reason 15). */
static int wifi_handshake(WifiShared *w) {
  static volatile int never;
  wifi_conn_stage(w, WIFI_CONN_KEYS);
  wifi_pump(w, 8000, &g_wpa_hit);
  if (g_wpa_hit && g_link > 0) {
    /* Message 4 has to leave before the keys go in, or it goes out
     * encrypted with a key the access point does not have yet. */
    wifi_pump(w, 100, &never);
    wifi_wpa_keys(w);
    return g_link > 0 ? HS_OK : HS_FAIL;
  }
  uint32_t why = w->conn_reason >> 16;
  if (g_wpa.bad_mic || g_wpa.m1 >= 2u || why == 15u)
    return HS_BADPASS;
  if (g_wpa.m1 == 1u && g_link < 0)
    return HS_IE;
  return HS_FAIL;
}

static void wifi_join(WifiShared *w, int style, int stay) {
  uint8_t c[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  g_style = style;
  if (!g_want_len) {
    wifi_conn_stage(w, WIFI_CONN_NOREQ);
    return;
  }
  if (!g_tgt_ok && !g_fw_asserted)
    wifi_wmi_scan(w, style, 0); /* once more: a beacon can be missed */
  g_tick_us = HTC_TICK_MS * 1000u;
  if (!g_tgt_ok) {
    wifi_conn_stage(w, WIFI_CONN_NOTFOUND);
    return;
  }
  for (uint32_t k = 0; k < 6; k++)
    w->conn_bssid[k] = g_tgt_bssid[k];
  w->conn_channel = g_tgt_channel;
  w->conn_rssi = g_tgt_rssi;
  w->conn_caps = g_tgt_caps;
  w->sec = g_tgt_sec;
  w->wpa_group = g_tgt_group;
  int secured = (g_tgt_caps & 0x10u) != 0;
  if (secured && (g_tgt_sec != WIFI_SEC_WPA2 ||
                  (g_tgt_group != 4u && g_tgt_group != 2u))) {
    wifi_conn_stage(w, WIFI_CONN_SECURED);
    return;
  }
  if (secured && !g_have_pmk) {
    wifi_conn_stage(w, WIFI_CONN_SECURED);
    return;
  }

  /* Pinned twice, then plain; on a secured network each association also
   * has to get through the handshake, and a refused RSN element moves on to
   * its next form without using up a try. */
  int tries = 0, variant = 0, up = 0, hs = HS_FAIL;
  while (tries < 3 && !up && !g_fw_asserted) {
    if (tries || variant) {
      /* Stop whatever the firmware still tries before the next attempt. */
      g_link = 0;
      g_disc = 0;
      wifi_wmi_send(w, WMI_DISCONNECT_CMDID, c, 0, style);
      wifi_pump(w, 1000, &g_disc);
    }
    if (secured)
      wifi_wpa_begin(w, (uint32_t)variant);
    wifi_join_try(w, tries < 2, secured);
    if (g_link == 1) {
      /* NONE_BSS_FILTER. Otherwise every beacon of the network comes up as a
       * BSSINFO, about ten a second, and reading each byte by byte takes
       * most of a beacon interval: replies queue behind them. */
      wifi_wmi_send(w, WMI_SET_BSS_FILTER_CMDID, c, 8, style);
      if (!secured) {
        up = 1;
        break;
      }
      hs = wifi_handshake(w);
      if (hs == HS_OK) {
        up = 1;
        break;
      }
      if (hs == HS_BADPASS)
        break;
      if (hs == HS_IE && variant < 2) {
        variant++;
        continue;
      }
    }
    tries++;
  }
  if (!up) {
    if (hs == HS_BADPASS || (hs == HS_IE && variant == 2)) {
      wifi_conn_stage(w, WIFI_CONN_BADPASS);
    } else {
      wifi_conn_stage(w, WIFI_CONN_FAILED);
    }
    if (g_link == 1) {
      g_disc = 0;
      wifi_wmi_send(w, WMI_DISCONNECT_CMDID, c, 0, style);
      wifi_pump(w, 1000, &g_disc);
    }
    if (g_wpa_on)
      wpa_end(&g_wpa);
    g_wpa_on = 0;
    return;
  }
  wifi_conn_stage(w, WIFI_CONN_LINKED);

  if (w->svc_status[0] == 0u && w->svc_ep[0])
    wifi_net_run(w);
  else
    wifi_conn_stage(w, WIFI_CONN_NODHCP); /* no data service */

  if (stay && g_link == 1 && g_net.ip && g_net.have_gw_mac) {
    g_session = 1;
    w->session = 1;
    dcache_clean();
    return;
  }
  /* Leave, so the access point does not keep a station nobody serves. */
  if (g_link == 1) {
    g_disc = 0;
    wifi_wmi_send(w, WMI_DISCONNECT_CMDID, c, 0, style);
    wifi_pump(w, 1000, &g_disc);
  }
  g_net_on = 0;
  if (g_wpa_on)
    wpa_end(&g_wpa);
  g_wpa_on = 0;
}

/* The boot options of the command in progress. */
static uint32_t g_opts;

#define WMI_DATA_BE_SVC 0x0101u /* then BK, VI, VO */

/* The four WMI data services, which NWM, ath6kl and Octoblimp's port all
 * connect before setup complete, with ath6kl's connection flags: reduce
 * credit dribble, threshold one half. */
static void wifi_htc_connect_data(WifiShared *w, int style) {
  for (uint32_t i = 0; i < 4; i++) {
    uint8_t req[8], msg[64];
    uint32_t svc = WMI_DATA_BE_SVC + i;
    req[0] = (uint8_t)HTC_MSG_CONNECT_SERVICE_ID;
    req[1] = 0;
    req[2] = (uint8_t)svc;
    req[3] = (uint8_t)(svc >> 8);
    req[4] = 0x05; /* HTC_CONNECT_FLAGS_REDUCE_CREDIT_DRIBBLE | ONE_HALF */
    req[5] = 0;
    req[6] = 0;
    req[7] = 0;
    w->svc_status[i] = 0xFF;
    w->svc_ep[i] = 0;
    if (wifi_htc_send(w, HTC_EP0, 0, req, 8, style) == 0) {
      uint32_t len = wifi_htc_recv(w, msg, sizeof(msg), HTC_TICKS(2000));
      if (len >= 8u && msg[0] == HTC_MSG_CONNECT_RESP_ID && msg[1] == 0) {
        w->svc_status[i] = msg[4];
        w->svc_ep[i] = msg[5];
      }
    }
    dcache_clean();
  }
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
  wifi_stage(w, WIFI_HTC_CONNECT);
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
    int sent = wifi_htc_send(w, HTC_EP0, 0, req, 8, style);
    refused = sent != 0;
    if (refused)
      w->htc_err |= 1u << style;
    len = wifi_htc_recv(w, msg, sizeof(msg),
                        (style == HTC_W_BYTE || style == HTC_W_BYTE4)
                            ? HTC_TICKS(2000)
                            : HTC_TICKS(500));
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
  wifi_stage(w, WIFI_HTC_CONNECTED);
  dcache_clean();

  if (!(g_opts & WIFI_OPT_CTRL_ONLY))
    wifi_htc_connect_data(w, mode);

  /* HTC_SETUP_COMPLETE hands the mailbox from the control channel to the
   * endpoints, after which the firmware starts WMI. */
  req[0] = (uint8_t)HTC_MSG_SETUP_COMPLETE_ID;
  req[1] = 0;
  wifi_htc_send(w, HTC_EP0, 0, req, 2, mode);
  wifi_stage(w, WIFI_HTC_SETUP);
  dcache_clean();

  /* WMI_READY with the AR6014's short header: event id, MAC, PHY capability, a
   * reserved byte, firmware version. */
  len = wifi_htc_recv(w, msg, sizeof(msg), HTC_TICKS(2000));
  if (len >= 2u) {
    w->wmi_event = (uint32_t)msg[0] | ((uint32_t)msg[1] << 8);
    wifi_stage(w, WIFI_HTC_WMI);
    if (w->wmi_event == WMI_READY_EVENTID && len >= 14u) {
      w->wmi_mac0 = (uint32_t)msg[2] | ((uint32_t)msg[3] << 8) |
                    ((uint32_t)msg[4] << 16) | ((uint32_t)msg[5] << 24);
      w->wmi_mac1 = (uint32_t)msg[6] | ((uint32_t)msg[7] << 8);
      /* Control messages are not counted against the credits: ath6kl's
       * HTC gives them all to the service endpoints. */
      w->htc_credit = w->htc_credits;
      wifi_wmi_scan(w, mode, (g_opts & WIFI_OPT_PASSIVE) != 0);
      if (g_opts & WIFI_OPT_CONNECT)
        wifi_join(w, mode, (g_opts & WIFI_OPT_STAY) != 0);
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
    wifi_step(w, WIFI_BOOT_NOFW);
    dcache_clean();
    return;
  }
  if ((fw->main_type != WIFI_FW_TYPE1 && fw->main_type != WIFI_FW_TYPE4) ||
      fw->main_len == 0) {
    wifi_step(w, WIFI_BOOT_NOFW);
    dcache_clean();
    return;
  }
  w->fw_type = fw->main_type;
  dcache_clean();

  wifi_bmi_target_info(w);
  if (w->bmi_ver != AR6014_VERSION || w->bmi_type != AR6014_TYPE) {
    wifi_step(w, WIFI_BOOT_BADVER);
    dcache_clean();
    return;
  }

  /* 1. HTC protocol version into host interest, then the clock/sleep setup. */
  wifi_step(w, WIFI_BOOT_HI);
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
  wifi_step(w, WIFI_BOOT_STUB);
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
  wifi_step(w, WIFI_BOOT_MAIN);
  dcache_clean();
  wifi_bmi_fast_download(w, AR6014_PARTA_DST, (const uint8_t *)WIFI_FW_MAIN,
                         fw->main_len);
  if (!(opts & WIFI_OPT_NO_POST_LZ)) {
    /* DATABASE.BIN is an AR6K DataSet list with a BDIFF patch stream for the
     * firmware, its pointers relocated for 0x53FE18 (Octoblimp's analysis).
     * The firmware applies it only once hi_dset_list_head (HI + 0x18) points
     * at it; without that it fails and the chip stops answering right after
     * HTC_READY. stub_data goes back over the start of what the LZ stream
     * wrote: both land at 0x524C00. */
    if (!(opts & WIFI_OPT_NO_STUB2))
      wifi_bmi_write_mem(w, AR6014_PARTD_DST,
                         (const uint8_t *)WIFI_FW_STUBDATA, fw->stubdata_len);
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
  wifi_step(w, WIFI_BOOT_DONE);
  dcache_clean();
  wifi_htc_wait_ready(w);
  dcache_clean();

  if (w->htc_msgid == HTC_MSG_READY_ID)
    wifi_htc_connect(w);
  dcache_clean();
}

static void wifi_boot_run(uint32_t opts) {
  WifiShared *w = (WifiShared *)WIFI_SHARED_ADDR;
  wifi_timer_start();
  w->boot_opts = opts;
  g_opts = opts;
  g_fw_asserted = 0;
  wifi_wmi_reset(w);
  wifi_join_reset(opts);
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
  w->htc_regs2 = 0;
  w->htc_look2 = 0;
  w->htc_drained = 0;
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


/* Until a command writes it, WifiShared holds whatever FCRAM held. */
void wifi11_init(void) {
  volatile uint8_t *p = (volatile uint8_t *)WIFI_SHARED_ADDR;
  for (uint32_t i = 0; i < sizeof(WifiShared); i++)
    p[i] = 0;
  dcache_clean();
}

/* The MAC a frame to `ip` goes to: the router's for anything off our subnet,
 * else the host's own, asked for with ARP. 0 if there is none. */
static int wifi_mac_for(WifiShared *w, uint32_t ip, uint8_t *mac,
                        uint32_t ms) {
  static uint8_t f[NET_FRAME_MAX];
  NetState *n = &g_net;
  const uint8_t *src = 0;
  if (ip == n->gw || !n->mask || ((ip ^ n->ip) & n->mask)) {
    if (n->have_gw_mac)
      src = n->gw_mac;
  } else {
    if (n->arp_ip != ip) {
      n->arp_ip = ip;
      n->have_arp = 0;
    }
    for (uint32_t k = 0; k < 3u && !n->have_arp && g_link > 0; k++) {
      g_net_want = NET_BIT(NET_RX_ARP_REPLY);
      g_net_hit = 0;
      wifi_data_send(w, f, net_arp_request(n, f, ip));
      wifi_pump(w, ms < 1000u ? ms : 1000u, &g_net_hit);
    }
    g_net_want = 0;
    if (n->have_arp)
      src = n->arp_mac;
  }
  if (!src)
    return 0;
  for (uint32_t k = 0; k < 6; k++)
    mac[k] = src[k];
  return 1;
}

static void wifi_op_dns(WifiShared *w, WifiNetIo *io, uint32_t ms) {
  static char name[256];
  static uint8_t f[NET_FRAME_MAX];
  NetState *n = &g_net;
  uint8_t mac[6];
  uint32_t i = 0, server = n->dns ? n->dns : n->gw;
  for (; i < sizeof(name) - 1u && io->name[i]; i++)
    name[i] = io->name[i];
  name[i] = '\0';
  if (!wifi_mac_for(w, server, mac, ms)) {
    io->status = WIFI_NETS_NOHOST;
    return;
  }
  io->status = WIFI_NETS_TIMEOUT;
  for (uint32_t k = 0; k < 3u && g_link > 0; k++) {
    uint32_t r = wifi_now();
    n->dns_id = (uint16_t)(r ^ (r >> 16));
    n->dns_port = (uint16_t)(0xC000u | (r & 0x3FFFu));
    uint32_t len = net_dns(n, f, server, mac, name);
    if (!len) {
      io->status = WIFI_NETS_BADNAME;
      break;
    }
    g_net_want = NET_BIT(NET_RX_DNS);
    g_net_hit = 0;
    g_dns_ip = 0;
    if (wifi_data_send(w, f, len) != 0) {
      io->status = WIFI_NETS_NOSEND;
      continue;
    }
    wifi_pump(w, ms, &g_net_hit);
    if (g_net_hit) {
      io->dns_rcode = n->dns_rcode;
      io->ip = g_dns_ip;
      io->status = g_dns_ip               ? WIFI_NETS_OK
                   : n->dns_rcode == 3u ? WIFI_NETS_NONAME
                                         : WIFI_NETS_DNSFAIL;
      break;
    }
  }
  g_net_want = 0;
  n->dns_port = 0;
}

static void wifi_op_ping(WifiShared *w, WifiNetIo *io, uint32_t ms) {
  static uint8_t f[NET_FRAME_MAX];
  NetState *n = &g_net;
  uint8_t mac[6];
  uint32_t dst = io->ip;
  if (dst == n->ip) { /* ourselves: as the loopback would answer */
    io->status = WIFI_NETS_OK;
    io->from = dst;
    io->ttl = 64;
    io->bytes = 8u + NET_PING_DATA;
    return;
  }
  if (!wifi_mac_for(w, dst, mac, ms)) {
    io->status = WIFI_NETS_NOHOST;
    return;
  }
  g_ping_seq = io->seq & 0xFFFFu;
  g_ping_got = 0;
  g_net_want = NET_BIT(NET_RX_PING_REPLY) | NET_BIT(NET_RX_PING_ERR);
  g_net_hit = 0;
  uint32_t len = net_ping(n, f, dst, mac, (uint16_t)g_ping_seq);
  g_ping_t = wifi_now();
  if (wifi_data_send(w, f, len) != 0) {
    io->status = WIFI_NETS_NOSEND;
    g_net_want = 0;
    return;
  }
  wifi_pump(w, ms, &g_net_hit);
  g_net_want = 0;
  if (!g_net_hit) {
    io->status = g_link > 0 ? WIFI_NETS_TIMEOUT : WIFI_NETS_NOLINK;
    return;
  }
  io->rtt_us = g_ping_rtt;
  io->ttl = n->reply_ttl;
  io->bytes = n->reply_len;
  io->from = n->reply_from;
  if (g_ping_got == NET_RX_PING_ERR) {
    io->status = WIFI_NETS_ICMPERR;
    io->icmp = g_ping_err;
  } else {
    io->status = WIFI_NETS_OK;
  }
}

/* Whether the connection is over: the reply in (or the server closed), the
 * server reset it, or the reply filled the buffer. Sets io->status. */
static int http_over(NetState *n, WifiNetIo *io) {
  if (n->tcp_state == NET_TCP_RESET) {
    io->status = WIFI_NETS_REFUSED;
    return 1;
  }
  if (n->tcp_state != NET_TCP_OPEN)
    return 0;
  if (n->tcp_fin || net_http_done(n->tcp_rx, n->tcp_rx_len)) {
    io->status = WIFI_NETS_OK;
    io->stage = WIFI_HTTP_DONE;
    return 1;
  }
  if (n->tcp_rx_len >= n->tcp_rx_cap) {
    io->status = WIFI_NETS_TOOBIG;
    return 1;
  }
  return 0;
}

/* The request goes out in segments as the server's window allows. Unanswered
 * for the retransmission timeout (1 s, doubling to 4 s), everything from the
 * first unacknowledged byte goes again. Once the reply is in, a FIN ends the
 * connection, and the server's own FIN is waited for briefly. */
static void wifi_op_http(WifiShared *w, WifiNetIo *io, uint32_t ms) {
  static uint8_t f[NET_TX_MAX];
  NetState *n = &g_net;
  const uint8_t *req = (const uint8_t *)WIFI_HTTP_REQ;
  uint32_t len = io->req_len, t0 = wifi_now(), rto = 1000000u, base, last, una;
  uint8_t mac[6];
  io->resp_len = 0;
  io->stage = 0;
  io->resent = 0;
  if (!len || len > WIFI_HTTP_REQ_MAX || !io->port || io->port > 0xFFFFu) {
    io->status = WIFI_NETS_TOOBIG;
    return;
  }
  if (!wifi_mac_for(w, io->ip, mac, ms)) {
    io->status = WIFI_NETS_NOHOST;
    return;
  }
  uint32_t r = wifi_now() ^ w->wmi_mac0;
  net_tcp_open(n, io->ip, (uint16_t)(0xC000u | ((r >> 3) & 0x3FFFu)),
               (uint16_t)io->port, r * 2654435761u, (uint8_t *)WIFI_HTTP_RESP,
               WIFI_HTTP_RESP_MAX);
  base = n->tcp_iss + 1u;
  una = n->snd_una;
  g_net_want = NET_BIT(NET_RX_TCP);
  io->stage = WIFI_HTTP_CONNECT;
  io->status = WIFI_NETS_TIMEOUT;
  wifi_data_send(w, f, net_tcp_seg(n, f, mac, NET_TCP_SYN, n->tcp_iss, 0, 0));
  last = wifi_now();

  while (g_link > 0 && !g_fw_asserted && wifi_now() - t0 < ms * 1000u &&
         !http_over(n, io)) {
    if (n->snd_una != una) {
      una = n->snd_una;
      rto = 1000000u;
      last = wifi_now();
    }
    if (n->snd_una != n->snd_nxt && wifi_now() - last >= rto) {
      io->resent++;
      rto = rto < 4000000u ? rto * 2u : rto;
      last = wifi_now();
      if (n->tcp_state == NET_TCP_SYN_SENT)
        wifi_data_send(w, f,
                       net_tcp_seg(n, f, mac, NET_TCP_SYN, n->tcp_iss, 0, 0));
      else
        n->snd_nxt = n->snd_una;
    }
    while (n->tcp_state == NET_TCP_OPEN && n->snd_nxt - base < len) {
      uint32_t off = n->snd_nxt - base, flight = n->snd_nxt - n->snd_una;
      uint32_t seg = len - off;
      if (flight >= n->tcp_wnd)
        break;
      if (seg > n->tcp_mss)
        seg = n->tcp_mss;
      if (seg > NET_TCP_SEG)
        seg = NET_TCP_SEG;
      if (seg > n->tcp_wnd - flight)
        seg = n->tcp_wnd - flight;
      uint32_t fl = NET_TCP_ACK | (off + seg == len ? NET_TCP_PSH : 0u);
      if (wifi_data_send(w, f, net_tcp_seg(n, f, mac, fl, n->snd_nxt,
                                           req + off, seg)) != 0)
        break;
      n->snd_nxt += seg;
      last = wifi_now();
    }
    if (n->tcp_state == NET_TCP_OPEN)
      io->stage = n->snd_una - base >= len ? WIFI_HTTP_WAIT : WIFI_HTTP_SEND;
    g_net_hit = 0;
    wifi_pump(w, 100, &g_net_hit);
  }

  if (n->tcp_state == NET_TCP_OPEN && io->status == WIFI_NETS_OK) {
    wifi_data_send(w, f, net_tcp_seg(n, f, mac, NET_TCP_FIN | NET_TCP_ACK,
                                     n->snd_nxt, 0, 0));
    n->snd_nxt++;
    uint32_t t1 = wifi_now();
    while (g_link > 0 && !(n->tcp_fin && n->snd_una == n->snd_nxt) &&
           wifi_now() - t1 < 500000u) {
      g_net_hit = 0;
      wifi_pump(w, 50, &g_net_hit);
    }
  } else if (n->tcp_state == NET_TCP_OPEN) {
    wifi_data_send(w, f, net_tcp_seg(n, f, mac, NET_TCP_RST | NET_TCP_ACK,
                                     n->snd_nxt, 0, 0));
  }
  g_net_want = 0;
  n->tcp_state = NET_TCP_CLOSED;
  io->resp_len = n->tcp_rx_len;
  if (g_link <= 0 && io->status == WIFI_NETS_TIMEOUT)
    io->status = WIFI_NETS_NOLINK;
}

/* The faster bus for a network operation (see bus_divs): the highest clock
 * left that reads bus_sig as the slow one does. Only on a 1-bit bus, the one a
 * failed CMD53 resets the controller to. */
static void wifi_bus_up(WifiShared *w) {
  uint32_t ref, errs;
  g_net_bus = g_style == HTC_W_BYTE || g_style == HTC_W_BLOCK;
  if (!g_net_bus || g_bus_step >= BUS_DIVS ||
      bus_div() == bus_divs[g_bus_step])
    return;
  bus_clock(BUS_SLOW);
  errs = g_bus_errs;
  ref = bus_sig(w);
  if (g_bus_errs != errs)
    return;
  for (; g_bus_step < BUS_DIVS; g_bus_step++) {
    bus_clock(bus_divs[g_bus_step]);
    if (bus_sig(w) == ref && g_bus_errs == errs)
      return;
    bus_clock(BUS_SLOW);
    errs = g_bus_errs;
  }
}

/* While a network operation waits for frames, it polls this often. */
#define NET_TICK_US 1000u

/* One AUDIO_CMD_WIFI_NET operation on the session a WIFI_OPT_STAY join left.
 * Nothing reads the chip between commands, so what queued up meanwhile is
 * taken first: a DISCONNECT there ends the session. */
static void wifi_net_op(uint32_t op) {
  WifiShared *w = (WifiShared *)WIFI_SHARED_ADDR;
  WifiNetIo *io = (WifiNetIo *)WIFI_NET_ADDR;
  wifi_timer_start();
  uint32_t ms = io->timeout_ms, cap = op == WIFI_NETOP_HTTP ? 20000u : 10000u;
  ms = ms < 100u ? 2000u : ms > cap ? cap : ms;
  io->status = WIFI_NETS_NOLINK;
  io->rtt_us = 0;
  io->ttl = 0;
  io->bytes = 0;
  io->from = 0;
  io->icmp = 0;
  io->dns_rcode = 0;
  if (g_session && !g_fw_asserted) {
    g_tick_us = NET_TICK_US;
    wifi_bus_up(w);
    for (int i = 0; i < 64 && g_link > 0; i++) {
      wifi_wmi_events(w, 0, 1, 1);
      if (!g_htc_got)
        break;
    }
    if (g_wpa_on && g_link > 0)
      wifi_wpa_keys(w); /* a group rekey that came meanwhile */
    if (g_link <= 0 || g_fw_asserted) {
      g_session = 0;
      g_net_on = 0;
    } else if (op == WIFI_NETOP_STATUS) {
      io->status = WIFI_NETS_OK;
    } else if (op == WIFI_NETOP_DNS) {
      wifi_op_dns(w, io, ms);
    } else if (op == WIFI_NETOP_PING) {
      wifi_op_ping(w, io, ms);
    } else if (op == WIFI_NETOP_HTTP) {
      wifi_op_http(w, io, ms);
    } else if (op == WIFI_NETOP_LEAVE) {
      uint8_t c[2] = {0, 0};
      g_disc = 0;
      wifi_wmi_send(w, WMI_DISCONNECT_CMDID, c, 0, g_style);
      wifi_pump(w, 1000, &g_disc);
      g_session = 0;
      g_net_on = 0;
      io->status = WIFI_NETS_OK;
    }
    if (g_wpa_on && g_link > 0)
      wifi_wpa_keys(w);
    if (g_link <= 0) {
      g_session = 0;
      g_net_on = 0;
    }
    if (!g_session && g_wpa_on) {
      wpa_end(&g_wpa);
      g_wpa_on = 0;
    }
    g_net_bus = 0;
  }
  w->session = (uint32_t)g_session;
  w->bus_div = bus_div();
  w->bus_rx53 = !g_rx53_bad;
  w->bus_errs = g_bus_errs;
  dcache_clean();
  wifi_finish(w);
}

void wifi11_probe(void) { wifi_probe_run(); }
void wifi11_boot(uint32_t opts) { wifi_boot_run(opts); }
void wifi11_net(uint32_t op) { wifi_net_op(op); }
