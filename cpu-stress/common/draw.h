/* Drawing into a 3DS framebuffer as both systems lay it out (stress.h,
 * plat_fb). Colours are 0xRRGGBB. Everything clips to the screen. */
#ifndef STRESS_DRAW_H
#define STRESS_DRAW_H

#include <stdint.h>

#define SCR_H 240

typedef struct {
  uint8_t *p;
  int w; /* 400 or 320 */
} Fb;

void d_rect(Fb *f, int x, int y, int w, int h, uint32_t c);
void d_clear(Fb *f, uint32_t c);
void d_line(Fb *f, int x0, int y0, int x1, int y1, uint32_t c);
/* The 8x8 font, clear background; returns the width drawn. */
int d_text(Fb *f, int x, int y, uint32_t c, const char *s);
int d_textf(Fb *f, int x, int y, uint32_t c, const char *fmt, ...)
    __attribute__((format(printf, 5, 6)));

#endif
