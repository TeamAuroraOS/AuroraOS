#include "anim.h"
#include "gpu.h"
#include "timer.h"

extern void os_dcache_flush(void);

/* The frames animations work from: what each screen showed before, what it
 * shows next, and what goes behind a pop-up card. Clear of the screenshot
 * buffers (to 0x26B1CA36) and the ARM11 mailbox at 0x27000000. */
#define FR_TOP_OLD    0x26C00000u /* 288,000 B -> 0x26C46500 */
#define FR_TOP_NEW    0x26C50000u /*           -> 0x26C96500 */
#define FR_BOT_OLD    0x26CA0000u /* 230,400 B -> 0x26CD8400 */
#define FR_BOT_NEW    0x26CE0000u /*           -> 0x26D18400 */
#define FR_BOT_BEHIND 0x26D20000u /*           -> 0x26D58400 */

/* Both screens are 240 pixels tall, so a framebuffer column is the same size
 * on each, and a multiple of the 16 bytes a GPU copy moves. */
#define SH        TOP_SCREEN_HEIGHT
#define COL_BYTES ((u32)SH * BYTES_PER_PIXEL)

#define SLIDE_MS    360
#define FADE_MS     220
#define POPUP_MS    300
#define XFADE_MS    200
#define XFADE_SHIFT 28  /* how far content travels in a cross-fade, pixels */
#define PARALLAX    77  /* the screen underneath moves 77/256 as far */
#define SHADOW_W    20  /* the moving screen's shadow, columns */
#define SHADOW_A    120 /* its darkness at the edge, /256 */
#define FRAME_MS    16
#define MAX_FRAMES  240 /* a stalled timer still ends a transition */

typedef struct {
  int w;
  u32 size;
  u32 old, neu;
} Panel;

static const Panel panels[2] = {
    {TOP_SCREEN_WIDTH, TOP_FB_SIZE, FR_TOP_OLD, FR_TOP_NEW},
    {BOT_SCREEN_WIDTH, BOT_FB_SIZE, FR_BOT_OLD, FR_BOT_NEW},
};

static volatile u8 *back_of(int i) { return i ? VRAM_BOT_A : VRAM_TOP_LA; }

static void present_of(int i) {
  if (i)
    screen_present_bottom();
  else
    screen_present_top();
}

static int on, gpu_on;
static u32 ticks_ms, ticks_frame;

static int tr_kind, tr_want, tr_ready;

static struct {
  int x, y, w, h, behind;
} pop;

/* How the last transition went, for anim_status(). */
static struct {
  int kind, frames, ms, cpu_copies;
  u32 why; /* gpu_last_fail() for the last CPU copy */
} last;

static int ms_since(u32 t0) { return (int)((timer_ticks() - t0) / ticks_ms); }

/* 0..256 over `ms`. */
static int progress(u32 t0, int ms) {
  int t = ms_since(t0);
  return t >= ms ? 256 : t * 256 / ms;
}

static int ease_out(int p) {
  int q = 256 - p;
  return 256 - (((q * q) >> 8) * q >> 8);
}

/* Overshoots by about 6% before it settles, for a card that pops. */
static int back_out(int p) {
  int u = p - 256, u2 = (u * u) >> 8, u3 = (u2 * u) >> 8;
  return 256 + u3 * 23 / 10 + u2 * 13 / 10;
}

/* Holds the loop to one frame per FRAME_MS, unless the panels' presents wait
 * for the vertical blank, which paces the loop to the display already. */
static void pace(u32 *frame, int ready) {
  if ((gpu_vsynced() & (u32)ready) == (u32)ready)
    return;
  while (timer_ticks() - *frame < ticks_frame)
    ;
  *frame = timer_ticks();
}

/* The pair is waited for, since g_blit_src can guard only one backbuffer. */
void anim_present(int ready) {
  if (ready == ANIM_BOTH && !tr_kind) {
    gpu_wait_idle();
    g_blit_src = 0;
    if (gpu_present2_async((u32)back_of(0), (u32)back_of(1))) {
      gpu_wait_idle();
      return;
    }
  }
  for (int i = 0; i < 2; i++)
    if (ready & (1 << i))
      present_of(i);
}

/* A GPU copy, or the CPU's when the GPU refuses one. */
static void copy(u32 dst, u32 src, u32 len) {
  u32 *d = (u32 *)dst;
  const u32 *s = (const u32 *)src;
  if (!len || gpu_texcopy(src, dst, len))
    return;
  last.cpu_copies++;
  last.why = gpu_last_fail();
  os_dcache_flush(); /* the GPU may have written either side since */
  for (u32 i = 0; i < len >> 2; i++)
    d[i] = s[i];
}

