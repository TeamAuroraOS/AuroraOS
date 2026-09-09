/*
 * AuroraOS ARM11 audio core.
 *
 * Runs on the ARM11 (woken by the firm stub via the mailbox). Brings up the
 * full analog output path and plays a test tone through CSND on request from
 * the ARM9 OS via the shared command block at AUDIO_CTRL_ADDR.
 *
 * The output bring-up (SPI + PDN + CFG11 + I2S controller + CTR audio codec)
 * is ported from profi200's libn3ds (source/arm11/drivers/{codec,spi,i2s}.c),
 * which is the authoritative bare-metal reference. The CSND channel programming
 * follows GBATEK "3DS Sound and Microphone".
 *
 * Untested-on-hardware caveats (flagged inline): the codec calibration values
 * (driver gains / analog volumes) normally come from the console's HWCAL block,
 * which isn't read here, neutral defaults are used instead. Headphone-jack
 * detection is skipped (output forced to the speaker path). EQ-filter upload is
 * skipped (codec defaults). Per-unit "depop" GPIO pulsing is skipped.
 */
#include "audio.h"
#include "touch.h"
#include "wifi.h"

/* PICA200 GPU operations live in gpu11.c (GPL-2.0), linked into this core. */
void gpu11_run(void);

typedef volatile uint8_t  vu8;
typedef volatile uint16_t vu16;
typedef volatile uint32_t vu32;

#define MMIO8(a)  (*(vu8 *)(a))
#define MMIO16(a) (*(vu16 *)(a))
#define MMIO32(a) (*(vu32 *)(a))

/* Cache maintenance */
static inline void dsb(void) {
  __asm__ volatile("mcr p15, 0, %0, c7, c10, 4" ::"r"(0) : "memory");
}
static inline void dcache_clean(void) {
  __asm__ volatile("mcr p15, 0, %0, c7, c10, 0" ::"r"(0) : "memory");
  dsb();
}
static inline void dcache_clean_inval(void) {
  __asm__ volatile("mcr p15, 0, %0, c7, c14, 0" ::"r"(0) : "memory");
  dsb();
}

static void spin(uint32_t n) {
  while (n--)
    __asm__ volatile("nop");
}
/* Coarse millisecond sleep (over-sleeps slightly; only used for codec settle
 * delays, so erring long is fine). ~300k nops/ms is comfortably >= 1 ms at the
 * ARM11's clock. */
static void sleep_ms(uint32_t ms) { spin(ms * 300000u); }

/* SoC I/O: SPI, PDN, CFG11 */
/* IO_COMMON_BASE = 0x10100000 on the ARM11 (libn3ds mem_map). */
#define IO_BASE 0x10100000u

#define CFG11_SPI_CNT (IO_BASE + 0x401C0u) /* u16: bits0-2 enable new SPI IF */
#define PDN_I2S_CNT   (IO_BASE + 0x41220u) /* u8:  bit1 = I2S clock 2 enable  */

/* SoC I2S controller (== GBATEK "SNDEXCNT"): two u16 halves at 0x10145000/2. */
#define I2S1_CNT (IO_BASE + 0x45000u) /* u16 */
#define I2S2_CNT (IO_BASE + 0x45002u) /* u16 */
#define I2S1_EN          (1u << 15)
#define I2S1_MCLK1_16MHZ (1u << 14)
#define I2S1_FREQ_32KHZ  (0u)
#define I2S1_LGY_VOL(n)  (((n) & 0x3Fu) << 6)
#define I2S1_DSP_VOL(n)  ((n) & 0x3Fu)
#define I2S2_EN          (1u << 15)
#define I2S2_MCLK2_16MHZ (1u << 14)
#define I2S2_FREQ_47KHZ  (1u << 13)

/* NSPI bus 2 (0x10142800), the CTR audio codec lives here (CS0, 16 MHz). */
#define NSPI2 (IO_BASE + 0x42800u)
#define NSPI_CNT       (NSPI2 + 0x00) /* u32 */
#define NSPI_CS        (NSPI2 + 0x04) /* u32 */
#define NSPI_BLKLEN    (NSPI2 + 0x08) /* u32: transfer length in bytes */
#define NSPI_FIFO      (NSPI2 + 0x0C) /* u32 */
#define NSPI_FIFO_STAT (NSPI2 + 0x10) /* u8: bit0 = FIFO busy */
#define NSPI_INT_MASK  (NSPI2 + 0x18) /* u32 */
#define NSPI_INT_STAT  (NSPI2 + 0x1C) /* u32 */

#define NSPI_EN     (1u << 15)
#define NSPI_DIR_S  (1u << 13)
#define NSPI_DIR_R  (0u)
#define NSPI_FIFO_BUSY 1u
#define CODEC_CSCLK 0x05u /* NSPI_CS_0 | NSPI_CLK_16MHZ */
#define DEV_CS_HIGH 0x80u /* keep CS as-is (HW auto-manages), don't force-raise */

#define SPI_GUARD 200000u /* bound the busy-waits so a mis-config can't hang */

static uint32_t g_spi_timeouts = 0; /* diagnostics: SPI waits that maxed out */

static void nspi_wait_fifo(void) {
  uint32_t g = 0;
  while ((MMIO8(NSPI_FIFO_STAT) & NSPI_FIFO_BUSY) && ++g < SPI_GUARD)
    ;
  if (g >= SPI_GUARD)
    g_spi_timeouts++;
}
static void nspi_wait_done(void) {
  uint32_t g = 0;
  while ((MMIO32(NSPI_CNT) & NSPI_EN) && ++g < SPI_GUARD)
    ;
  if (g >= SPI_GUARD)
    g_spi_timeouts++;
}

/* Faithful port of NSPI_sendRecv for the codec device (always bus 2). `dev`
 * carries the DEV_CS_HIGH flag exactly as the codec calls use it. */
static void nspi_sendrecv(uint32_t dev, const uint8_t *in, uint8_t *out,
                          uint32_t inSize, uint32_t outSize) {
  const uint32_t cntParams = NSPI_EN | CODEC_CSCLK;
  if (in) {
    const uint32_t *p = (const uint32_t *)in;
    uint32_t c = 0;
    MMIO32(NSPI_BLKLEN) = inSize;
    MMIO32(NSPI_CNT) = cntParams | NSPI_DIR_S;
    do {
      if ((c & 31u) == 0)
        nspi_wait_fifo();
      MMIO32(NSPI_FIFO) = *p++;
      c += 4;
    } while (c < inSize);
    nspi_wait_done();
  }
  if (out) {
    uint32_t *p = (uint32_t *)out;
    uint32_t c = 0;
    MMIO32(NSPI_BLKLEN) = outSize;
    MMIO32(NSPI_CNT) = cntParams | NSPI_DIR_R;
    do {
      if ((c & 31u) == 0)
        nspi_wait_fifo();
      *p++ = MMIO32(NSPI_FIFO);
      c += 4;
    } while (c < outSize);
    nspi_wait_done();
  }
  /* NSPI_DEV_CS_HIGH set => raise CS (deassert) at the end of the transaction. */
  if (dev & DEV_CS_HIGH)
    MMIO8(NSPI_CS) = 0; /* NSPI_CS_HIGH = 0 */
}

/* ---- CTR audio codec register access (page<<8 | offset) ---- */
#define CODEC_DEV (DEV_CS_HIGH | 0x03u) /* NSPI_DEV_CS_HIGH | NSPI_DEV_CTR_CODEC */

static uint8_t cdc_page = 0xFF;

