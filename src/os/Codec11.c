/*
 * CTR codec bus: NSPI transport and register access.
 *
 * The codec chip carries the analog audio path and the touchscreen ADC, so
 * this layer is shared by Audio11.c and Touch11.c rather than owned by either.
 * Ported from profi200's libn3ds (source/arm11/drivers/{codec,spi}.c).
 */
#include "core11.h"

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
void cdc_write(uint16_t reg, uint8_t val) {
  cdc_switch_page(reg);
  uint8_t buf[4] __attribute__((aligned(4)));
  buf[0] = (uint8_t)((reg << 1) & 0xFF); /* write: bit0 = 0 */
  buf[1] = val;
  nspi_sendrecv(CODEC_DEV, buf, 0, 2, 0);
}
uint8_t cdc_read(uint16_t reg) {
  cdc_switch_page(reg);
  uint8_t in[4] __attribute__((aligned(4)));
  uint8_t out[4] __attribute__((aligned(4)));
  in[0] = (uint8_t)(((reg << 1) & 0xFF) | 1u); /* read: bit0 = 1 */
  nspi_sendrecv(CODEC_DEV, in, out, 1, 1);
  return out[0];
}
void cdc_mask(uint16_t reg, uint8_t val, uint8_t mask) {
  uint8_t d = cdc_read(reg);
  d = (uint8_t)((d & ~mask) | (val & mask));
  cdc_write(reg, d);
}

/* Diagnostic: read `reg` receiving 4 bytes and return the whole FIFO word, so a
 * byte-lane mismatch (data not in bits 0-7) is visible. */
uint32_t cdc_read_word(uint16_t reg) {
  cdc_switch_page(reg);
  uint8_t in[4] __attribute__((aligned(4)));
  uint32_t out = 0;
  in[0] = (uint8_t)(((reg << 1) & 0xFF) | 1u);
  nspi_sendrecv(CODEC_DEV, in, (uint8_t *)&out, 1, 4);
  return out;
}

/* Burst-read `size` bytes starting at `reg` (the codec auto-increments the
 * register). `buf` must be 4-byte aligned. */
void cdc_read_buf(uint16_t reg, uint8_t *buf, uint32_t size) {
  cdc_switch_page(reg);
  uint8_t in[4] __attribute__((aligned(4)));
  in[0] = (uint8_t)(((reg << 1) & 0xFF) | 1u);
  nspi_sendrecv(CODEC_DEV, in, buf, 1, size);
}


uint32_t codec11_spi_timeouts(void) { return g_spi_timeouts; }

/* Clock/interface domain: enable the new SPI interface, the codec MCLK and the
 * codec SPI bus, then reset the chip. Must run before any cdc_* access. */
void codec11_bus_init(void) {
  MMIO16(CFG11_SPI_CNT) = 0x7u;
  MMIO8(PDN_I2S_CNT) = 0x02u; /* PDN_I2S_CNT_I2S_CLK2_EN */
  dsb();
  MMIO32(NSPI_INT_MASK) = 0x1u;
  MMIO32(NSPI_INT_STAT) = 0x7u;
  dsb();

  cdc_page = 0xFF;
  cdc_write(CDC_SOFT_RST, 1);
  sleep_ms(40);
  cdc_switch_page(0);
}