/* dst = a + (b - a) * t / 256 over n bytes. The three share an alignment, so
 * the middle goes a word at a time, two channels per multiply. */
static void blend_run(u8 *dst, const u8 *a, const u8 *b, u32 n, u32 t) {
  u32 it = 256u - t;
  while (n && ((u32)dst & 3u)) {
    *dst++ = (u8)((*a++ * it + *b++ * t) >> 8);
    n--;
  }
  u32 *d = (u32 *)dst;
  const u32 *pa = (const u32 *)a, *pb = (const u32 *)b;
  for (u32 i = n >> 2; i; i--) {
    u32 x = *pa++, y = *pb++;
    u32 lo = ((x & 0x00FF00FFu) * it + (y & 0x00FF00FFu) * t) >> 8;
    u32 hi = ((x >> 8) & 0x00FF00FFu) * it + ((y >> 8) & 0x00FF00FFu) * t;
    *d++ = (lo & 0x00FF00FFu) | (hi & 0xFF00FF00u);
  }
  dst = (u8 *)d;
  a = (const u8 *)pa;
  b = (const u8 *)pb;
  for (n &= 3u; n; n--)
    *dst++ = (u8)((*a++ * it + *b++ * t) >> 8);
}

/* Darkens one framebuffer column to keep/256. */
static void darken_column(u8 *col, u32 keep) {
  u32 *w = (u32 *)col;
  for (u32 i = 0; i < COL_BYTES / 4u; i++) {
    u32 x = w[i];
    w[i] = ((((x & 0x00FF00FFu) * keep) >> 8) & 0x00FF00FFu) |
           ((((x >> 8) & 0x00FF00FFu) * keep) & 0xFF00FF00u);
  }
}

/* The moving screen's shadow on the screen under it, darkest at `edge`. */
static void shadow(u8 *back, int w, int edge) {
  for (int d = 1; d <= SHADOW_W && edge - d >= 0; d++) {
    int k = SHADOW_W + 1 - d;
    if (edge - d < w)
      darken_column(back + (u32)(edge - d) * COL_BYTES,
                    256u - (u32)(SHADOW_A * k * k / (SHADOW_W * SHADOW_W)));
  }
}

/* One slide frame into the backbuffer, as two contiguous runs: the framebuffer
 * runs down screen columns, so a band of columns is one block of memory. The
 * screen on top moves the whole width; the one under it moves PARALLAX of
 * that. Returns where the top screen's left edge is. */
static int slide_compose(int i, int kind, int e) {
  const Panel *pn = &panels[i];
  u32 back = (u32)back_of(i);
  int w = pn->w, move = (e * w + 128) >> 8, off, edge;

  if (kind == ANIM_PUSH) {
    off = move * PARALLAX >> 8;
    edge = w - move;
    copy(back, pn->old + (u32)off * COL_BYTES, (u32)edge * COL_BYTES);
    copy(back + (u32)edge * COL_BYTES, pn->neu, (u32)move * COL_BYTES);
  } else {
    off = (w - move) * PARALLAX >> 8;
    edge = move;
    copy(back, pn->neu + (u32)off * COL_BYTES, (u32)edge * COL_BYTES);
    copy(back + (u32)edge * COL_BYTES, pn->old, (u32)(w - edge) * COL_BYTES);
  }
  return edge;
}

static void run_slide(int kind, int ready) {
  u32 t0 = timer_ticks(), frame = t0;
  int edge[2] = {0, 0};
  for (;;) {
    int p = progress(t0, SLIDE_MS), e = ease_out(p);
    for (int i = 0; i < 2; i++)
      if (ready & (1 << i))
        edge[i] = slide_compose(i, kind, e);
    os_dcache_flush(); /* the shadow reads what the GPU just wrote */
    for (int i = 0; i < 2; i++)
      if ((ready & (1 << i)) && edge[i] > 0 && edge[i] < panels[i].w)
        shadow((u8 *)back_of(i), panels[i].w, edge[i]);
    anim_present(ready);
    last.frames++;
    if (p >= 256 || last.frames >= MAX_FRAMES)
      break;
    pace(&frame, ready);
  }
}