static void cdc_switch_page(uint16_t reg) {
  uint8_t page = (uint8_t)(reg >> 8);
  if (cdc_page != page) {
    cdc_page = page;
    uint8_t buf[4] __attribute__((aligned(4)));
    buf[0] = 0; /* CDC_REG_PAGE_CTRL */
    buf[1] = page;
    nspi_sendrecv(CODEC_DEV, buf, 0, 2, 0);
  }
}
static void cdc_write(uint16_t reg, uint8_t val) {
  cdc_switch_page(reg);
  uint8_t buf[4] __attribute__((aligned(4)));
  buf[0] = (uint8_t)((reg << 1) & 0xFF); /* write: bit0 = 0 */
  buf[1] = val;
  nspi_sendrecv(CODEC_DEV, buf, 0, 2, 0);
}
static uint8_t cdc_read(uint16_t reg) {
  cdc_switch_page(reg);
  uint8_t in[4] __attribute__((aligned(4)));
  uint8_t out[4] __attribute__((aligned(4)));
  in[0] = (uint8_t)(((reg << 1) & 0xFF) | 1u); /* read: bit0 = 1 */
  nspi_sendrecv(CODEC_DEV, in, out, 1, 1);
  return out[0];
}
static void cdc_mask(uint16_t reg, uint8_t val, uint8_t mask) {
  uint8_t d = cdc_read(reg);
  d = (uint8_t)((d & ~mask) | (val & mask));
  cdc_write(reg, d);
}

/* Diagnostic: read `reg` receiving 4 bytes and return the whole FIFO word, so a
 * byte-lane mismatch (data not in bits 0-7) is visible. */
static uint32_t cdc_read_word(uint16_t reg) {
  cdc_switch_page(reg);
  uint8_t in[4] __attribute__((aligned(4)));
  uint32_t out = 0;
  in[0] = (uint8_t)(((reg << 1) & 0xFF) | 1u);
  nspi_sendrecv(CODEC_DEV, in, (uint8_t *)&out, 1, 4);
  return out;
}

/* Burst-read `size` bytes starting at `reg` (the codec auto-increments the
 * register). `buf` must be 4-byte aligned. */
static void cdc_read_buf(uint16_t reg, uint8_t *buf, uint32_t size) {
  cdc_switch_page(reg);
  uint8_t in[4] __attribute__((aligned(4)));
  in[0] = (uint8_t)(((reg << 1) & 0xFF) | 1u);
  nspi_sendrecv(CODEC_DEV, in, buf, 1, size);
}

/* ---- codec registers (page<<8 | offset), from libn3ds codec_regmap.h ---- */
#define CDC_SOFT_RST ((100u << 8) | 1u)
#define CDC_0_2      ((0u << 8) | 2u)
#define CDC_0_3      ((0u << 8) | 3u)
#define CDC_GPI_PIN  ((0u << 8) | 57u)  /* GPI1_GPI2_PIN_CTRL */
#define CDC_DAC_PATH ((0u << 8) | 63u)  /* DAC_DATA_PATH_SETUP */
#define CDC_DAC_VOL  ((0u << 8) | 64u)  /* DAC_VOLUME_CTRL */
#define CDC_DAC_NDAC ((0u << 8) | 11u)  /* DAC_NDAC_VAL */
#define CDC_HEADSET  ((100u << 8) | 69u)
#define CDC_100_34   ((100u << 8) | 34u)
#define CDC_100_37   ((100u << 8) | 37u)
#define CDC_100_67   ((100u << 8) | 67u)
#define CDC_100_118  ((100u << 8) | 118u)
#define CDC_100_119  ((100u << 8) | 119u)
#define CDC_100_120  ((100u << 8) | 120u)
#define CDC_100_122  ((100u << 8) | 122u)
#define CDC_100_124  ((100u << 8) | 124u)
#define CDC_101_10   ((101u << 8) | 10u)
#define CDC_101_11   ((101u << 8) | 11u)
#define CDC_101_12   ((101u << 8) | 12u)
#define CDC_101_17   ((101u << 8) | 17u)
#define CDC_101_18   ((101u << 8) | 18u)
#define CDC_101_19   ((101u << 8) | 19u)
#define CDC_101_22   ((101u << 8) | 22u)
#define CDC_101_23   ((101u << 8) | 23u)
#define CDC_101_27   ((101u << 8) | 27u)
#define CDC_101_28   ((101u << 8) | 28u)
#define CDC_101_119  ((101u << 8) | 119u)
#define CDC_101_122  ((101u << 8) | 122u)

/* Neutral defaults in place of per-console HWCAL calibration. */
#define CAL_DRIVER_GAIN_HP 1u
#define CAL_DRIVER_GAIN_SP 1u
#define CAL_ANALOG_VOL_HP  0u
#define CAL_ANALOG_VOL_SP  0u

static void power_on_dac(void) {
  cdc_mask(CDC_100_118, 0xC0, 0xC0);
  sleep_ms(10);
  for (int i = 0; i < 100; i++) {
    if ((uint8_t)(~cdc_read(CDC_100_37) & 0x88u) == 0)
      break;
    sleep_ms(1);
  }
}

/* Full analog-output bring-up. Sets status via the caller. */
static void codec_init(void) {
  /* Clock/interface domain: enable new SPI IF, codec MCLK, codec SPI bus. */
  MMIO16(CFG11_SPI_CNT) = 0x7u;
  MMIO8(PDN_I2S_CNT) = 0x02u; /* PDN_I2S_CNT_I2S_CLK2_EN */
  dsb();
  MMIO32(NSPI_INT_MASK) = 0x1u;
  MMIO32(NSPI_INT_STAT) = 0x7u;
  dsb();

  /* CTR codec init (subset of libn3ds CODEC_init/soundInit for output). */
  cdc_page = 0xFF;
  cdc_write(CDC_SOFT_RST, 1);
  sleep_ms(40);
  cdc_switch_page(0);

  cdc_write(CDC_100_67, 0x11);
  cdc_mask(CDC_101_119, 1, 1);
  cdc_mask(CDC_GPI_PIN, 0x66, 0x66);
  cdc_write(CDC_101_122, 1);        /* VREF */
  cdc_mask(CDC_100_34, 0x18, 0x18); /* PLL  */

  /* Headset: force the speaker path (skip headphone-jack GPIO detection). */
  cdc_mask(CDC_HEADSET, 0x20, 0x30); /* HP_EN set, HP-select clear */
  cdc_mask(CDC_100_67, 0x00, 0x80);
  cdc_mask(CDC_100_67, 0x80, 0x80);

  /* Codec-side I2S dividers: line 1 = 32 kHz, line 2 = 47 kHz. */
  cdc_write(CDC_DAC_NDAC, 0x87);
  cdc_mask(CDC_100_124, 0, 1);

  /* SoC I2S controller: enable both lines with their MCLK + sample clocks. */
  MMIO16(I2S1_CNT) = 0;
  MMIO16(I2S2_CNT) = 0;
  MMIO16(I2S1_CNT) = I2S1_EN | I2S1_MCLK1_16MHZ | I2S1_FREQ_32KHZ |
                     I2S1_LGY_VOL(32) | I2S1_DSP_VOL(0);
  MMIO16(I2S2_CNT) = I2S2_EN | I2S2_MCLK2_16MHZ | I2S2_FREQ_47KHZ;
  dsb();

  /* Output stage: power the DAC, unmute both lines, power the drivers. */
  cdc_mask(CDC_101_17, 0x10, 0x1C);
  cdc_write(CDC_100_122, 0);
  cdc_write(CDC_100_120, 0);
  power_on_dac();
  cdc_write(CDC_101_10, 0xA);

  cdc_mask(CDC_DAC_PATH, 0xC0, 0xC0); /* unmute DAC line 1 */
  cdc_write(CDC_DAC_VOL, 0);
  cdc_mask(CDC_100_119, 0, 0xC); /* unmute DAC line 2 */

  {
    uint8_t r2 = cdc_read(CDC_0_2), r3 = cdc_read(CDC_0_3);
    uint8_t v = ((r2 & 0xFu) <= 1u && (((r3 & 0x70u) >> 4) <= 2u)) ? 0x3C : 0x1C;
    cdc_write(CDC_101_11, v);
  }
  cdc_write(CDC_101_12, (CAL_DRIVER_GAIN_HP << 3) | 4); /* headphone driver */
  cdc_write(CDC_101_22, CAL_ANALOG_VOL_HP);
  cdc_write(CDC_101_23, CAL_ANALOG_VOL_HP);

  cdc_mask(CDC_101_17, 0xC0, 0xC0);                     /* speaker driver */
  cdc_write(CDC_101_18, (CAL_DRIVER_GAIN_SP << 2) | 2);
  cdc_write(CDC_101_19, (CAL_DRIVER_GAIN_SP << 2) | 2);
  cdc_write(CDC_101_27, CAL_ANALOG_VOL_SP);
  cdc_write(CDC_101_28, CAL_ANALOG_VOL_SP);
  sleep_ms(38);
}

