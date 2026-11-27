/* The firm's frames. Drawing goes to the FCRAM backbuffers, cached once
 * fx_init() turns the protection unit on (until then the firm ran with the
 * caches off, as every loader leaves them, and a full-screen redraw could be
 * watched line by line). A present copies a backbuffer into a framebuffer the
 * panel is not showing, and the firm's ARM11 switches the panel to it in the
 * next vertical blank (screen11_serve()), so nothing half-drawn reaches the
 * screen. Three framebuffers per panel, as the OS's core uses: the one copied
 * into is neither the one shown nor the one shown before it, in case the
 * controller takes a new address a frame late. */

#include "fx.h"
#include "screen11.h"

extern void fx_mpu_on(void);
extern void fx_copy(volatile void *dst, const volatile void *src, u32 bytes);

/* Timer 0 divides the 67.027964 MHz bus clock by 1024, timer 1 counts its
 * overflows: the OS's Timer9.c, which restarts them. */
#define TMR0_VAL (*(volatile u16 *)0x10003000u)
#define TMR0_CNT (*(volatile u16 *)0x10003002u)
#define TMR1_VAL (*(volatile u16 *)0x10003004u)
#define TMR1_CNT (*(volatile u16 *)0x10003006u)
#define TICKS_PER_S 65457u

/* Well past the ARM11 dropping both blank methods (src/screen11.c). */
#define FLIP_TIMEOUT (5u * TICKS_PER_S)

/* Where fx_push keeps the two frames: the OS's Anim.c addresses, unused
 * while the firm runs and clear of everything the loader writes. */
#define SNAP_TOP_OLD ((u8 *)0x26C00000u)
#define SNAP_TOP_NEW ((u8 *)0x26C50000u)
#define SNAP_BOT_OLD ((u8 *)0x26CA0000u)
#define SNAP_BOT_NEW ((u8 *)0x26CE0000u)

#define COL_BYTES (240u * 3u) /* one framebuffer column */

static int timer_on, cached, flip_ok, handed_off;
static u32 seq;
static int shown[2], before[2];

static void timer_start(void) {
  TMR0_CNT = 0;
  TMR1_CNT = 0;
  TMR0_VAL = 0;
  TMR1_VAL = 0;
  TMR1_CNT = 0x84; /* count timer 0's overflows */
  TMR0_CNT = 0x83; /* run, prescaler /1024 */
  timer_on = 1;
}

static u32 ticks(void) {
  u32 hi, lo, hi2;
  if (!timer_on)
    timer_start();
  hi = TMR1_VAL;
  lo = TMR0_VAL;
  hi2 = TMR1_VAL;
  if (hi != hi2) {
    hi = hi2;
    lo = TMR0_VAL;
  }
  return (hi << 16) | lo;
}

u32 fx_ms(void) { return (u32)(((u64)ticks() * 1001u) >> 16); }

/* The firm's waits were nop loops timed with the caches off, where 7000
 * "cycles" made about a millisecond; the caches would make them far shorter,
 * so they are timed instead. */
void delay(volatile u32 cycles) {
  u32 n = (u32)(((u64)cycles * 613u) >> 16) + 1u, t0 = ticks();
  while (ticks() - t0 < n)
    ;
}

int fx_cached(void) { return cached; }

static volatile u8 *back(int i) { return i ? VRAM_BOT_BACK : VRAM_TOP_BACK; }
static u32 fb_size(int i) { return i ? BOT_FB_SIZE : TOP_FB_SIZE; }
static int fb_width(int i) { return i ? BOT_SCREEN_WIDTH : TOP_SCREEN_WIDTH; }

static volatile u8 *fb_addr(int i, int f) {
  return (volatile u8 *)(i ? SCREEN11_FB_BOT(f) : SCREEN11_FB_TOP(f));
}

static int next_fb(int i) {
  if (shown[i] == before[i])
    return (shown[i] + 1) % 3;
  return 3 - shown[i] - before[i];
}

static void drain(void) {
  __asm__ volatile("mcr p15, 0, %0, c7, c10, 4" ::"r"(0) : "memory");
}

/* An ARM11 that stops answering may still switch to the frame it was last
 * given, so from then on every frame goes into all three framebuffers. */
static void flip_wait(void) {
  u32 t0;
  if (!flip_ok)
    return;
  t0 = ticks();
  while (SCREEN11_SEQ(SCREEN11_FLIPPED) != seq)
    if (ticks() - t0 > FLIP_TIMEOUT) {
      flip_ok = 0;
      return;
    }
}

static void request(u32 panels) {
  seq = (seq + 1u) & 0xFFFFFFu;
  drain();
  SCREEN11_FLIP = (seq << 8) | panels;
}

void fx_present(int mask) {
  u32 req = 0;

  flip_wait();
  for (int i = 0; i < 2; i++) {
    if (!(mask & (1 << i)))
      continue;
    if (handed_off) {
      fx_copy(fb_addr(i, 0), back(i), fb_size(i));
      continue;
    }
    if (!flip_ok) {
      for (int f = 0; f < 3; f++)
        fx_copy(fb_addr(i, f), back(i), fb_size(i));
      continue;
    }
    int f = next_fb(i);
    fx_copy(fb_addr(i, f), back(i), fb_size(i));
    req |= SCREEN11_SHOW(f) << (4 * i);
    if (f != shown[i])
      before[i] = shown[i];
    shown[i] = f;
  }
  if (req)
    request(req);
}