static void run_fade(int ready) {
  u32 t0 = timer_ticks(), frame = t0;
  os_dcache_flush(); /* the CPU reads frames the GPU just wrote */
  for (;;) {
    int p = progress(t0, FADE_MS), e = ease_out(p);
    gpu_wait_idle(); /* the last frame's present has read the backbuffer */
    g_blit_src = 0;
    for (int i = 0; i < 2; i++) {
      const Panel *pn = &panels[i];
      if (!(ready & (1 << i)))
        continue;
      blend_run((u8 *)back_of(i), (const u8 *)pn->old, (const u8 *)pn->neu,
                pn->size, (u32)e);
    }
    anim_present(ready);
    last.frames++;
    if (p >= 256 || last.frames >= MAX_FRAMES)
      break;
    pace(&frame, ready);
  }
}

/* The card at scale s/256 about its centre and opacity a/256, sampled from the
 * new frame, over what the backbuffer already holds. */
static void popup_card(u8 *back, const u8 *neu, int s, int a) {
  static int rowmap[SH];
  int x0 = pop.x, y0 = pop.y, w = pop.w, h = pop.h;
  int cx2 = 2 * x0 + w, cy2 = 2 * y0 + h; /* the centre, doubled */
  int inv = (256 << 8) / s;
  int ow = (w * s) >> 8, oh = (h * s) >> 8;
  int ox0 = (cx2 - ow) / 2, oy0 = (cy2 - oh) / 2;

  for (int oy = 0; oy < SH; oy++) {
    int sy = (cy2 + (((2 * oy + 1 - cy2) * inv) >> 8)) >> 1;
    rowmap[oy] =
        (oy >= oy0 && oy < oy0 + oh && sy >= y0 && sy < y0 + h) ? sy : -1;
  }
  for (int ox = ox0; ox < ox0 + ow; ox++) {
    int sx = (cx2 + (((2 * ox + 1 - cx2) * inv) >> 8)) >> 1;
    u8 *dc;
    const u8 *sc;
    if (ox < 0 || ox >= BOT_SCREEN_WIDTH || sx < x0 || sx >= x0 + w)
      continue;
    dc = back + (u32)ox * COL_BYTES;
    sc = neu + (u32)sx * COL_BYTES;
    for (int oy = oy0 < 0 ? 0 : oy0; oy < oy0 + oh && oy < SH; oy++) {
      int sy = rowmap[oy];
      u8 *d;
      const u8 *src;
      if (sy < 0)
        continue;
      d = dc + (u32)(SH - 1 - oy) * 3u;
      src = sc + (u32)(SH - 1 - sy) * 3u;
      d[0] = (u8)((src[0] * a + d[0] * (256 - a)) >> 8);
      d[1] = (u8)((src[1] * a + d[1] * (256 - a)) >> 8);
      d[2] = (u8)((src[2] * a + d[2] * (256 - a)) >> 8);
    }
  }
}

/* What is behind the card fades in from the old frame while the card grows
 * from 80% with a small overshoot, fading in over the first third. */
static void run_popup(void) {
  const Panel *pn = &panels[1];
  u8 *back = (u8 *)back_of(1);
  u32 t0 = timer_ticks(), frame = t0;
  os_dcache_flush();
  for (;;) {
    int p = progress(t0, POPUP_MS), e = ease_out(p);
    int s = 205 + 51 * back_out(p) / 256, a = p * 3 > 256 ? 256 : p * 3;
    gpu_wait_idle();
    g_blit_src = 0;
    blend_run(back, (const u8 *)pn->old, (const u8 *)FR_BOT_BEHIND, pn->size,
              (u32)e);
    popup_card(back, (const u8 *)pn->neu, s, a);
    screen_present_bottom();
    last.frames++;
    if (p >= 256 || last.frames >= MAX_FRAMES)
      break;
    pace(&frame, ANIM_BOT);
  }
  copy((u32)back, pn->neu, pn->size); /* exact, whatever rounding did */
  screen_present_bottom();
}

static void run(void) {
  int kind = tr_kind, ready = tr_ready;
  u32 t0 = timer_ticks();
  tr_kind = 0; /* presents from here on go straight to the panels */
  if (!ready)
    return;
  for (int i = 0; i < 2; i++)
    if (ready & (1 << i))
      copy(panels[i].neu, (u32)back_of(i), panels[i].size);
  if (kind == ANIM_POPUP && (!(ready & ANIM_BOT) || !pop.behind))
    kind = ANIM_FADE;
  last.kind = kind;
  last.frames = 0;
  if (kind == ANIM_FADE)
    run_fade(ready);
  else if (kind == ANIM_POPUP)
    run_popup();
  else
    run_slide(kind, ready);
  last.ms = ms_since(t0);
}