/* Touchscreen (via codec) */
/* The touchscreen + circle-pad ADC live inside the same CTR codec. The register
 * init sequence (page 0x67) and the raw-data layout (page 0xFB, byte offsets)
 * below are REIMPLEMENTED from the hardware facts in GodMode9's codec driver:
 *   arm11/source/hw/codec.c
 *   Copyright (C) 2017 Sergi Granell, Paul LaMendola
 *   Copyright (C) 2019 Wolfvak, licensed GPL v2-or-later.
 * Only the register addresses / init sequence / data layout are used (hardware
 * facts); the code here is Aurora's own, built on its existing SPI helpers. */

#define CDC(page, off) (((uint16_t)(page) << 8) | (uint16_t)(off))

static void touch_init(void) {
  cdc_write(CDC(0x67, 0x24), 0x98);
  cdc_write(CDC(0x67, 0x26), 0x00);
  cdc_write(CDC(0x67, 0x25), 0x43);
  cdc_write(CDC(0x67, 0x24), 0x18);
  cdc_write(CDC(0x67, 0x17), 0x43);
  cdc_write(CDC(0x67, 0x19), 0x69);
  cdc_write(CDC(0x67, 0x1B), 0x80);
  cdc_write(CDC(0x67, 0x27), 0x11);
  cdc_write(CDC(0x67, 0x26), 0xEC);
  cdc_write(CDC(0x67, 0x24), 0x18);
  cdc_write(CDC(0x67, 0x25), 0x53);
  cdc_mask(CDC(0x67, 0x26), 0x80, 0x80);
  cdc_mask(CDC(0x67, 0x24), 0x00, 0x80);
  cdc_mask(CDC(0x67, 0x25), 0x10, 0x3C);
}

/* Read one raw sample block from the codec and publish touch state. */
static uint32_t g_touch_seq = 0;
static void touch_poll(void) {
  uint8_t buf[52] __attribute__((aligned(4)));
  cdc_read_buf(CDC(0xFB, 1), buf, 52);

  TouchShared *ts = (TouchShared *)TOUCH_SHARED_ADDR;
  ts->d_b0 = buf[0]; /* diagnostics: raw sample bytes, always */
  ts->d_b1 = buf[1];
  ts->d_b10 = buf[10];
  ts->d_b11 = buf[11];
  int pressed = !(buf[0] & 0x10); /* byte0 bit4 low => pen down */
  if (pressed) {
    ts->raw_x = (uint32_t)(((buf[0] << 8) | buf[1]) & 0xFFF);
    ts->raw_y = (uint32_t)(((buf[10] << 8) | buf[11]) & 0xFFF);
    ts->pressed = 1;
  } else {
    ts->pressed = 0;
  }
  ts->seq = ++g_touch_seq;
  dcache_clean(); /* publish to the ARM9 */
}

/* CSND audio (GBATEK) */
/* CSND master control (0x10103000, u32): bits0-15 vol, bit16 mute (0=on),
 * bit30 = normal, bit31 = allow channel writes (required before ch start). */
#define CSND_MAIN     0x10103000u
#define CSND_MAIN_VAL (0x8000u | (1u << 30) | (1u << 31))

/* Channel blocks: 0x10103400 + n*0x20. */
#define CSND_CH(n)      (0x10103400u + (uint32_t)(n) * 0x20u)
#define CSND_CH_CNT(n)  (CSND_CH(n) + 0x00) /* u32: rate<<16 | start | fmt | ...*/
#define CSND_CH_VOL(n)  (CSND_CH(n) + 0x04) /* u32: volR | volL<<16              */
#define CSND_CH_SAD(n)  (CSND_CH(n) + 0x0C) /* u32: sample source phys addr      */
#define CSND_CH_SIZE(n) (CSND_CH(n) + 0x10) /* u32: total size in BYTES          */
#define CSND_CH_LOOP(n) (CSND_CH(n) + 0x14) /* u32: loop restart phys addr       */

#define CH_REPEAT_LOOP    (1u << 10) /* bits10-11: 1 = loop infinite */
#define CH_REPEAT_ONESHOT (2u << 10) /* bits10-11: 2 = one-shot      */
#define CH_FORMAT_PCM16   (1u << 12)
#define CH_NORMAL         (1u << 14)
#define CH_START          (1u << 15)
#define CSND_RATE(rate) ((0x3FEC3FCu / (uint32_t)(rate)) & 0xFFFFu)

#define TONE_RATE 32000u

static void csnd_init(void) {
  MMIO32(CSND_MAIN) = CSND_MAIN_VAL;
  dsb();
}
static void csnd_stop(void) {
  MMIO32(CSND_CH_CNT(0)) = 0;
  dsb();
}
/* Play `bytes` of PCM at `phys`. fmt = CH_FORMAT_PCM16 (16-bit) or 0 (8-bit);
 * loop != 0 loops forever, else one-shot. */
static void csnd_play(uint32_t phys, uint32_t bytes, uint32_t rate, uint32_t fmt,
                      int loop) {
  csnd_stop();
  MMIO32(CSND_CH_VOL(0)) = 0x80008000u; /* full L + R */
  MMIO32(CSND_CH_SAD(0)) = phys;
  MMIO32(CSND_CH_SIZE(0)) = bytes;
  MMIO32(CSND_CH_LOOP(0)) = phys;
  dsb();
  MMIO32(CSND_CH_CNT(0)) = ((uint32_t)CSND_RATE(rate) << 16) | CH_START |
                           CH_NORMAL | fmt |
                           (loop ? CH_REPEAT_LOOP : CH_REPEAT_ONESHOT);
  dsb();
}

/* Fill the PCM buffer with a square-wave tone; return the sample count. */
static uint32_t gen_tone(uint32_t freq, uint32_t rate) {
  const uint32_t N = 16000;
  const int16_t amp = 0x2800;
  int16_t *buf = (int16_t *)AUDIO_PCM_ADDR;
  uint32_t step = (freq << 16) / rate;
  uint32_t phase = 0;
  for (uint32_t i = 0; i < N; i++) {
    buf[i] = (phase & 0x8000) ? amp : (int16_t)-amp;
    phase = (phase + step) & 0xFFFF;
  }
  dcache_clean();
  return N;
}

/* ==== Wi-Fi driver (GPL-2.0) ====
 * The Wi-Fi SDIO/BMI code in this file (from here to the end of the Wi-Fi
 * section) is licensed GPL-2.0, separately from the rest of AuroraOS. It derives
 * register facts and the BMI/HIF bring-up sequence from the ath6kl legacy driver
 * as ported to the 3DS by Octoblimp. Credit: Octoblimp; ath6kl (GPL-2.0). See
 * docs/wifi.md "License and credits".
 *
 * Wi-Fi SDIO probe of the Atheros host controller at WIFI_SDIO_BASE. Reuses the
 * 16-bit TMIO register layout of Aurora's SD driver (src/sdmmc.h). Every loop is
 * bounded and each phase is published so a bad MMIO access stalls, not hangs. */
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
#define WMASK_GW          0x807Fu /* sdmmc.h TMIO_MASK_GW; ILL_FUNC is not fatal */
#define WMASK_ALL         0x837F031Du

/* Write divider, then divider|enable(bit8), mirrors sdmmc.c setckl(). */
static void wifi_setckl(uint32_t data) {
  WB16(WR_CLKCTL) = (uint16_t)(data & 0xFF);
  WB16(WR_CLKCTL) = (uint16_t)((1u << 8) | (data & 0x2FF));
}

/* Transcription of sdmmc.c sdmmc_controller_init() + set_target() against the
 * Wi-Fi base. The SD-slot-only "mount fix" CFG poke is deliberately omitted. */
