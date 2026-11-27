#ifndef AURORA_SCREEN11_H
#define AURORA_SCREEN11_H

#include <stdint.h>

/* The firm's ARM11 sets the panels up as it starts (src/screen11.c) and then
 * reports here. The word is the second one of its image (src/arm11_start.s),
 * which the loader copies before either CPU runs, so it starts as BUSY. */
#define SCREEN11_STATE (*(volatile uint32_t *)0x1FF80004u)

#define SCREEN11_BUSY  0u
#define SCREEN11_READY 1u /* the panels were on: now on Aurora's framebuffers */
#define SCREEN11_POWER 2u /* the panels were off: the ARM9 must power the LCDs */

void screen11_setup(void);

#endif
