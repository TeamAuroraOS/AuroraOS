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
 * which isn't don't read here -- we use neutral defaults. Headphone-jack detection
 * is skipped (output forced to the speaker path). EQ-filter upload is skipped
 * (codec defaults). Per-unit "depop" GPIO pulsing is skipped.
 */
#include "audio.h"
#include "touch.h"
#include "wifi.h"

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

/* NSPI bus 2 (0x10142800) -- the CTR audio codec lives here (CS0, 16 MHz). */
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
  /* NSPI_DEV_CS_HIGH set => raise CS (deassert) at the end of the transaction.
   * (This condition was previously inverted, so CS never deasserted and the
   * codec never saw a completed transaction -- reads and writes both dead.) */
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
 *   Copyright (C) 2019 Wolfvak    -- licensed GPL v2-or-later.
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

/* Wi-Fi SDIO probe of the Atheros host controller at WIFI_SDIO_BASE. Reuses the
 * 16-bit TMIO register layout of Aurora's SD driver (src/sdmmc.h, values
 * transcribed from there -- no NWM code). Every loop is bounded and each phase
 * is published so a bad MMIO access stalls a phase rather than hanging. */
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

/* Write divider, then divider|enable(bit8) -- mirrors sdmmc.c setckl(). */
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
 * the mcu::NWM / GPIO OS services; bare-metal we set it directly. */
#define WIFI_GPIO_WIFI 0x10147028u

#define WR_FIFO 0x30 /* SD_FIFO (16-bit data port) */
#define WSTAT1_RXRDY 0x0100u
#define WSTAT1_TXRQ  0x0200u

/* CMD53 (IO_RW_EXTENDED) byte-mode transfer of `nwords` 32-bit words to/from
 * function `func` at `addr`. `incr`=1 increments the address, 0 = fixed.
 * Returns 0 on success, negative on error/timeout. */