/* A screen outside the transition presents as usual. */
static int intercept(volatile u8 *back) {
  int bit = (back == VRAM_TOP_LA) ? ANIM_TOP : ANIM_BOT;
  if (!tr_kind || !(tr_want & bit))
    return 0;
  tr_ready |= bit;
  if ((tr_ready & tr_want) == tr_want)
    run();
  return 1;
}

/* Cross-fades in place, one per screen. */
typedef struct {
  short x, y, w, h;
} Rect;

static struct {
  int state; /* 0 idle, 1 collecting areas, 2 running */
  int n, dir;
  Rect r[4];
  u32 t0;
} xf[2];

void anim_init(void) {
  u32 hz = timer_hz(), t0, i;
  if (hz < 1000u)
    return;
  t0 = timer_ticks();
  for (i = 0; i < 1000000u && timer_ticks() == t0; i++)
    ;
  if (timer_ticks() == t0)
    return; /* the timer is not running */
  ticks_ms = hz / 1000u;
  ticks_frame = ticks_ms * FRAME_MS;
  on = 1;
  if (g_screen_blit) {
    gpu_on = 1;
    g_screen_intercept = intercept;
  }
}

void anim_gpu_start(void) {
  if (!on || gpu_on || !g_screen_blit)
    return;
  gpu_on = 1;
  g_screen_intercept = intercept;
  anim_vsync_setup(0);
}

void anim_transition(int kind, int screens) {
  if (!gpu_on)
    return;
  xf[0].state = xf[1].state = 0; /* the frames are about to be reused */
  if (!tr_kind) {
    last.cpu_copies = 0;
    /* The panels themselves, which is what the user sees. */
    for (int i = 0; i < 2; i++)
      copy(panels[i].old, gpu_front(i), panels[i].size);
    tr_ready = 0;
  }
  tr_kind = kind;
  tr_want = screens;
}

void anim_popup(int x, int y, int w, int h) {
  if (!gpu_on)
    return;
  anim_transition(ANIM_POPUP, ANIM_BOT);
  pop.x = x;
  pop.y = y;
  pop.w = w;
  pop.h = h;
  pop.behind = 0;
}

void anim_popup_behind(void) {
  if (tr_kind != ANIM_POPUP || pop.behind)
    return;
  copy(FR_BOT_BEHIND, (u32)back_of(1), panels[1].size);
  pop.behind = 1;
}

void anim_flush(void) {
  if (tr_kind)
    run();
}

/* Content moves `dir * XFADE_SHIFT` pixels across the fade: the old leaves
 * one way as the new arrives from the other. */
static void xf_blend(int s, int e) {
  const Panel *pn = &panels[s];
  u8 *back = (u8 *)back_of(s);
  int shift_old = xf[s].dir * XFADE_SHIFT * e / 256;
  int shift_new = xf[s].dir * XFADE_SHIFT * (256 - e) / 256;
  for (int k = 0; k < xf[s].n; k++) {
    const Rect *r = &xf[s].r[k];
    u32 rows = ((u32)(SH - r->y - r->h)) * 3u;
    for (int x = r->x; x < r->x + r->w; x++) {
      int xo = x + shift_old, xn = x - shift_new;
      xo = xo < 0 ? 0 : (xo >= pn->w ? pn->w - 1 : xo);
      xn = xn < 0 ? 0 : (xn >= pn->w ? pn->w - 1 : xn);
      blend_run(back + (u32)x * COL_BYTES + rows,
                (const u8 *)pn->old + (u32)xo * COL_BYTES + rows,
                (const u8 *)pn->neu + (u32)xn * COL_BYTES + rows,
                (u32)r->h * 3u, (u32)e);
    }
  }
}

void anim_xfade_begin(int screen) {
  int s = screen == ANIM_BOT;
  if (!gpu_on || tr_kind) {
    xf[s].state = 0;
    return;
  }
  /* Part way through, the backbuffer holds the target: put back the blend the
   * panel shows before keeping it. */
  if (xf[s].state == 2) {
    gpu_wait_idle();
    g_blit_src = 0;
    xf_blend(s, ease_out(progress(xf[s].t0, XFADE_MS)));
  }
  copy(panels[s].old, (u32)back_of(s), panels[s].size);
  xf[s].state = 1;
  xf[s].n = 0;
}