static void wifi_ctrl_init(void) {
  WB16(WR_DATACTL32) &= 0xF7FFu;
  WB16(WR_DATACTL32) &= 0xEFFFu;
  WB16(WR_DATACTL32) |= 0x402u;
  WB16(WR_DATACTL) = (uint16_t)((WB16(WR_DATACTL) & 0xFFDD) | 2);
  WB16(WR_DATACTL32) &= 0xFFFFu;
  WB16(WR_DATACTL) &= 0xFFDFu;
  WB16(WR_BLKLEN32) = 512;
  WB16(WR_BLKCOUNT32) = 1;
  WB16(WR_RESET) &= 0xFFFEu; /* assert reset  */
  WB16(WR_RESET) |= 1u;      /* release reset */
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

  /* set_target: port 0, ~523 KHz ID clock, 1-bit bus. */
  WB16(WR_PORTSEL) &= 0xFFFCu;
  wifi_setckl(0x20);
  WB16(WR_OPT) |= 0x8000u; /* 1-bit bus */
}

/* Command word = index | response-type field (R4/R3 = 0x700, R6 = 0x400,
 * R1b = 0x500, R5 = 0x400), following sdmmc.c's encoding. */
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

/* Send a no-data command and wait (bounded) for a response, into log entry `e`. */
static void wifi_cmd(WifiShared *w, WifiCmd *e, uint16_t cmd16, uint32_t arg) {
  uint32_t g = 0;
  while ((WB16(WR_STAT1) & WSTAT1_CMDBUSY) && ++g < 100000u)
    ;
  if (g >= 100000u)
    w->timeouts++;
  WB16(WR_IRM0) = 0;
  WB16(WR_IRM1) = 0;
  WB16(WR_STAT0) = 0;
  WB16(WR_STAT1) = 0;
  WB16(WR_DATACTL32) = (uint16_t)((WB16(WR_DATACTL32) & ~0x1800u) | 0x400u);
  WB16(WR_ARG0) = (uint16_t)(arg & 0xFFFF);
  WB16(WR_ARG1) = (uint16_t)(arg >> 16);
  WB16(WR_CMD) = cmd16;

  g = 0;
  for (;;) {
    uint16_t s1 = WB16(WR_STAT1);
    if (s1 & WMASK_GW)
      break; /* hard error (timeout/CRC/etc.) */
    if (!(s1 & WSTAT1_CMDBUSY) && (WB16(WR_STAT0) & WSTAT0_CMDRESPEND))
      break; /* response complete */
    if (++g >= 300000u) {
      w->timeouts++;
      break;
    }
  }
  uint16_t s0 = WB16(WR_STAT0);
  uint16_t s1 = WB16(WR_STAT1);
  e->cmd = cmd16;
  e->arg = arg;
  e->stat0 = s0;
  e->stat1 = s1;
  e->resp = (uint32_t)WB16(WR_RESP0) | ((uint32_t)WB16(WR_RESP1) << 16);
  e->ok = (s0 & WSTAT0_CMDRESPEND) ? 1u : 2u;
}

/* Append one command to the log (bounded) and return its entry. */
static WifiCmd *wifi_logcmd(WifiShared *w, uint16_t cmd16, uint32_t arg) {
  if (w->nlog >= WIFI_LOG_MAX)
    return &w->log[WIFI_LOG_MAX - 1];
  WifiCmd *e = (WifiCmd *)&w->log[w->nlog];
  wifi_cmd(w, e, cmd16, arg);
  w->nlog++;
  dcache_clean();
  return e;
}

/* Wi-Fi chip reset line: GPIO_DATA4_DATA_OUT_WIFI (ARM11 GPIO). GBATEK: bit0 =
 * "Wifi Enable (0=Reset, need re-upload wifi firmware, 1=On)", wired to the
 * chip's RESET/SYS_RST_L. Setting bit0 brings the Atheros chip out of reset so
 * it powers up and answers on the SDIO bus. This is the piece NWM gets done via
 * the mcu::NWM / GPIO OS services; bare-metal it is set directly. */
#define WIFI_GPIO_WIFI 0x10147028u

#define WR_FIFO     0x30  /* SD_FIFO (16-bit data port) */
#define WR_SDFIFO32 0x10C /* 32-bit FIFO data port */
#define WSTAT1_RXRDY 0x0100u
#define WSTAT1_TXRQ  0x0200u

/* CMD53 (IO_RW_EXTENDED) byte-mode transfer of `len` bytes (a multiple of 4)
 * to/from function `func` at `addr`. The Function-1 mailbox is a CMD53 interface.
 * Uses DATA16 mode (SD_DATACTL 0xD8 bit1 = 0), driving the 16-bit FIFO (0x30) via
 * STAT1 RXRDY(0x100)/TXRQ(0x200). `incr`=1 increments the address; `buf` must be
 * 4-byte aligned. Returns 0 on success, negative on error/timeout. */
static int wifi_cmd53(WifiShared *w, uint32_t func, uint32_t addr,
                      uint32_t *buf, uint32_t len, int is_write, int incr) {
  uint32_t bytes = (len + 3u) & ~3u;
  int nhalf = (int)(bytes / 2u);
  uint16_t *h = (uint16_t *)buf; /* 16-bit FIFO access */
  uint32_t g = 0;
  while ((WB16(WR_STAT1) & WSTAT1_CMDBUSY) && ++g < 100000u)
    ;
  WB16(WR_IRM0) = 0;
  WB16(WR_IRM1) = 0;
  WB16(WR_STAT0) = 0;
  WB16(WR_STAT1) = 0;

  /* DATA16 mode (0xD8 bit1 = 0), block length = this transfer. */
  WB16(WR_DATACTL) &= ~0x0002u;
  WB16(WR_DATACTL32) &= ~0x0002u;
  WB16(WR_STOP) = 0;
  WB16(0x0A) = 1;                    /* SDBLKCOUNT */
  WB16(WR_BLKLEN) = (uint16_t)bytes; /* SDBLKLEN */

  uint32_t arg = ((uint32_t)(is_write & 1) << 31) | ((func & 7u) << 28) |
                 ((uint32_t)(incr & 1) << 26) | ((addr & 0x1FFFFu) << 9) |
                 (len & 0x1FFu); /* byte mode (bit27=0), count in bytes */
  WB16(WR_ARG0) = (uint16_t)(arg & 0xFFFF);
  WB16(WR_ARG1) = (uint16_t)(arg >> 16);
  uint16_t cmd = 0x0035u | 0x0400u | 0x0800u; /* CMD53 | R5 | data present */
  if (!is_write)
    cmd |= 0x1000u; /* read direction */
  WB16(WR_CMD) = cmd;

  int idx = 0, done_data = 0;
  g = 0;
  for (;;) {
    uint16_t s1 = WB16(WR_STAT1);
    if (!is_write && (s1 & WSTAT1_RXRDY) && idx < nhalf) {
      WB16(WR_STAT1) = (uint16_t)(s1 & ~WSTAT1_RXRDY);
      for (int i = 0; i < nhalf; i++)
        h[idx++] = WB16(WR_FIFO);
      done_data = 1;
    }
    if (is_write && (s1 & WSTAT1_TXRQ) && idx < nhalf) {
      WB16(WR_STAT1) = (uint16_t)(s1 & ~WSTAT1_TXRQ);
      for (int i = 0; i < nhalf; i++)
        WB16(WR_FIFO) = h[idx++];
      done_data = 1;
    }
    uint16_t s0 = WB16(WR_STAT0);
    if (s1 & WMASK_GW) {
      w->timeouts++;
      return -1;
    }
    if (!(s1 & WSTAT1_CMDBUSY) && done_data && (s0 & 0x0004u)) /* DATAEND */
      return 0;
    if (++g >= 800000u) {
      w->timeouts++;
      return -2;
    }
  }
}

/* Atheros HIF mailbox 0 base (SDIO function-1 address). */
#define WIFI_MBOX0 0x800u

