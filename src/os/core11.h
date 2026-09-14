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

static inline void dsb(void) {
  __asm__ volatile("mcr p15, 0, %0, c7, c10, 4" ::"r"(0) : "memory");
}
static inline void dcache_clean(void) {
  __asm__ volatile("mcr p15, 0, %0, c7, c10, 0" ::"r"(0) : "memory");
  dsb();
}
/* One cache line, for the command poll; a whole-cache operation per pass is too
 * slow. */
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
/* Coarse and over-sleeps, which is fine for the codec settle delays it is used
 * for. */
static inline void sleep_ms(uint32_t ms) { spin(ms * 300000u); }

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

#define CDC_SOFT_RST ((100u << 8) | 1u)
void codec11_bus_init(void);
uint8_t cdc_read(uint16_t reg);
void cdc_write(uint16_t reg, uint8_t val);
void cdc_mask(uint16_t reg, uint8_t set, uint8_t mask);
uint32_t cdc_read_word(uint16_t reg);
void cdc_read_buf(uint16_t reg, uint8_t *buf, uint32_t size);
uint32_t codec11_spi_timeouts(void);

void audio11_init(AudioCtrl *ct);
/* Handle one audio command; returns 1 if it was one of ours. */
int audio11_command(AudioCtrl *ct, uint32_t cmd, uint32_t arg0);

/* Play the three-beep crash tone (pre-rendered at boot). */
void audio11_error_play(void);

void touch11_init(void);
void touch11_poll(void);

void wifi11_probe(void);
void wifi11_boot(void);

void gpu11_run(void);

/* AUDIO_CMD_N3DS: ask for New 3DS clock mode `mode` and report the outcome in
 * ct->n3ds_before, n3ds_after and n3ds_status. */
void clock11_set(AudioCtrl *ct, uint32_t mode);

#endif