void anim_xfade_area(int screen, int x, int y, int w, int h) {
  int s = screen == ANIM_BOT;
  Rect *r;
  if (xf[s].state != 1 || xf[s].n >= 4)
    return;
  if (x < 0) {
    w += x;
    x = 0;
  }
  if (y < 0) {
    h += y;
    y = 0;
  }
  if (x + w > panels[s].w)
    w = panels[s].w - x;
  if (y + h > SH)
    h = SH - y;
  if (w <= 0 || h <= 0)
    return;
  r = &xf[s].r[xf[s].n++];
  r->x = (short)x;
  r->y = (short)y;
  r->w = (short)w;
  r->h = (short)h;
}

void anim_xfade_start(int screen, int dir) {
  int s = screen == ANIM_BOT;
  if (xf[s].state != 1)
    return;
  copy(panels[s].neu, (u32)back_of(s), panels[s].size);
  os_dcache_flush(); /* drop lines of either frame from an earlier fade */
  xf[s].dir = dir;
  xf[s].t0 = timer_ticks();
  xf[s].state = 2;
  xf_blend(s, 0); /* the caller presents this: still the old state */
}

int anim_xfade_frame(int screen) {
  int s = screen == ANIM_BOT, p;
  if (xf[s].state != 2)
    return 0;
  p = progress(xf[s].t0, XFADE_MS);
  gpu_wait_idle();
  g_blit_src = 0;
  xf_blend(s, ease_out(p));
  if (p >= 256)
    xf[s].state = 0;
  return 1;
}

void anim_jump(AnimVal *v, int px) {
  v->cur = v->to = px << 8;
  v->vel = 0;
}

void anim_to(AnimVal *v, int px) {
  v->to = px << 8;
  if (!on) {
    v->cur = v->to;
    v->vel = 0;
  }
}

/* A damped spring stepped in 2 ms slices (semi-implicit Euler, stable at these
 * stiffnesses). Plain: critically damped, omega 20/s. Bouncy: omega 22/s at a
 * damping ratio of 0.7, which overshoots by about 5%. Velocity is in 1/256
 * pixels per second. */
int anim_step(AnimVal *v, int ms) {
  int was = anim_px(v), k = v->bounce ? 484 : 400, c = v->bounce ? 31 : 40;
  if (!anim_moving(v))
    return 0;
  for (int n = (ms + 1) / 2; n > 0; n--) {
    int acc = -k * (v->cur - v->to) - c * v->vel;
    v->vel += acc / 500;
    v->cur += v->vel / 500;
  }
  if (v->cur - v->to > -32 && v->cur - v->to < 32 && v->vel > -2560 &&
      v->vel < 2560) {
    v->cur = v->to;
    v->vel = 0;
  }
  return anim_px(v) != was;
}

/* With presents waiting for the blank, a shorter gate leaves the display to
 * set the pace. */
int anim_frame(u32 *last_frame) {
  u32 now, d;
  if (!on)
    return FRAME_MS;
  now = timer_ticks();
  d = now - *last_frame;
  if (d < (gpu_vsynced() == 3u ? ticks_ms * 12u : ticks_frame))
    return 0;
  *last_frame = now;
  d /= ticks_ms;
  return d > FRAME_MS + 4 ? FRAME_MS + 4 : (int)d;
}

u32 anim_now(void) { return on ? timer_ticks() : 0; }

int anim_ms(u32 t0) { return on ? ms_since(t0) : 1 << 20; }

int anim_list_top(const AnimList *l, int sel) {
  int top = sel - l->visible / 2;
  if (top > l->count - l->visible)
    top = l->count - l->visible;
  return top < 0 ? 0 : top;
}

void anim_list_jump(AnimList *l, int sel) {
  anim_jump(&l->ring, sel * l->step);
  anim_jump(&l->scroll, anim_list_top(l, sel) * l->step);
}

void anim_list_to(AnimList *l, int sel) {
  l->ring.bounce = 1;
  anim_to(&l->ring, sel * l->step);
  anim_to(&l->scroll, anim_list_top(l, sel) * l->step);
}

int anim_list_step(AnimList *l, int ms) {
  int moved = anim_step(&l->ring, ms);
  return anim_step(&l->scroll, ms) | moved;
}

static char *put(char *p, char *end, const char *s) {
  while (*s && p < end)
    *p++ = *s++;
  return p;
}

static char *put_num(char *p, char *end, u32 v) {
  char tmp[12];
  int n = 0;
  do
    tmp[n++] = (char)('0' + v % 10u);
  while ((v /= 10u) && n < 11);
  while (n && p < end)
    *p++ = tmp[--n];
  return p;
}