/* Diagnostic register window (ath6kl ar6000_SetAddressWindowRegister /
 * ReadRegDiag / WriteRegDiag). Reads/writes any target SOC address over SDIO,
 * independent of the target CPU. WINDOW_DATA 0x474, WINDOW_WRITE_ADDR 0x478,
 * WINDOW_READ_ADDR 0x47C. The address's upper 3 bytes are written first, the LSB
 * last (which initiates the access). Credit: Octoblimp / ath6kl. */
static void wifi_diag_setwin(WifiShared *w, uint32_t reg, uint32_t addr) {
  WifiCmd t;
  wifi_cmd(w, &t, WCMD52, WCMD52_WR(1, reg + 1, (addr >> 8) & 0xFFu));
  wifi_cmd(w, &t, WCMD52, WCMD52_WR(1, reg + 2, (addr >> 16) & 0xFFu));
  wifi_cmd(w, &t, WCMD52, WCMD52_WR(1, reg + 3, (addr >> 24) & 0xFFu));
  wifi_cmd(w, &t, WCMD52, WCMD52_WR(1, reg, addr & 0xFFu)); /* LSB triggers */
}

static uint32_t wifi_diag_read(WifiShared *w, uint32_t addr) {
  wifi_diag_setwin(w, 0x47Cu, addr); /* WINDOW_READ_ADDR */
  uint32_t v = 0;
  WifiCmd t;
  for (int i = 0; i < 4; i++) {
    wifi_cmd(w, &t, WCMD52, WCMD52_RD(1, 0x474u + i));
    v |= (t.resp & 0xFFu) << (i * 8);
  }
  return v;
}

static void wifi_diag_write(WifiShared *w, uint32_t addr, uint32_t data) {
  WifiCmd t;
  for (int i = 0; i < 4; i++)
    wifi_cmd(w, &t, WCMD52, WCMD52_WR(1, 0x474u + i, (data >> (i * 8)) & 0xFFu));
  wifi_diag_setwin(w, 0x478u, addr); /* WINDOW_WRITE_ADDR */
}

/* BMI receive one 32-bit word: poll RX_LOOKAHEAD_VALID (0x405) bit0, then read 4
 * bytes from mailbox 0 byte-by-byte via CMD52 (bmiBufferReceive). */
static uint32_t wifi_bmi_recv(WifiShared *w) {
  WifiCmd t;
  for (int i = 0; i < 1500; i++) {
    wifi_cmd(w, &t, WCMD52, WCMD52_RD(1, 0x405));
    if (t.resp & 0x01u)
      break;
    for (volatile int d = 0; d < 300; d++)
      ;
  }
  uint32_t v = 0;
  for (int i = 0; i < 4; i++) {
    wifi_cmd(w, &t, WCMD52, WCMD52_RD(1, WIFI_MBOX0 + i));
    v |= (t.resp & 0xFFu) << (i * 8);
  }
  return v;
}

/* BMI_GET_TARGET_INFO (id 8): send the id, read back version + type. Needs no
 * firmware, so it tests the BMI channel.
 *
 * BMI command credit lives at COUNT_DEC(0x440) + (HTC_MAILBOX_NUM_MAX 4 +
 * ENDPOINT1 0)*4 = 0x450 (counter 4). The response is signalled by
 * RX_LOOKAHEAD_VALID (0x405) bit0 and read from mailbox 0. Credit: Octoblimp. */
static void wifi_bmi_target_info(WifiShared *w) {
  uint16_t dbg[4] = {0, 0, 0, 0};
  WifiCmd t;

  /* COUNT block dump (counter 4 at 0x430 holds the BMI credit). */
  for (int i = 0; i < 8; i++) {
    wifi_cmd(w, &t, WCMD52, WCMD52_RD(1, 0x420 + i * 4));
    w->cnt[i] = (uint8_t)(t.resp & 0xFFu);
  }

  /* Wait for a command credit: read COUNT_DEC[4] = 0x450 (reading its LSB
   * decrements the counter) until it reports non-zero. */
  uint32_t credit = 0;
  for (int i = 0; i < 400; i++) {
    wifi_cmd(w, &t, WCMD52, WCMD52_RD(1, 0x450));
    credit = t.resp & 0xFFu;
    if (credit)
      break;
    for (volatile int d = 0; d < 300; d++)
      ;
  }
  w->bmi_credit = credit;

  /* Write BMI_GET_TARGET_INFO (id 8) via CMD53, EOM-aligned (4 bytes end at
   * 0xFFF, base 0xFFC). */
  static uint32_t cmdw;
  cmdw = 8; /* BMI_GET_TARGET_INFO */
  wifi_cmd53(w, 1, 0x1000u - 4u, &cmdw, 4, 1, 1);
  w->bmi_wr = 0;
  (void)dbg;
  (void)t;

  /* Receive: version word (0xFFFFFFFF sentinel for newer targets like AR6014),
   * then byte_count, then the real version and target type. */
  uint32_t v0 = wifi_bmi_recv(w);
  w->bmi_look = v0;
  if (v0 == 0xFFFFFFFFu) {
    (void)wifi_bmi_recv(w);         /* byte_count */
    w->bmi_ver = wifi_bmi_recv(w);  /* real target version */
    w->bmi_type = wifi_bmi_recv(w); /* target type */
  } else {
    w->bmi_ver = v0;
    w->bmi_type = wifi_bmi_recv(w);
  }
  w->bmi_rd = 0;
}

/* Set once the target stops granting BMI credits for too long: remaining sends
 * become no-ops so the upload aborts instead of writing un-credited data (which
 * corrupts the BMI stream). */
static int g_bmi_abort = 0;
static int g_bmi_consec_nc = 0;
static uint32_t g_nc_hi = 0, g_nc_stub = 0, g_nc_main = 0; /* no-credit per phase */

