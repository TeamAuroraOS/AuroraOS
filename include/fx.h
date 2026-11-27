#ifndef AURORA_FX_H
#define AURORA_FX_H

/* The firm's frames (src/fx.c): caches, a millisecond clock, and tear-free
 * presents through the firm's ARM11. Firm only. */

#include "aurora.h"

#define FX_TOP  1
#define FX_BOT  2
#define FX_BOTH 3

/* Starts the clock, turns the caches on unless `plain`, and points drawing at
 * the FCRAM backbuffers (VRAM_TOP_LA / VRAM_BOT_A). Call once the firm's
 * ARM11 has set the screens up. */
void fx_init(int plain);
int fx_cached(void);

u32 fx_ms(void);

/* Puts the backbuffers of `mask` on the panels at the next vertical blank.
 * Returns once the copy is made; the next present waits for the flip. */
void fx_present(int mask);

/* Before the OS starts: both panels on framebuffer A with the last frame, as
 * the OS expects, and no present pending on the firm's ARM11. Presents after
 * this go straight into A. */
void fx_handoff(void);

/* Whole-screen effects on the backbuffers: a horizontal push between the
 * frame on the panel and the one `draw` makes (dir 1: the new one comes in
 * from the right), and a fade of what is drawn towards black (256 = as is). */
void fx_push(int mask, int dir, void (*draw)(void *), void *ctx, u32 ms);
void fx_dim(int mask, int alpha);
void fx_fade_out(int mask, u32 ms);

/* 0..256 in, 0..256 out. */
int fx_ease_out(int t);
int fx_ease_in_out(int t);
int fx_ease_back(int t); /* overshoots a little before it settles */
int fx_progress(u32 start, u32 now, u32 ms);
int fx_lerp(int a, int b, int t);
Color fx_mix(Color a, Color b, int t);

void fx_dcache_clean(void);

#endif