static int wifi_cmd53(WifiShared *w, uint32_t func, uint32_t addr,
                      uint32_t *buf, int nwords, int is_write, int incr,
                      uint16_t *dbg) {
  uint32_t bytes = (uint32_t)nwords * 4u;
  uint16_t *h = (uint16_t *)buf; /* 16-bit FIFO access */
  int nhalf = (int)(bytes / 2u);
  uint32_t g = 0;
  while ((WB16(WR_STAT1) & WSTAT1_CMDBUSY) && ++g < 100000u)
    ;
  WB16(WR_IRM0) = 0;
  WB16(WR_IRM1) = 0;
  WB16(WR_STAT0) = 0;
  WB16(WR_STAT1) = 0;

  /* Use the 16-bit FIFO path (the controller signals via STAT1 RXRDY/TXRQ for
   * these small transfers, not the 32-bit DATACTL32 flags). */
  WB16(WR_DATACTL32) &= ~0x0002u; /* disable 32-bit FIFO */
  WB16(WR_STOP) = 0;
  WB16(0x0A) = 1;                    /* SDBLKCOUNT */
  WB16(WR_BLKLEN) = (uint16_t)bytes; /* SDBLKLEN */

  uint32_t arg = ((uint32_t)(is_write & 1) << 31) | ((func & 7u) << 28) |
                 ((uint32_t)(incr & 1) << 26) | ((addr & 0x1FFFFu) << 9) |
                 (bytes & 0x1FFu); /* byte mode (bit27=0), count in bytes */
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
    if (dbg) {
      dbg[0] = s0;
      dbg[1] = s1;
      dbg[2] = (uint16_t)idx;
      dbg[3] = (uint16_t)g;
    }
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

/* Atheros HIF mailbox 0 (SDIO function-1 address, per ath6kl). Reads use the
 * base 0x800. WRITES must END at the mailbox boundary (0xFFF) for the target to
 * treat the packet as complete, so the write address is base + (WIDTH - len) =
 * 0x1000 - len (ath6kl's mailbox write-address adjustment). */
#define WIFI_MBOX0        0x800u
#define WIFI_MBOX_WIDTH   0x800u
#define WIFI_MBOX0_WR(len) (WIFI_MBOX0 + (WIFI_MBOX_WIDTH - (len)))

/* BMI_GET_TARGET_INFO (id 8): send the id, read back version + type. Needs no
 * firmware, so it tests the BMI channel. NOTE: does not yet get a response --
 * the target never sets mbox-data-pending; see docs/wifi.md for the open work. */
static void wifi_bmi_target_info(WifiShared *w) {
  uint16_t dbg[4] = {0, 0, 0, 0};

  /* Dump the 8 COUNT registers (0x420..0x43F) low bytes: counter 1 holds the
   * BMI command credit on this chip. */
  WifiCmd tc;
  for (int i = 0; i < 8; i++) {
    wifi_cmd(w, &tc, WCMD52, WCMD52_RD(1, 0x420 + i * 4));
    w->cnt[i] = (uint8_t)(tc.resp & 0xFFu);
  }

  /* Consume the credit by reading COUNT_DEC counter 1 (0x444, auto-decrements)
   * -- the flow-control step before writing a BMI command. */
  WifiCmd tcr;
  wifi_cmd(w, &tcr, WCMD52, WCMD52_RD(1, 0x444));
  w->bmi_credit = tcr.resp & 0xFFu;

  uint32_t cmd = 8; /* BMI_GET_TARGET_INFO */
  w->bmi_wr = wifi_cmd53(w, 1, WIFI_MBOX0_WR(4), &cmd, 1, 1 /*write*/, 1, 0);

  /* Poll HOST_INT_STATUS (0x400) low nibble = mailbox-0 data pending. */
  WifiCmd t;
  uint32_t polls, hs = 0;
  for (polls = 0; polls < 4000; polls++) {
    wifi_cmd(w, &t, WCMD52, WCMD52_RD(1, 0x400));
    hs = t.resp & 0xFFu;
    if (hs & 0x0Fu)
      break;
    for (volatile int d = 0; d < 3000; d++)
      ;
  }
  uint32_t r403, r405;
  wifi_cmd(w, &t, WCMD52, WCMD52_RD(1, 0x403));
  r403 = t.resp & 0xFFu;
  wifi_cmd(w, &t, WCMD52, WCMD52_RD(1, 0x405));
  r405 = t.resp & 0xFFu;
  w->bmi_look = hs | (r403 << 8) | (r405 << 16) | ((polls & 0xFF) << 24);

  uint32_t ver = 0, type = 0;
  w->bmi_rd = wifi_cmd53(w, 1, WIFI_MBOX0, &ver, 1, 0 /*read*/, 1, dbg);
  w->bmi_s0 = dbg[0];
  w->bmi_s1 = dbg[1];
  w->bmi_ctl = dbg[2];
  w->bmi_idx = dbg[3];
  w->bmi_ver = ver;
  wifi_cmd53(w, 1, WIFI_MBOX0, &type, 1, 0, 1, 0);
  w->bmi_type = type;
}

static void wifi_probe_run(void) {
  WifiShared *w = (WifiShared *)WIFI_SHARED_ADDR;
  w->phase = WIFI_PH_NONE;
  w->timeouts = 0;
  w->nlog = 0;
  dcache_clean();

  /* Release the chip's hardware reset (GPIO_DATA4 bit0 = 1) and let it boot. */
  w->gpio_before = MMIO16(WIFI_GPIO_WIFI);
  MMIO16(WIFI_GPIO_WIFI) = (uint16_t)(w->gpio_before | 1u);
  dcache_clean();
  for (volatile int d = 0; d < 4000000; d++)
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

  /* Function-1 HIF register block 0x400..0x40F (HOST_INT_STATUS etc. per ath6kl). */
  for (int i = 0; i < 16; i++) {
    wifi_cmd(w, &tmp, WCMD52, WCMD52_RD(1, 0x400 + i));
    w->hif[i] = (uint8_t)(tmp.resp & 0xFFu);
  }

  wifi_bmi_target_info(w);

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
  /* Diagnostics: read back codec ID/rev registers and one register we wrote.
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
      }
      ct->ack_seq = ct->cmd_seq;
      dcache_clean();
    }
    touch_poll(); /* sample the touchscreen every loop */
    spin(20000);
  }
}
