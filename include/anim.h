#ifndef AURORA_ANIM_H
#define AURORA_ANIM_H

#include "aurora.h"

/* Time-based, so an animation lasts as long whatever the frame rate.
 * Transitions need the GPU backbuffers; without them a screen just appears,
 * while springs still run. */

#define ANIM_TOP  1
#define ANIM_BOT  2
#define ANIM_BOTH 3

enum {
  ANIM_PUSH = 1, /* the new screen slides in from the right: opening */
  ANIM_POP,      /* the old screen slides away to the right: going back */
  ANIM_FADE,     /* a cross-fade: changes in place, dialogs closing */
  ANIM_POPUP,    /* a card scales up over a dimming screen: see anim_popup */
};

/* After gpu_init() and timer_ready(). */
void anim_init(void);

/* For backbuffers and a present hook set up after anim_init(): turns on
 * transitions and runs anim_vsync_setup(). */
void anim_gpu_start(void);

/* Times a few vertical blanks of each panel by each way the GPU code can wait
 * for one, and has presents use the first that runs at the display's rate.
 * After anim_init(). The choice lasts as long as the core, so a panel that
 * already has one keeps it unless `again` is set. */
void anim_vsync_setup(int again);

/* Call just before drawing the next screen: the panels keep what they show
 * until each screen in `screens` has presented, then animate to the new frames.
 * A screen calls it with ANIM_PUSH as it opens and ANIM_POP as it returns, so
 * the screen it returns to animates when it redraws. Screens not named present
 * as usual. */
void anim_transition(int kind, int screens);

/* A dialog card at (x, y, w, h) on the bottom screen grows into place while
 * what is behind it dims:
 *   anim_popup(x, y, w, h);
 *   ...draw what goes behind the card, such as the scrim...
 *   anim_popup_behind();
 *   ...draw the card, and present...
 * anim_popup_behind() does nothing unless a pop-up is waiting, so a dialog
 * that redraws itself can call it every time. */
void anim_popup(int x, int y, int w, int h);
void anim_popup_behind(void);

/* Runs a transition still waiting on a present; ui_idle() calls it, so a
 * screen that redraws only one panel still animates. */
void anim_flush(void);

/* Cross-fades parts of one screen to a new state without blocking, the new
 * content sliding in from the side `dir` gives (1 from the right, -1 from the
 * left, 0 in place):
 *   anim_xfade_begin(ANIM_TOP);      keep what the screen shows now
 *   ...draw the new state...
 *   anim_xfade_area(ANIM_TOP, x, y, w, h);   up to four areas
 *   anim_xfade_start(ANIM_TOP, dir);
 *   screen_present_top();
 * then, while anim_xfade_frame() returns 1 on a due frame, present again. */
void anim_xfade_begin(int screen);
void anim_xfade_area(int screen, int x, int y, int w, int h);
void anim_xfade_start(int screen, int dir);
int anim_xfade_frame(int screen);

/* A position on a spring: it moves off quickly, settles gently and stays
 * smooth when the target moves again part way. With `bounce` set it overshoots
 * a little before it settles. Kept in 1/256 pixels. */
typedef struct {
  int cur, to, vel;
  int bounce;
} AnimVal;

void anim_jump(AnimVal *v, int px);
void anim_to(AnimVal *v, int px);
/* Advances by `ms`; 1 if the pixel it rounds to changed, so a redraw is due. */
int anim_step(AnimVal *v, int ms);
static inline int anim_px(const AnimVal *v) { return (v->cur + 128) >> 8; }
static inline int anim_moving(const AnimVal *v) {
  return v->cur != v->to || v->vel;
}

/* For a loop that animates: once a frame is due, the milliseconds since the
 * last one (capped, so a stall does not jump), else 0. */
int anim_frame(u32 *last);

/* Presents ANIM_TOP, ANIM_BOT or both; both go in one operation, since each
 * may wait for its panel's vertical blank. */
void anim_present(int screens);

/* Milliseconds since `t0`, a value from anim_now(). */
u32 anim_now(void);
int anim_ms(u32 t0);

/* A list showing `visible` of `count` rows `step` pixels apart, the selection
 * centred where it can be. `ring` is the selected row's offset and `scroll`
 * the first shown row's, both from the top of the list, so a row is drawn at
 * list_y + i * step - anim_px(&scroll). */
typedef struct {
  int count, visible, step;
  AnimVal ring, scroll;
} AnimList;

int anim_list_top(const AnimList *l, int sel);
void anim_list_jump(AnimList *l, int sel);
void anim_list_to(AnimList *l, int sel);
int anim_list_step(AnimList *l, int ms);
static inline int anim_list_moving(const AnimList *l) {
  return anim_moving(&l->ring) || anim_moving(&l->scroll);
}

/* Status lines for the GPU Test screen. */
void anim_status(char *out, int size);

#endif
