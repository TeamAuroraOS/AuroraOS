/*
 * Shared internals for the ARM11 core's modules.
 *
 * The core is one binary built from Core11.c plus a file per subsystem
 * (Audio11, Touch11, WiFi11, Gpu11, Codec11). This header carries what they
 * all need: register access, cache maintenance, timing, the SoC I/O block
 * addresses, and each module's entry points.
 */
#ifndef AURORA_CORE11_H
#define AURORA_CORE11_H

#include <stdint.h>
#include "audio.h"

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
/* Single-line cache maintenance, for the command poll. Cleaning or
 * invalidating the whole cache every pass is what kept command latency tied to
 * the touch sampling interval. */
static inline void dcache_inval_line(const void *p) {
  __asm__ volatile("mcr p15, 0, %0, c7, c6, 1" ::"r"(p) : "memory");
}
static inline void dcache_clean_line(const void *p) {
  __asm__ volatile("mcr p15, 0, %0, c7, c10, 1" ::"r"(p) : "memory");
}
static inline void dcache_clean_inval(void) {
  __asm__ volatile("mcr p15, 0, %0, c7, c14, 0" ::"r"(0) : "memory");
  dsb();
}

static inline void spin(uint32_t n) {
  while (n--)
    __asm__ volatile("nop");
}
/* Coarse millisecond sleep (over-sleeps slightly; only used for codec settle
 * delays, so erring long is fine). ~300k nops/ms is comfortably >= 1 ms at the
 * ARM11's clock. */
static inline void sleep_ms(uint32_t ms) { spin(ms * 300000u); }

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

/* ---- Codec11.c: NSPI transport and CTR codec register access -------------
 * The CTR codec carries both the audio path and the touchscreen ADC, so the
 * bus belongs to neither and is shared. */
#define CDC_SOFT_RST ((100u << 8) | 1u)
void codec11_bus_init(void);
uint8_t cdc_read(uint16_t reg);
void cdc_write(uint16_t reg, uint8_t val);
void cdc_mask(uint16_t reg, uint8_t set, uint8_t mask);
uint32_t cdc_read_word(uint16_t reg);
void cdc_read_buf(uint16_t reg, uint8_t *buf, uint32_t size);
uint32_t codec11_spi_timeouts(void);

/* ---- Audio11.c ---- */
void audio11_init(AudioCtrl *ct);
/* Handle one audio command; returns 1 if it was one of ours. */
int audio11_command(AudioCtrl *ct, uint32_t cmd, uint32_t arg0);

/* Play the three-beep crash tone (pre-rendered at boot). */
void audio11_error_play(void);

/* ---- Touch11.c ---- */
void touch11_init(void);
void touch11_poll(void);

/* ---- WiFi11.c ---- */
void wifi11_probe(void);
void wifi11_boot(void);

/* ---- Gpu11.c ---- */
void gpu11_run(void);

#endif /* AURORA_CORE11_H */