/* What each vsync method measured for each panel, in ms for VS_BLANKS blanks;
 * VS_NONE when it never answered, 0 when it was not tried. */
#define VS_BLANKS 4
#define VS_NONE   0xFFFFu
static u16 vs_ms[2][3];

/* Tried in this order; the distributor is the one known to work. */
static const u32 vs_order[2] = {GPU_VSYNC_GIC, GPU_VSYNC_IRQ};
static const char *const vs_names[3] = {"off", "irq", "gic"};

void anim_vsync_setup(int again) {
  u32 kept = gpu_vsynced();
  if (!gpu_on)
    return;
  for (int i = 0; i < 2; i++) {
    u32 pick = GPU_VSYNC_OFF;
    if (!again && (kept & (1u << i)))
      continue;
    for (int k = 0; k < 2; k++) {
      u32 m = vs_order[k], t0 = timer_ticks();
      int ok = gpu_vsync_test(i, m, VS_BLANKS);
      int ms = ms_since(t0);
      vs_ms[i][m] = ok ? (u16)ms : VS_NONE;
      /* Real blanks come every 16.7 ms, the first after up to one period; a
       * flag that is always set finishes far sooner. */
      if (ok && ms >= (VS_BLANKS - 1) * 13 && ms <= VS_BLANKS * 40) {
        pick = m;
        break;
      }
    }
    gpu_vsync_set(i, pick);
  }
}

static char *put_vsync(char *p, char *end, int i, u32 mode) {
  p = put(p, end, i ? "  Bottom " : "Vsync: top ");
  p = put(p, end, vs_names[mode < 3u ? mode : 0u]);
  if (mode && mode < 3u && !vs_ms[i][mode])
    return put(p, end, " (kept)"); /* chosen by an earlier boot */
  p = put(p, end, " (");
  for (int k = 0, n = 0; k < 2; k++) {
    u32 m = vs_order[k];
    if (!vs_ms[i][m])
      continue;
    p = put(p, end, n++ ? " " : "");
    p = put(p, end, vs_names[m]);
    p = put(p, end, " ");
    if (vs_ms[i][m] == VS_NONE)
      p = put(p, end, "-");
    else
      p = put_num(p, end, vs_ms[i][m]);
  }
  return put(p, end, ")");
}

static char *put_presents(char *p, char *end, int i, u32 n, u32 missed) {
  p = put(p, end, i ? "  bottom " : "Presents: top ");
  p = put_num(p, end, n);
  p = put(p, end, " (");
  p = put_num(p, end, missed);
  return put(p, end, " missed)");
}

void anim_status(char *out, int size) {
  static const char *const kinds[] = {"none yet", "slide", "slide back",
                                      "fade", "pop-up"};
  char *p = out, *end = out + size - 1;
  if (!on) {
    p = put(p, end, "Animations off: the ARM9 timer is not running");
  } else {
    u32 vs, swap[2], cnt[2], pres[2], missed[2];
    static const char *const whys[] = {"ok", "not ready", "bad request",
                                       "timeout", "busy"};
    gpu_vsync_info(&vs, swap, cnt, pres, missed);
    p = put(p, end, gpu_on ? "Animations on" : "Movement only, no GPU frames");
    p = put(p, end, ", timer ");
    p = put_num(p, end, timer_hz());
    p = put(p, end, " Hz");
    p = put(p, end, "\n");
    p = put_vsync(p, end, 0, vs & 3u);
    p = put_vsync(p, end, 1, (vs >> 2) & 3u);
    p = put(p, end, "\n");
    p = put_presents(p, end, 0, pres[0], missed[0]);
    p = put_presents(p, end, 1, pres[1], missed[1]);
    p = put(p, end, "\nLast: ");
    p = put(p, end, kinds[last.kind]);
    if (last.kind) {
      p = put(p, end, ", ");
      p = put_num(p, end, (u32)last.frames);
      p = put(p, end, " frames in ");
      p = put_num(p, end, (u32)last.ms);
      p = put(p, end, " ms");
    }
    if (last.cpu_copies) {
      p = put(p, end, ", ");
      p = put_num(p, end, (u32)last.cpu_copies);
      p = put(p, end, " CPU copies (GPU: ");
      p = put(p, end, last.why == GPU_FAIL_NO_ANSWER ? "no answer"
                      : last.why < 5u              ? whys[last.why]
                                                   : "?");
      p = put(p, end, ")");
    }
  }
  *p = 0;
}