static void wifi_bmi_send(WifiShared *w, const uint8_t *buf, int len) {
  WifiCmd t;
  if (g_bmi_abort)
    return;
  int got = 0;
  /* Wait for a BMI command credit (0x450 = COUNT_DEC, auto-decrement on read).
   * Read it slowly (~0.5 ms apart): hammering the auto-decrementing counter
   * consumes credits as fast as the target grants them and pins the count at 0. */
  for (int i = 0; i < 120; i++) {
    wifi_cmd(w, &t, WCMD52, WCMD52_RD(1, 0x450));
    if (t.resp & 0xFFu) {
      got = 1;
      break;
    }
    for (volatile int d = 0; d < 150000; d++) /* ~0.5 ms between credit reads */
      ;
  }
  if (got) {
    g_bmi_consec_nc = 0;
  } else {
    w->bmi_nocred++;
    if (w->boot_step == WIFI_BOOT_HI)
      g_nc_hi++;
    else if (w->boot_step == WIFI_BOOT_STUB)
      g_nc_stub++;
    else
      g_nc_main++;
    w->fw_chk = (g_nc_hi & 0xFFu) | ((g_nc_stub & 0xFFu) << 8) |
                ((g_nc_main & 0xFFu) << 16); /* per-phase no-credit breakdown */
    g_bmi_consec_nc++;
    /* Abort before too many un-credited writes accumulate: a healthy boot has
     * only ~8 (structural, harmless); more than ~25 means credits have stalled
     * (bad chip state) and continuing would corrupt the firmware. Clean abort
     * (no freeze, no silent corruption), power-cycle and retry. */
    if (w->bmi_nocred > 25) {
      g_bmi_abort = 1;
      w->bmi_sends++;
      dcache_clean();
      return;
    }
  }
  /* Write the command via a single CMD53 block transfer, end-aligned so the last
   * byte lands on the mailbox EOM address 0xFFF (base = 0x1000 - len). BMI
   * command lengths are all multiples of 4. */
  static uint32_t sbuf[80];
  for (int i = 0; i < len; i++)
    ((uint8_t *)sbuf)[i] = buf[i];
  uint32_t wbase = 0x1000u - (uint32_t)len;
  wifi_cmd53(w, 1, wbase, sbuf, (uint32_t)len, 1, 1);
  (void)t;
  w->bmi_sends++;
  if ((w->bmi_sends & 15) == 0)
    dcache_clean(); /* publish upload progress so the ARM9 can show it live */
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

/* ---- BMI command set (bmi.c: BMIWriteMemory/Execute/Done/SOCRegister/LZ) ----
 * BMI command IDs and the four-blob AR6014 boot sequence are from Octoblimp's
 * ath6kl legacy 3DS port. Commands are written to mailbox 0 via CMD53 (EOM-
 * aligned) and responses read back via CMD52. Each command is capped to the
 * 128-byte target FIFO; the BMI credit gates one command at a time. */
#define BMI_DONE              1u
#define BMI_WRITE_MEMORY      3u
#define BMI_EXECUTE          4u
#define BMI_READ_SOC_REGISTER 6u
#define BMI_WRITE_SOC_REGISTER 7u
#define BMI_LZ_STREAM_START  13u
#define BMI_LZ_DATA          14u
#define BMI_MBOX_FIFO        128u /* target mailbox FIFO size (bmiBufferReceive) */

static void wr32le(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

/* Receive `len` bytes of a BMI response: poll RX_LOOKAHEAD_VALID (0x405) bit0,
 * then read the bytes from mailbox 0 via CMD52. */
static void wifi_bmi_recv_bytes(WifiShared *w, uint8_t *buf, int len) {
  WifiCmd t;
  for (int i = 0; i < 1500; i++) {
    wifi_cmd(w, &t, WCMD52, WCMD52_RD(1, 0x405));
    if (t.resp & 0x01u)
      break;
    for (volatile int d = 0; d < 300; d++)
      ;
  }
  for (int i = 0; i < len; i++) {
    wifi_cmd(w, &t, WCMD52, WCMD52_RD(1, WIFI_MBOX0 + i));
    buf[i] = (uint8_t)(t.resp & 0xFFu);
  }
}

static uint32_t wifi_bmi_recv_word(WifiShared *w) {
  uint8_t b[4];
  wifi_bmi_recv_bytes(w, b, 4);
  return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) |
         ((uint32_t)b[3] << 24);
}

/* BMIWriteMemory: [cid=3][address][length][data...]. Chunked so each command's
 * mailbox write (12-byte header + data) stays within the target FIFO; the final
 * chunk is zero-padded to a 4-byte length as the target expects. */
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

/* BMIExecute: [cid=4][address][param], read back the updated param. Waits for
 * the response with a bound so a misbehaving stub cannot hang the core. */
static uint32_t wifi_bmi_execute(WifiShared *w, uint32_t addr, uint32_t param) {
  uint8_t cmd[12];
  wr32le(cmd + 0, BMI_EXECUTE);
  wr32le(cmd + 4, addr);
  wr32le(cmd + 8, param);
  wifi_bmi_send(w, cmd, 12);
  return wifi_bmi_recv_word(w);
}

/* BMIDone: [cid=1]. Hands the target off from BMI to the running firmware. */
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
  return wifi_bmi_recv_word(w);
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

/* BMILZData: [cid=14][length][data...], chunked within the target FIFO. */
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

/* BMIFastDownload: stream an LZ-compressed image the target decompresses in
 * place (BMILZStreamStart, LZ data 4-byte aligned + padded last word, then a
 * fake stream-start to flush the target caches). */
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
  wifi_bmi_lz_start(w, 0); /* flush target caches */
}

/* Bring the chip up to a working BMI channel: release the reset line, init the
 * host controller, run SDIO enumeration, enable function 1, set its block size,
 * and read the HIF map. On return the diagnostic window and BMI mailbox are
 * usable. Shared by the probe and the firmware boot. */
static void wifi_bringup(WifiShared *w) {
  w->phase = WIFI_PH_NONE;
  w->timeouts = 0;
  w->nlog = 0;
  w->boot_step = WIFI_BOOT_NONE; /* clear any stale boot status for the UI */
  w->boot_exec = 0;
  w->boot_ready = 0;
  dcache_clean();

  /* Full reset pulse of the Wi-Fi chip: drive the reset line low, then high, so
   * the chip re-enters BMI mode from a clean state. A plain "set bit0 = 1" is a
   * no-op when the chip is already on and does NOT recover it after the ARM9 has
   * touched the shared SDMMC block to load firmware from SD (which leaves the
   * chip unresponsive until it is reset). GBATEK: GPIO_DATA4 bit0 = Wi-Fi reset
   * (0 = reset, needs firmware re-upload; 1 = on). */
  w->gpio_before = MMIO16(WIFI_GPIO_WIFI);
  MMIO16(WIFI_GPIO_WIFI) = (uint16_t)(w->gpio_before & ~1u); /* assert reset */
  dcache_clean();
  for (volatile int d = 0; d < 2000000; d++)
    ;
  MMIO16(WIFI_GPIO_WIFI) = (uint16_t)((w->gpio_before & ~1u) | 1u); /* release */
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

  /* SDIO identification: CMD5 (OCR) -> CMD3 (get RCA) -> CMD7 (select). */
  w->phase = WIFI_PH_CMD;
  dcache_clean();

  WifiCmd *e = wifi_logcmd(w, WCMD5, 0);
  uint32_t ocr = e->resp & 0x00FFFFFFu;
  if (ocr == 0)
    ocr = 0x00FF8000u;

  for (int i = 0; i < 4; i++) { /* re-send with OCR until ready bit31 set */
    e = wifi_logcmd(w, WCMD5, ocr);
    if (e->resp & 0x80000000u)
      break;
    for (volatile int d = 0; d < 200000; d++)
      ;
  }

  e = wifi_logcmd(w, WCMD3, 0);
  uint32_t rca = e->resp & 0xFFFF0000u;
  wifi_logcmd(w, WCMD7, rca);

  /* CCCR reads (I/O-level access): 0x00 SDIO rev, 0x08 capability. */
  wifi_logcmd(w, WCMD52, WCMD52_RD(0, 0x00));
  wifi_logcmd(w, WCMD52, WCMD52_RD(0, 0x08));

  /* Walk the CIS (pointer in CCCR 0x09..0x0B) for the MANFID tuple = chip ID. */
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

  /* Enable I/O function 1 (Wi-Fi): CCCR 0x02 IOE bit1, then poll CCCR 0x03 IOR
   * bit1 = function core ready. */
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

  /* Set function-1 SDIO block size to 128 (HIF_MBOX_BLOCK_SIZE) via the FBR
   * block-size registers (func0 0x110 low / 0x111 high). ath6kl's hifEnableFunc
   * does this before any mailbox access. Credit: Octoblimp / ath6kl. */
  wifi_logcmd(w, WCMD52, WCMD52_WR(0, 0x110, 128 & 0xFF));
  wifi_logcmd(w, WCMD52, WCMD52_WR(0, 0x111, (128 >> 8) & 0xFF));

  /* Enable the SDIO card interrupt (CCCR 0x04 INT_ENABLE): master IENM bit0 +
   * function-1 bit1. ath6kl's hifEnableFunc enables this before the mailbox is
   * used; some Atheros targets only latch the RX lookahead (and therefore only
   * signal a pending HTC message) once the SDIO interrupt is enabled. */
  {
    WifiCmd ci;
    wifi_cmd(w, &ci, WCMD52, WCMD52_WR(0, 0x04, 0x03));
  }

  /* Function-1 HIF register block 0x400..0x40F (HOST_INT_STATUS etc. per ath6kl). */
  for (int i = 0; i < 16; i++) {
    wifi_cmd(w, &tmp, WCMD52, WCMD52_RD(1, 0x400 + i));
    w->hif[i] = (uint8_t)(tmp.resp & 0xFFu);
  }

  w->diag_a = wifi_diag_read(w, 0x00500400u); /* host interest, reference */
}

/* Common tail: snapshot the controller registers and mark the run complete. */
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
  wifi_bringup(w);
  wifi_bmi_target_info(w);

  /* Verify BMI_WRITE_MEMORY end-to-end: write a word via BMI, read it back via
   * the diag window. diag_b == 0xCAFEBABE means the BMI memory-write path works
   * (the foundation for firmware upload). */
  wifi_bmi_write_word(w, 0x00520100u, 0xCAFEBABEu);
  w->diag_b = wifi_diag_read(w, 0x00520100u);

  wifi_finish(w);
}

