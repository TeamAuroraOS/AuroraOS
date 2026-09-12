/*
 * Audio, ARM11 side: the analog output path and the CSND mixer. The codec bus
 * itself is Codec11.c, shared with the touchscreen. See docs/audio.md.
 *
 * The output bring-up is ported from profi200's libn3ds
 * (source/arm11/drivers/{codec,i2s}.c).
 *
 * Not done here, and untested as a result: calibration values normally come
 * from the console's HWCAL block, so neutral defaults are used; headphone-jack
 * detection is skipped and output forced to the speaker; no EQ upload; no
 * per-unit depop pulsing.
 */
#include "core11.h"

/* ---- codec registers (page<<8 | offset), from libn3ds codec_regmap.h ---- */
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
  /* The bus and the chip reset are Codec11.c's job; this is the analog
   * output path only. */

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


/* CSND audio (GBATEK) */
/* CSND master control (0x10103000, u32): bits0-15 vol, bit16 mute (0=on),
 * bit30 = normal, bit31 = allow channel writes (required before ch start). */
#define CSND_MAIN     0x10103000u
#define CSND_MAIN_VAL (0x8000u | (1u << 30) | (1u << 31))

/* Channel blocks: 0x10103400 + n*0x20. */
#define CSND_CH(n)      (0x10103400u + (uint32_t)(n) * 0x20u)
#define CSND_CH_CNT(n)  (CSND_CH(n) + 0x00) /* u16: flags, bit15 = start      */
#define CSND_CH_TIMER(n) (CSND_CH(n) + 0x02) /* u16 write-only: period reload  */
#define CSND_CH_VOL(n)  (CSND_CH(n) + 0x04) /* u32: volR | volL<<16              */
#define CSND_CH_SAD(n)  (CSND_CH(n) + 0x0C) /* u32: sample source phys addr      */
#define CSND_CH_SIZE(n) (CSND_CH(n) + 0x10) /* u32: total size in BYTES          */
#define CSND_CH_LOOP(n) (CSND_CH(n) + 0x14) /* u32: loop restart phys addr       */

/* Control is 16-bit, and the reload beside it is write-only and holds the
 * two's complement of the period. libctru's `timer << 16` packing is its
 * service's argument format, not a register image, so writing it here loses
 * the divider in a discarded upper half. docs/audio.md has the full story. */
#define CH_LINEAR_INTERP  (1u << 6)  /* resample smoothly instead of nearest  */
#define CH_REPEAT_LOOP    (1u << 10) /* loop mode 1 = loop infinite           */
#define CH_REPEAT_ONESHOT (2u << 10) /* loop mode 2 = one-shot                */
#define CH_FORMAT_PCM16   (1u << 12) /* encoding 1 = PCM16                    */
#define CH_ENABLE         (1u << 14)
#define CH_START          (1u << 15)

/* Clock divider for `rate` (libctru CSND_TIMER), clamped the way libctru does:
 * masking would silently wrap any rate below ~1023 Hz. */
#define CSND_TIMER_MIN 0x0042u
#define CSND_TIMER_MAX 0xFFFFu
static uint32_t csnd_timer(uint32_t rate) {
  uint32_t t = rate ? (0x3FEC3FCu / rate) : CSND_TIMER_MAX;
  if (t < CSND_TIMER_MIN)
    t = CSND_TIMER_MIN;
  if (t > CSND_TIMER_MAX)
    t = CSND_TIMER_MAX;
  return t;
}

#define TONE_RATE 32000u

static void csnd_init(void) {
  MMIO32(CSND_MAIN) = CSND_MAIN_VAL;
  dsb();
}
static void csnd_stop(void) {
  MMIO16(CSND_CH_CNT(0)) = 0;
  dsb();
}
/* Play `bytes` of PCM at `phys`. fmt = CH_FORMAT_PCM16 (16-bit) or 0 (8-bit);
 * loop != 0 loops forever, else one-shot. */
