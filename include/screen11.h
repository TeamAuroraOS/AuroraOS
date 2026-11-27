#ifndef AURORA_SCREEN11_H
#define AURORA_SCREEN11_H

#include <stdint.h>

/* The firm's ARM11 sets the panels up as it starts (src/screen11.c) and then
 * reports here. The words are the second to fourth of its image
 * (src/arm11_start.s), which the loader copies before either CPU runs, so they
 * all start at 0. */
#define SCREEN11_STATE (*(volatile uint32_t *)0x1FF80004u)

#define SCREEN11_BUSY  0u
#define SCREEN11_READY 1u /* the panels were on: now on Aurora's framebuffers */
#define SCREEN11_POWER 2u /* the panels were off: the ARM9 must power the LCDs */

/* Presents. Each panel has three framebuffers (A is the one the OS expects).
 * The ARM9 (src/fx.c) copies a frame into one the panel is not showing and
 * asks for it in SCREEN11_FLIP; the ARM11 (screen11_serve()) points the panel
 * at it in the next vertical blank and echoes the request's sequence number in
 * SCREEN11_FLIPPED.
 *
 * SCREEN11_FLIP: bits 8-31 a sequence number, bits 0-3 the top panel and
 * bits 4-7 the bottom one, each SCREEN11_SHOW(framebuffer) or 0 to leave it.
 * SCREEN11_FLIPPED: bits 8-31 the last sequence number done, bits 0-7 the
 * blank method in use (SCREEN11_BLANK_*). */
#define SCREEN11_FLIP    (*(volatile uint32_t *)0x1FF80008u)
#define SCREEN11_FLIPPED (*(volatile uint32_t *)0x1FF8000Cu)

#define SCREEN11_SEQ(v)   ((uint32_t)(v) >> 8)
#define SCREEN11_SHOW(fb) (4u | (uint32_t)(fb))

#define SCREEN11_BLANK_GIC  0u /* the display interrupt, pending in the GIC */
#define SCREEN11_BLANK_FLAG 1u /* the vblank flag in the controller */
#define SCREEN11_BLANK_NONE 2u /* neither answered: flips are not synced */

/* Framebuffer 0-2 of each panel: A, B and C of include/gpu.h. */
#define SCREEN11_FB_TOP(i) (0x18300000u + 0x100000u * (uint32_t)(i))
#define SCREEN11_FB_BOT(i) (0x18346500u + 0x100000u * (uint32_t)(i))

void screen11_setup(void);
/* Serves presents until the loader leaves an entry point in the mailbox, and
 * returns it. */
uint32_t screen11_serve(void);

#endif