/* AR6014 four-blob boot sequence (ar6000_ar6002_boot_firmware). Addresses and
 * ordering are verbatim from Octoblimp's ath6kl legacy 3DS port; the blobs are
 * SD-staged into the WIFI_FW slots by the ARM9. Credit: Octoblimp / ath6kl. */
#define AR6014_VERSION       0x2300006Fu
#define AR6014_TYPE          2u          /* TARGET_TYPE_AR6002 */
#define AR6014_HI            0x00520000u
#define AR6014_PARTA_DST     0x00524C00u /* main firmware (LZ)  */
#define AR6014_PARTB_DST     0x0053FE18u /* database            */
#define AR6014_PARTC_DST     0x00527000u /* stub code           */
#define AR6014_PARTD_DST     0x00524C00u /* stub data           */
#define AR6014_EXEC_MIRROR   0x00400000u
#define HTC_PROTOCOL_VERSION 0x0002u

/* HTC (host-target comm) sits on the mailbox once BMI hands off. After boot the
 * running firmware posts one HTC control message, HTC_READY, on mailbox 0. Poll
 * HOST_INT_STATUS(0x400) mbox0-data + RX_LOOKAHEAD_VALID(0x405), read the 4-byte
 * HTC frame header from the lookahead register (0x408) for the payload length,
 * then drain the full message from mailbox 0. HTC frame header = EndpointID(1),
 * Flags(1), PayloadLen(2); payload follows a 6-byte header (2 control bytes).
 * HTC_READY_MSG = MessageID(2, =1), CreditCount(2), CreditSize(2), MaxEp(1).
 * Reference: ath6kl htc.c HTCWaitTarget / DevPollMboxMsgRecv. Credit: Octoblimp. */
#define HTC_HDR_LENGTH   6u
#define HTC_MSG_READY_ID 1u

/* Enable the mailbox-0, counter, CPU and error interrupts (INT_STATUS_ENABLE
 * block at 0x418), as ath6kl DevEnableInterrupts does, so HOST_INT_STATUS
 * reflects mailbox-0 data. int_status_enable = ERROR|CPU|COUNTER|MBOX0 = 0xD1;
 * cpu_int = 0; error_status = RX_UNDERFLOW|TX_OVERFLOW = 0x03; counter = 0x01. */
static void wifi_htc_enable_ints(WifiShared *w) {
  static const uint8_t en[4] = {0xD1, 0x00, 0x03, 0x01};
  WifiCmd t;
  for (int i = 0; i < 4; i++)
    wifi_cmd(w, &t, WCMD52, WCMD52_WR(1, 0x418 + i, en[i]));
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

  /* Watch mailbox 0 for a pending message from the very start (no settle first):
   * poll RX_LOOKAHEAD_VALID(0x405) bit0 or HOST_INT_STATUS(0x400) mbox-data bits.
   * Occasionally sample the NWM ready flag (HI+0x58) via the diag window. Bail if
   * CMD52 stops being acked so the core can't freeze for minutes. */
  uint32_t t0 = w->timeouts;
  int have = 0;
  for (int i = 0; i < 6000; i++) { /* ~1 s, bounded so the core returns to its
                                    * loop (touch keeps working); 0x405/0x400 are
                                    * status regs, so fast reads are fine here */
    wifi_cmd(w, &t, WCMD52, WCMD52_RD(1, 0x405));
    uint8_t lav = (uint8_t)(t.resp & 0xFFu);
    wifi_cmd(w, &t, WCMD52, WCMD52_RD(1, 0x400));
    uint8_t his = (uint8_t)(t.resp & 0xFFu);
    if ((lav & 0x01u) || (his & 0x0Fu)) {
      have = 1;
      break;
    }
    if ((i % 400) == 0) { /* sample the NWM ready flag every so often */
      if (wifi_diag_read(w, AR6014_HI + 0x58u) == 1u)
        w->boot_ready = 1;
    }
    if (w->timeouts - t0 > 8u)
      break; /* chip no longer acking register reads */
    for (volatile int d = 0; d < 300; d++)
      ;
  }

  /* Diagnostic snapshot: HOST_INT_STATUS 0x400 | CPU_INT 0x401 | ERROR_INT 0x402
   * | RX_LOOKAHEAD_VALID 0x405, so an assert (error/cpu int) after boot is
   * visible even when no mailbox message ever appears. */
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
    /* Nothing flagged: peek mailbox 0 directly (CMD52) and grab mbox_frame(0x404)
     * in case the firmware posted but the lookahead register isn't showing it. */
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

  uint32_t payloadlen = (uint32_t)hdr[2] | ((uint32_t)hdr[3] << 8);
  uint32_t total = payloadlen + HTC_HDR_LENGTH;
  if (total > 32u)
    total = 32u;
  uint8_t msg[32];
  for (uint32_t i = 0; i < total; i++) {
    wifi_cmd(w, &t, WCMD52, WCMD52_RD(1, WIFI_MBOX0 + i));
    msg[i] = (uint8_t)(t.resp & 0xFFu);
  }
  if (total >= HTC_HDR_LENGTH + 6u) {
    w->htc_msgid = (uint32_t)msg[6] | ((uint32_t)msg[7] << 8);
    w->htc_credits = (uint32_t)msg[8] | ((uint32_t)msg[9] << 8);
    w->htc_credsz = (uint32_t)msg[10] | ((uint32_t)msg[11] << 8);
  }
}

static void wifi_boot_firmware(WifiShared *w) {
  const WifiFw *fw = (const WifiFw *)WIFI_FW_ADDR;
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

  wifi_bmi_target_info(w);
  if (w->bmi_ver != AR6014_VERSION || w->bmi_type != AR6014_TYPE) {
    w->boot_step = WIFI_BOOT_BADVER;
    dcache_clean();
    return;
  }

  /* 1. HTC protocol version into host interest, then the clock/sleep SOC setup.
   *    Let the target CPU clock settle afterwards before the heavy uploads. */
  w->boot_step = WIFI_BOOT_HI;
  dcache_clean();
  wifi_bmi_write_word(w, AR6014_HI, HTC_PROTOCOL_VERSION);
  wifi_bmi_write_soc(w, 0x000180C0u, wifi_bmi_read_soc(w, 0x000180C0u) | 0x8u);
  wifi_bmi_write_soc(w, 0x000040C4u, wifi_bmi_read_soc(w, 0x000040C4u) | 0x1u);
  wifi_bmi_write_soc(w, 0x00004028u, 0x5u);
  wifi_bmi_write_soc(w, 0x00004020u, 0x0u);
  for (volatile int d = 0; d < 2000000; d++) /* clock stabilise (~20 ms) */
    ;
  if (g_bmi_abort) {
    dcache_clean();
    return;
  }

  /* 2. Upload stub_data + stub_code, then execute the NWM helper stub. */
  w->boot_step = WIFI_BOOT_STUB;
  dcache_clean();
  wifi_bmi_write_mem(w, AR6014_PARTD_DST, (const uint8_t *)WIFI_FW_STUBDATA,
                     fw->stubdata_len);
  wifi_bmi_write_mem(w, AR6014_PARTC_DST, (const uint8_t *)WIFI_FW_STUBCODE,
                     fw->stubcode_len);
  /* Verify the raw upload landed byte-for-byte: read the stub_code start back
   * via the diag window now, before it runs and before anything overwrites it.
   * fw_dbrd should equal fw_dbex (the first word of the blob just sent). This
   * is the conclusive test of whether mailbox writes deliver intact. */
  {
    const uint32_t *sc = (const uint32_t *)WIFI_FW_STUBCODE;
    w->fw_dbrd = wifi_diag_read(w, AR6014_PARTC_DST);
    w->fw_dbex = sc[0];
  }
  dcache_clean();
  w->boot_exec = wifi_bmi_execute(w, AR6014_PARTC_DST + AR6014_EXEC_MIRROR,
                                  AR6014_PARTC_DST);
  for (volatile int d = 0; d < 10000000; d++) /* let the NWM stub finish (~100 ms) */
    ;
  if (g_bmi_abort) {
    dcache_clean();
    return;
  }

  /* 3. Fast-download the compressed main firmware, re-upload stub_data, upload
   *    the database, then set the host-interest pointers NWM expects. */
  w->boot_step = WIFI_BOOT_MAIN;
  dcache_clean();
  wifi_bmi_fast_download(w, AR6014_PARTA_DST, (const uint8_t *)WIFI_FW_MAIN,
                         fw->main_len);
  wifi_bmi_write_mem(w, AR6014_PARTD_DST, (const uint8_t *)WIFI_FW_STUBDATA,
                     fw->stubdata_len);
  wifi_bmi_write_mem(w, AR6014_PARTB_DST, (const uint8_t *)WIFI_FW_DATABASE,
                     fw->database_len);
  wifi_bmi_write_word(w, AR6014_HI + 0x18u, AR6014_PARTB_DST);
  wifi_bmi_write_word(w, AR6014_HI + 0x6Cu, 0x80u);
  wifi_bmi_write_word(w, AR6014_HI + 0x74u, 0x63u);
  if (g_bmi_abort) {
    dcache_clean();
    return;
  }

  /* 4. Arm target mailbox interrupts before the handoff, then watch for HTC_READY.
   * The AR6K driver applies this target-side configuration before unmasking its
   * host-side SDIO event loop. */
  wifi_htc_enable_ints(w);

  /* Hand off from BMI, then IMMEDIATELY watch the mailbox for HTC_READY. The
   *    firmware posts HTC_READY and, on the AR6014, asserts / aborts if the host
   *    is slow to answer the HTC handshake, a message posted-then-cleared would
   *    be missed by a delayed poll, so start watching the instant BMIDone lands
   *    (the NWM ready flag is polled inside, occasionally, via the diag window). */
  wifi_bmi_done(w);
  w->boot_step = WIFI_BOOT_DONE;
  dcache_clean();
  wifi_htc_wait_ready(w);
  dcache_clean();
}