static void csnd_play(uint32_t phys, uint32_t bytes, uint32_t rate, uint32_t fmt,
                      int loop) {
  uint32_t timer = csnd_timer(rate);
  uint32_t reload = (0x10000u - timer) & 0xFFFFu;

  csnd_stop();
  MMIO32(CSND_CH_VOL(0)) = 0x80008000u; /* full L + R */
  MMIO32(CSND_CH_SAD(0)) = phys;
  MMIO32(CSND_CH_SIZE(0)) = bytes;
  MMIO32(CSND_CH_LOOP(0)) = phys;
  /* The reload must be in place before the start bit, or the channel begins on
   * whatever the previous sound left there. */
  MMIO16(CSND_CH_TIMER(0)) = (uint16_t)reload;
  dsb();
  MMIO16(CSND_CH_CNT(0)) = (uint16_t)(CH_START | CH_ENABLE | CH_LINEAR_INTERP |
                                      fmt |
                                      (loop ? CH_REPEAT_LOOP
                                            : CH_REPEAT_ONESHOT));
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


/* Render the crash beep: three tones separated by silence, as one buffer that
 * can be played with a single channel start. Done at boot so the fault path
 * never has to build it. */
static void audio11_error_prepare(void) {
  int16_t *buf = (int16_t *)AUDIO_ERR_ADDR;
  const int16_t amp = 0x3000;
  uint32_t step = (AUDIO_ERR_FREQ << 16) / AUDIO_ERR_RATE;
  uint32_t phase = 0;
  uint32_t i = 0;

  for (int beep = 0; beep < 3; beep++) {
    for (uint32_t n = 0; n < AUDIO_ERR_BEEP; n++) {
      buf[i++] = (phase & 0x8000) ? amp : (int16_t)-amp;
      phase = (phase + step) & 0xFFFF;
    }
    if (beep < 2)
      for (uint32_t n = 0; n < AUDIO_ERR_GAP; n++)
        buf[i++] = 0;
  }
  dcache_clean(); /* CSND reads physical memory, so this must not sit in cache */
}

void audio11_error_play(void) {
  csnd_play(AUDIO_ERR_ADDR, AUDIO_ERR_BYTES, AUDIO_ERR_RATE, CH_FORMAT_PCM16, 0);
}

void audio11_init(AudioCtrl *ct) {
  audio11_error_prepare();
  codec_init();
  /* Diagnostics: read back codec ID/rev registers and one written register.
   * All-0x00 or all-0xFF here means the codec SPI link is not working. */
  ct->diag0 = cdc_read_word(CDC_0_2);
  cdc_write(CDC_101_11, 0x2A);
  ct->diag6 = cdc_read_word(CDC_101_11);
  ct->diag1 = codec11_spi_timeouts();
  ct->diag4 = MMIO16(CFG11_SPI_CNT); /* did new-SPI-interface enable stick? */
  ct->diag5 = MMIO32(NSPI_CNT);      /* NSPI bus control state after xfers  */
  ct->status = AUDIO_ST_CODEC;
  dcache_clean();

  csnd_init();
  ct->diag2 = MMIO32(CSND_MAIN); /* readback: did the master write stick? */
  ct->status = AUDIO_ST_READY;
  dcache_clean();
}

/* Handle one audio command; returns 1 if it was one of ours. */
int audio11_command(AudioCtrl *ct, uint32_t cmd, uint32_t arg0) {
  if (cmd == AUDIO_CMD_TONE) {
    uint32_t n = gen_tone(arg0 ? arg0 : 440, TONE_RATE);
    csnd_play(AUDIO_PCM_ADDR, n * 2u, TONE_RATE, CH_FORMAT_PCM16, 1);
    ct->diag3 = MMIO32(CSND_CH_CNT(0)); /* did the channel start? */
    ct->status = AUDIO_ST_PLAY;
  } else if (cmd == AUDIO_CMD_PCM) {
    /* PCM already loaded at AUDIO_PCM_ADDR by the ARM9. */
    uint32_t fmt = (ct->arg3 == 8) ? 0u : CH_FORMAT_PCM16;
    uint32_t bytes = ct->arg1 * ((ct->arg3 == 8) ? 1u : 2u);
    csnd_play(AUDIO_PCM_ADDR, bytes, ct->arg2 ? ct->arg2 : 8000u, fmt, 0);
    ct->diag3 = MMIO32(CSND_CH_CNT(0));
    ct->status = AUDIO_ST_PLAY;
  } else if (cmd == AUDIO_CMD_ERROR) {
    audio11_error_play();
    ct->status = AUDIO_ST_PLAY;
  } else if (cmd == AUDIO_CMD_STOP) {
    csnd_stop();
    ct->status = AUDIO_ST_IDLE;
  } else {
    return 0;
  }
  return 1;
}