void fx_handoff(void) {
  u32 req = 0;

  flip_wait();
  for (int i = 0; flip_ok && i < 2; i++) {
    if (!shown[i])
      continue; /* A holds the last frame already */
    fx_copy(fb_addr(i, 0), back(i), fb_size(i));
    req |= SCREEN11_SHOW(0) << (4 * i);
    before[i] = shown[i];
    shown[i] = 0;
  }
  if (req) {
    request(req);
    flip_wait();
  }
  handed_off = 1;
  if (cached) {
    drain();
    /* Write buffering as Luma3DS's table has it: VRAM not buffered. */
    __asm__ volatile("mcr p15, 0, %0, c3, c0, 0" ::"r"(0x29u) : "memory");
  }
}

/* screen_present_top/bottom() from the shared drawing code (the loader's
 * cards) come here. */
static int fx_blit(u32 src, u32 dst, u32 len) {
  (void)src;
  (void)len;
  if (dst == (u32)VRAM_TOP_PHYS)
    fx_present(FX_TOP);
  else if (dst == (u32)VRAM_BOT_PHYS)
    fx_present(FX_BOT);
  else
    return 0;
  return 1;
}

void fx_init(int plain) {
  if (!timer_on)
    timer_start();
  if (!plain) {
    fx_mpu_on();
    cached = 1;
  }
  flip_ok = SCREEN11_STATE != SCREEN11_BUSY;
  g_fb_top = VRAM_TOP_BACK;
  g_fb_bot = VRAM_BOT_BACK;
  g_screen_blit = fx_blit;
}

/* Two bytes of each of two pixels at once: 0x00FF00FF lanes cannot carry
 * into each other below a factor of 257. */
static void dim_buf(volatile u8 *buf, u32 size, int a) {
  u32 *p = (u32 *)buf, *end = (u32 *)(buf + size);
  u32 f = (u32)(a < 0 ? 0 : a > 256 ? 256 : a);
  if (f == 256)
    return;
  for (; p < end; p++) {
    u32 w = *p;
    *p = (((w & 0x00FF00FFu) * f >> 8) & 0x00FF00FFu) |
         ((((w >> 8) & 0x00FF00FFu) * f) & 0xFF00FF00u);
  }
}

void fx_dim(int mask, int alpha) {
  for (int i = 0; i < 2; i++)
    if (mask & (1 << i))
      dim_buf(back(i), fb_size(i), alpha);
}

static u8 *snap(int i, int fresh) {
  if (i)
    return fresh ? SNAP_BOT_NEW : SNAP_BOT_OLD;
  return fresh ? SNAP_TOP_NEW : SNAP_TOP_OLD;
}

/* A framebuffer runs down columns, left to right, so a horizontal push is
 * two straight copies per screen. */
static void push_frame(int i, int dir, int s) {
  int w = fb_width(i);
  u8 *old = snap(i, 0), *fresh = snap(i, 1);
  volatile u8 *b = back(i);
  u32 keep = (u32)(w - s) * COL_BYTES, in = (u32)s * COL_BYTES;

  if (dir > 0) {
    fx_copy(b, old + in, keep);
    fx_copy(b + keep, fresh, in);
  } else {
    fx_copy(b, fresh + keep, in);
    fx_copy(b + in, old, keep);
  }
}

void fx_push(int mask, int dir, void (*draw)(void *), void *ctx, u32 ms) {
  u32 start;
  int t = 0;

  for (int i = 0; i < 2; i++)
    if (mask & (1 << i))
      fx_copy(snap(i, 0), back(i), fb_size(i));
  draw(ctx);
  for (int i = 0; i < 2; i++)
    if (mask & (1 << i))
      fx_copy(snap(i, 1), back(i), fb_size(i));

  start = fx_ms();
  while (t < 256) {
    t = fx_progress(start, fx_ms(), ms);
    int e = fx_ease_in_out(t);
    for (int i = 0; i < 2; i++)
      if (mask & (1 << i))
        push_frame(i, dir, (fb_width(i) * e) >> 8);
    fx_present(mask);
  }
}

void fx_fade_out(int mask, u32 ms) {
  u32 start;
  int t = 0;

  for (int i = 0; i < 2; i++)
    if (mask & (1 << i))
      fx_copy(snap(i, 0), back(i), fb_size(i));
  start = fx_ms();
  while (t < 256) {
    t = fx_progress(start, fx_ms(), ms);
    for (int i = 0; i < 2; i++)
      if (mask & (1 << i)) {
        fx_copy(back(i), snap(i, 0), fb_size(i));
        dim_buf(back(i), fb_size(i), 256 - fx_ease_in_out(t));
      }
    fx_present(mask);
  }
}

int fx_progress(u32 start, u32 now, u32 ms) {
  u32 d = now - start;
  if (!ms || d >= ms)
    return 256;
  return (int)((d << 8) / ms);
}

int fx_ease_out(int t) {
  int u = 256 - t;
  return 256 - ((u * u >> 8) * u >> 8);
}

int fx_ease_in_out(int t) { return (t * t >> 8) * (768 - 2 * t) >> 8; }

/* 1 + 2.7 (t-1)^3 + 1.7 (t-1)^2, the usual "back" curve. */
int fx_ease_back(int t) {
  int u = t - 256, u2 = u * u >> 8, u3 = u2 * u >> 8;
  return 256 + (692 * u3 >> 8) + (436 * u2 >> 8);
}

int fx_lerp(int a, int b, int t) { return a + (((b - a) * t) >> 8); }

Color fx_mix(Color a, Color b, int t) {
  Color c;
  c.r = (u8)fx_lerp(a.r, b.r, t);
  c.g = (u8)fx_lerp(a.g, b.g, t);
  c.b = (u8)fx_lerp(a.b, b.b, t);
  return c;
}
