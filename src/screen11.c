/* Runs on the firm's ARM11 before it waits for the OS (src/arm11_start.s).
 * Everything in Aurora draws to VRAM_TOP_PHYS and VRAM_BOT_PHYS in BGR8, but
 * the panels show whatever the loader last set up: Luma3DS's chainloader menu
 * uses those same addresses, GodMode9 and fastboot3DS use their own, and a
 * loader that does no screen init leaves the display controllers off (with
 * fastboot3DS, also the LCDs). So the panels are pointed at Aurora's
 * framebuffers here, and set up from scratch when they were off.
 *
 * The display registers are ARM11-only. The register values are Luma3DS's
 * (arm11/source/main.c: initScreens, setupFramebuffers); the GPU reset before
 * a cold setup is fastboot3DS's (GFX_init). Powering the LCDs goes over I2C,
 * which the ARM9 does (main.c) once SCREEN11_STATE says so. No globals: the
 * firm's ARM11 has no .bss clear. */

#include "screen11.h"

typedef volatile uint32_t vu32;
#define REG(a) (*(vu32 *)(a))

#define PDN_GPU_CNT  0x10141200u /* low byte 1: GPU blocks off (Luma's test) */
#define LCD_CNT      0x1020200Cu
#define LCD_ENABLE   0x10202014u
#define LCD_TOP      0x10202200u
#define LCD_BOT      0x10202A00u
#define LCD_FILL     0x04u
#define LCD_LIGHT    0x40u
#define LCD_PWM      0x44u
#define FILL_BLACK   (1u << 24)

#define GPU_CLK      0x10400004u
#define PSC_UNK      0x10400030u
#define PSC0_CNT     0x1040001Cu
#define PSC1_CNT     0x1040002Cu
#define PPF_CNT      0x10400C18u
#define GPU_CLK_ALL  0x70100u /* libn3ds sets it before the 3D engine runs */

#define PDC_TOP      0x10400400u
#define PDC_BOT      0x10400500u

#define FB_TOP       0x18300000u /* VRAM_TOP_PHYS */
#define FB_BOT       0x18346500u /* VRAM_BOT_PHYS */
#define FB_END       (FB_BOT + 320u * 240u * 3u)

#define BRIGHTNESS   0x39u /* until the OS sets its own */

/* PDC registers 0x00-0x48, then 0x5C-0x64: the panel timings. */
static const uint32_t pdc_timing[2][22] = {
    {0x1C2, 0xD1, 0x1C1, 0x1C1, 0x00, 0xCF, 0xD1, 0x01C501C1, 0x10000, 0x19D,
     0x02, 0x192, 0x192, 0x192, 0x01, 0x02, 0x01960192, 0x00, 0x00,
     0x00F00190, 0x01C100D1, 0x01920002},
    {0x1C2, 0xD1, 0x1C1, 0x1C1, 0xCD, 0xCF, 0xD1, 0x01C501C1, 0x10000, 0x19D,
     0x52, 0x192, 0x192, 0x4F, 0x50, 0x52, 0x01980194, 0x00, 0x11,
     0x00F00140, 0x01C100D1, 0x01920052},
};

static void spin(uint32_t n) {
  while (n--)
    __asm__ volatile("nop");
}

/* Both address slots and both eyes on one framebuffer, so neither the slot
 * select nor the 3D state can show anything else. */
static void point(uint32_t pdc, uint32_t fb, uint32_t format) {
  REG(pdc + 0x68) = fb;
  REG(pdc + 0x6C) = fb;
  REG(pdc + 0x94) = fb;
  REG(pdc + 0x98) = fb;
  REG(pdc + 0x70) = format;
  REG(pdc + 0x78) = 0;
  REG(pdc + 0x90) = 240u * 3u; /* stride: one BGR8 column */
}

static void pdc_start(int i) {
  uint32_t pdc = i ? PDC_BOT : PDC_TOP;
  const uint32_t *t = pdc_timing[i];
  for (uint32_t r = 0; r < 19; r++)
    REG(pdc + 4u * r) = t[r];
  for (uint32_t r = 0; r < 3; r++)
    REG(pdc + 0x5C + 4u * r) = t[19 + r];
  point(pdc, i ? FB_BOT : FB_TOP, i ? 0x80301u : 0x80341u);
  REG(pdc + 0x74) = 0x10501; /* start */
  REG(pdc + 0x9C) = 0;
  REG(pdc + 0x80) = 0; /* identity colour table */
  for (uint32_t v = 0; v < 256; v++)
    REG(pdc + 0x84) = 0x10101u * v;
}

static void cold_setup(void) {
  REG(PDN_GPU_CNT) = 0x10000; /* reset */
  spin(1000);
  REG(PDN_GPU_CNT) = 0x1007F;
  REG(GPU_CLK) = GPU_CLK_ALL;
  REG(PSC_UNK) = 0;
  REG(PSC0_CNT) = 0;
  REG(PSC1_CNT) = 0;
  REG(PPF_CNT) = 0;

  REG(LCD_TOP + LCD_FILL) = FILL_BLACK;
  REG(LCD_BOT + LCD_FILL) = FILL_BLACK;
  REG(LCD_ENABLE) = 1;
  REG(LCD_CNT) &= 0xFFFEFFFEu;
  REG(LCD_TOP + LCD_LIGHT) = BRIGHTNESS;
  REG(LCD_BOT + LCD_LIGHT) = BRIGHTNESS;
  REG(LCD_TOP + LCD_PWM) = 0x1023E;
  REG(LCD_BOT + LCD_PWM) = 0x1023E;

  pdc_start(0);
  pdc_start(1);
}

void screen11_setup(void) {
  int was_off = (REG(PDN_GPU_CNT) & 0xFFu) == 1u;

  if (was_off) {
    cold_setup();
  } else {
    REG(LCD_TOP + LCD_FILL) = FILL_BLACK;
    REG(LCD_BOT + LCD_FILL) = FILL_BLACK;
    if (REG(GPU_CLK) == 0x100u) /* fastboot3DS's, for the display alone */
      REG(GPU_CLK) = GPU_CLK_ALL;
    point(PDC_TOP, FB_TOP, 0x80341u);
    point(PDC_BOT, FB_BOT, 0x80301u);
  }

  /* Another loader's leftovers would show until the ARM9's first frame. */
  for (vu32 *p = (vu32 *)FB_TOP; p < (vu32 *)FB_END; p++)
    *p = 0;
  REG(LCD_TOP + LCD_FILL) = 0;
  REG(LCD_BOT + LCD_FILL) = 0;

  SCREEN11_STATE = was_off ? SCREEN11_POWER : SCREEN11_READY;
}