static void wifi_boot_run(void) {
  WifiShared *w = (WifiShared *)WIFI_SHARED_ADDR;
  g_bmi_abort = 0;
  g_bmi_consec_nc = 0;
  g_nc_hi = g_nc_stub = g_nc_main = 0;
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
  wifi_bringup(w);
  wifi_boot_firmware(w);
  wifi_finish(w);
}

/* ARM11 exception stubs live in audio11_start.s. Install them so a fault in the
 * audio core is captured into the cross-core crash block for the ARM9 to show.
 * Best effort: assumes the ARM11 vector page is writable (like the audio
 * bring-up, this is untested register territory). */
extern void crash_vec_undef11(void);
extern void crash_vec_pabt11(void);
extern void crash_vec_dabt11(void);
extern void crash_hang11(void);

static void crash11_init(void) {
  uint32_t sctlr;
  __asm__ volatile("mrc p15, 0, %0, c1, c0, 0" : "=r"(sctlr));
  volatile uint32_t *vec =
      (volatile uint32_t *)((sctlr & (1u << 13)) ? 0xFFFF0000u : 0u);

  for (int i = 0; i < 8; i++)
    vec[i] = 0xE59FF018u; /* LDR PC, [PC, #0x18] */
  vec[8]  = (uint32_t)crash_hang11;      /* reset    */
  vec[9]  = (uint32_t)crash_vec_undef11; /* undef    */
  vec[10] = (uint32_t)crash_hang11;      /* swi      */
  vec[11] = (uint32_t)crash_vec_pabt11;  /* prefetch */
  vec[12] = (uint32_t)crash_vec_dabt11;  /* data     */
  vec[13] = (uint32_t)crash_hang11;      /* reserved */
  vec[14] = (uint32_t)crash_hang11;      /* irq      */
  vec[15] = (uint32_t)crash_hang11;      /* fiq      */

  __asm__ volatile("mcr p15, 0, %0, c7, c10, 0" ::"r"(0)); /* clean D-cache  */
  __asm__ volatile("mcr p15, 0, %0, c7, c5, 0" ::"r"(0));  /* invalidate I   */
  __asm__ volatile("mcr p15, 0, %0, c7, c10, 4" ::"r"(0)); /* DSB            */
}

/* ARM11 core entry + command loop */

void audio11_main(void) {
  AudioCtrl *ct = (AudioCtrl *)AUDIO_CTRL_ADDR;

  ct->status = AUDIO_ST_BOOT;
  ct->ack_seq = ct->cmd_seq;
  ct->version = AUDIO_CORE_VERSION;
  ct->diag0 = ct->diag1 = ct->diag2 = ct->diag3 = 0;
  ct->diag4 = ct->diag5 = ct->diag6 = ct->diag7 = 0;
  ct->magic = AUDIO_MAGIC;
  dcache_clean();

  /* NOTE: crash11_init() (ARM11 exception-vector install) is DISABLED for now.
   * Its vector write is untested register territory and is the prime suspect
   * for hanging the core before the main loop (seq stuck at 0, no audio). Bring
   * it back only once the vector page is confirmed writable on ARM11. */
  (void)crash11_init;

  codec_init();
  /* Diagnostics: read back codec ID/rev registers and one written register.
   * All-0x00 or all-0xFF here means the codec SPI link is not working. */
  ct->diag0 = cdc_read_word(CDC_0_2); /* raw 32-bit read: shows byte lane */
  /* Write-then-read-back verify on reg 101.11 (isolates write vs read). */
  cdc_write(CDC_101_11, 0x2A);
  ct->diag6 = cdc_read_word(CDC_101_11);
  ct->diag1 = g_spi_timeouts;
  ct->diag4 = MMIO16(CFG11_SPI_CNT); /* did new-SPI-interface enable stick? */
  ct->diag5 = MMIO32(NSPI_CNT);      /* NSPI bus control state after xfers  */
  ct->status = AUDIO_ST_CODEC;
  dcache_clean();

  csnd_init();
  ct->diag2 = MMIO32(CSND_MAIN); /* readback: did the master write stick? */
  ct->status = AUDIO_ST_READY;
  dcache_clean();

  /* Configure the codec touchscreen ADC after audio is fully up, so a problem
   * here can't stop audio/CSND from initialising. */
  touch_init();

  for (;;) {
    dcache_clean_inval();
    if (ct->cmd_seq != ct->ack_seq) {
      uint32_t cmd = ct->cmd;
      uint32_t arg0 = ct->arg0;
      if (cmd == AUDIO_CMD_TONE) {
        uint32_t n = gen_tone(arg0 ? arg0 : 440, TONE_RATE);
        csnd_play(AUDIO_PCM_ADDR, n * 2u, TONE_RATE, CH_FORMAT_PCM16, 1);
        ct->diag3 = MMIO32(CSND_CH_CNT(0)); /* did the channel start? */
        ct->status = AUDIO_ST_PLAY;
      } else if (cmd == AUDIO_CMD_PCM) {
        /* PCM already loaded at AUDIO_PCM_ADDR by the ARM9. */
        uint32_t samples = ct->arg1;
        uint32_t rate = ct->arg2;
        uint32_t depth = ct->arg3;
        uint32_t fmt = (depth == 8) ? 0u : CH_FORMAT_PCM16;
        uint32_t bytes = samples * ((depth == 8) ? 1u : 2u);
        csnd_play(AUDIO_PCM_ADDR, bytes, rate ? rate : 8000u, fmt, 0);
        ct->diag3 = MMIO32(CSND_CH_CNT(0));
        ct->status = AUDIO_ST_PLAY;
      } else if (cmd == AUDIO_CMD_STOP) {
        csnd_stop();
        ct->status = AUDIO_ST_IDLE;
      } else if (cmd == AUDIO_CMD_WIFI) {
        wifi_probe_run();
      } else if (cmd == AUDIO_CMD_WIFI_BOOT) {
        wifi_boot_run();
      } else if (cmd == AUDIO_CMD_GPU) {
        gpu11_run();
      }
      ct->ack_seq = ct->cmd_seq;
      dcache_clean();
    }
    touch_poll(); /* sample the touchscreen every loop */
    spin(20000);
  }
}
