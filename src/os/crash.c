/*
 * AuroraOS custom crash screen (ARM9).
 *
 * Blue (#0000FF) on both screens: a big sad face on the top screen, and the
 * crash reason + CPU register dump on the bottom. Replaces the default hang /
 * upstream crash handler. See include/crash.h and src/os/crash.s.
 */
#include "crash.h"
#include "crash_shared.h"
#include "font.h"
#include "i2c.h"

/* Shared with crash.s (filled by the exception stubs). */
CrashDump g_crash_dump;

/* From crash.s. */
extern void crash_vec_undef(void);
extern void crash_vec_pabt(void);
extern void crash_vec_dabt(void);
extern void crash_hang(void);

/* From os_launch.s: clean+invalidate caches so the new vectors are fetched. */
extern void os_cache_sync(void);

#define COLOR_CRASH ((Color){0x00, 0x00, 0xFF}) /* #0000ff */
#define SH_T TOP_SCREEN_HEIGHT
#define SH_B BOT_SCREEN_HEIGHT

/* Formatting */

static char *scpy(char *d, const char *s) {
  while ((*d = *s)) {
    d++;
    s++;
  }
  return d;
}

static void hex8(char *o, u32 v) { /* "XXXXXXXX" (8 chars, no prefix) */
  static const char h[] = "0123456789ABCDEF";
  for (int i = 0; i < 8; i++)
    o[i] = h[(v >> ((7 - i) * 4)) & 0xF];
  o[8] = '\0';
}

static const char *reason_name(u32 exc) {
  switch (exc) {
    case CRASH_UNDEF: return "Undefined Instruction";
    case CRASH_PABT:  return "Prefetch Abort";
    case CRASH_DABT:  return "Data Abort";
    case CRASH_USER:  return "User Forced Crash";
    default:          return "Unknown";
  }
}

/* Drawing helpers */

static void disc(int cx, int cy, int r, Color c) {
  draw_filled_round_rect(VRAM_TOP_LA, cx - r, cy - r, 2 * r, 2 * r, r, SH_T, c);
}

static void thick_line(int x0, int y0, int x1, int y1, int t, Color c) {
  int dx = x1 - x0, dy = y1 - y0;
  int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
  int steps = adx > ady ? adx : ady;
  if (steps < 1)
    steps = 1;
  for (int i = 0; i <= steps; i++) {
    int x = x0 + dx * i / steps, y = y0 + dy * i / steps;
    draw_filled_rect(VRAM_TOP_LA, x, y, t, t, SH_T, c);
  }
}

/* Big sad face centred on the top screen: two eyes, angled brows, a frown. */
static void draw_sad_face(void) {
  Color w = COLOR_WHITE;
  int cx = TOP_SCREEN_WIDTH / 2;

  disc(cx - 55, 95, 17, w); /* eyes */
  disc(cx + 55, 95, 17, w);

  /* Down-slanted brows (inner ends low) -> upset look. */
  thick_line(cx - 82, 55, cx - 30, 72, 6, w);
  thick_line(cx + 82, 55, cx + 30, 72, 6, w);

  /* Frown: an upward arch (corners droop down). Parabola about (cx, my). */
  int my = 185, wdt = 78, hgt = 40, th = 8;
  for (int dx = -wdt; dx <= wdt; dx++) {
    int y = my - (hgt - (hgt * dx * dx) / (wdt * wdt));
    draw_filled_rect(VRAM_TOP_LA, cx + dx, y, 4, th, SH_T, w);
  }
}

static void draw_dump(CrashDump *d) {
  volatile u8 *fb = VRAM_BOT_A;
  Color w = COLOR_WHITE, bg = COLOR_CRASH;
  char line[48], hx[12];
  char *p;

  draw_string(fb, 8, 8, SH_B, "AURORA - FATAL CRASH", w, bg);

  if (d->cpu == CRASH_CPU_ARM11) {
    p = scpy(line, "Processor: ARM11  core ");
    *p++ = (char)('0' + (d->core & 0x7));
    *p = '\0';
  } else {
    scpy(line, "Processor: ARM9 (single core)");
  }
  draw_string(fb, 8, 26, SH_B, line, w, bg);

  p = scpy(line, "Reason:    ");
  scpy(p, reason_name(d->exc));
  draw_string(fb, 8, 44, SH_B, line, w, bg);

  p = scpy(line, "PC:   0x");
  hex8(hx, d->pc);
  scpy(p, hx);
  draw_string(fb, 8, 62, SH_B, line, w, bg);

  p = scpy(line, "CPSR: 0x");
  hex8(hx, d->cpsr);
  scpy(p, hx);
  draw_string(fb, 8, 76, SH_B, line, w, bg);

  if (d->exc == CRASH_DABT) {
    p = scpy(line, "Fault @ 0x");
    hex8(hx, d->dfar);
    p = scpy(p, hx);
    p = scpy(p, "  DFSR 0x");
    hex8(hx, d->dfsr);
    scpy(p, hx);
    draw_string(fb, 8, 90, SH_B, line, w, bg);
  }

  /* r0-r12, three per row. */
  int y = 108;
  for (int i = 0; i < 13; i++) {
    int col = i % 3;
    p = line;
    *p++ = 'r';
    if (i >= 10) {
      *p++ = '1';
      *p++ = (char)('0' + (i - 10));
    } else {
      *p++ = (char)('0' + i);
    }
    *p++ = ':';
    hex8(p, d->r[i]);
    draw_string(fb, 8 + col * 104, y, SH_B, line, w, bg);
    if (col == 2)
      y += 14;
  }
  /* The auto-power-off countdown is drawn along the bottom by crash_handle(). */
}

/* Power the console off via the MCU (same sequence as os_power_off). */
static void crash_power_off(void) {
  I2C_init();
  I2C_writeReg(I2C_DEV_MCU, 0x22, 1 << 0); /* LCDs off */
  __asm__ volatile("mcr p15, 0, %0, c7, c10, 4" ::"r"(0) : "memory");
  I2C_writeReg(I2C_DEV_MCU, 0x20, 1 << 0); /* system power off */
  for (;;)
    __asm__ volatile("mcr p15, 0, r0, c7, c0, 4");
}

/* Wait ~1 real second using the MCU real-time clock (reg 0x30, first byte =
 * seconds), which is an actual clock -- busy-loops are not. Returns as soon as
 * the seconds value changes. The delay(40000) chunk both paces the polling and,
 * if the RTC never advances, bounds the wait to a ~2 s busy fallback so it can
 * never hang. */
static void crash_wait_1s(void) {
  u8 start = 0, now = 0;
  I2C_readRegBuf(I2C_DEV_MCU, 0x30, &start, 1);
  now = start;
  for (int i = 0; i < 500; i++) {
    delay(40000);
    I2C_readRegBuf(I2C_DEV_MCU, 0x30, &now, 1);
    if (now != start)
      return; /* a real second elapsed */
  }
}

/* Entry */

void crash_handle(CrashDump *d) {
  /* Mask IRQ + FIQ so nothing disturbs the final screen. */
  __asm__ volatile("mrs r0, cpsr\n\t"
                   "orr r0, r0, #0xC0\n\t"
                   "msr cpsr_c, r0"
                   :
                   :
                   : "r0", "memory");

  clear_screen(VRAM_TOP_LA, TOP_FB_SIZE, COLOR_CRASH);
  clear_screen(VRAM_BOT_A, BOT_FB_SIZE, COLOR_CRASH);
  draw_sad_face();
  draw_dump(d);

  I2C_init(); /* bring up I2C so the RTC-based countdown can read the clock */

  /* Count down and auto power off after 10 seconds. */
  for (int s = 10; s >= 1; s--) {
    char line[40], *p;
    draw_filled_rect(VRAM_BOT_A, 0, SH_B - 18, BOT_SCREEN_WIDTH, 18, SH_B,
                     COLOR_CRASH);
    p = scpy(line, "Auto power off in ");
    if (s >= 10) {
      *p++ = '1';
      *p++ = '0';
    } else {
      *p++ = (char)('0' + s);
    }
    scpy(p, "s...");
    draw_string(VRAM_BOT_A, 8, SH_B - 16, SH_B, line, COLOR_WHITE, COLOR_CRASH);
    crash_wait_1s();
  }
  crash_power_off();

  for (;;)
    ; /* unreachable */
}

void crash_force(void) {
  crash_capture(&g_crash_dump);
  g_crash_dump.exc = CRASH_USER;
  g_crash_dump.dfsr = 0;
  g_crash_dump.dfar = 0;
  g_crash_dump.cpu = CRASH_CPU_ARM9;
  g_crash_dump.core = 0;
  crash_handle(&g_crash_dump);
}

/* Poll the cross-core crash block for an ARM11 fault. Fast path: invalidate the
 * one cache line and read the magic. On a hit, sync and show the crash. */
void crash_poll_arm11(void) {
  volatile CrashShared *cs = (volatile CrashShared *)CRASH_SHARED_ADDR;
  __asm__ volatile("mcr p15, 0, %0, c7, c6, 1" ::"r"(CRASH_SHARED_ADDR)
                   : "memory"); /* invalidate D-cache line by MVA */
  if (cs->magic != CRASH_SHARED_MAGIC)
    return;

  os_cache_sync(); /* pull the whole dump in fresh */
  static CrashDump d;
  for (int i = 0; i < 13; i++)
    d.r[i] = cs->r[i];
  d.pc = cs->pc;
  d.cpsr = cs->cpsr;
  d.exc = cs->exc;
  d.dfsr = cs->dfsr;
  d.dfar = cs->dfar;
  d.cpu = cs->cpu;
  d.core = cs->core;
  crash_handle(&d); /* never returns */
}

void crash_init(void) {
  /* Vector base: high (0xFFFF0000) if SCTLR.V (bit 13) is set, else low (0). */
  u32 sctlr;
  __asm__ volatile("mrc p15, 0, %0, c1, c0, 0" : "=r"(sctlr));
  volatile u32 *vec =
      (volatile u32 *)((sctlr & (1u << 13)) ? 0xFFFF0000u : 0x00000000u);

  /* Each of the 8 vectors: LDR PC, [PC, #0x18]  (fetch handler from vec[i+8]). */
  for (int i = 0; i < 8; i++)
    vec[i] = 0xE59FF018u;

  vec[8]  = (u32)crash_hang;       /* reset    */
  vec[9]  = (u32)crash_vec_undef;  /* undef    */
  vec[10] = (u32)crash_hang;       /* swi      */
  vec[11] = (u32)crash_vec_pabt;   /* prefetch */
  vec[12] = (u32)crash_vec_dabt;   /* data     */
  vec[13] = (u32)crash_hang;       /* reserved */
  vec[14] = (u32)crash_hang;       /* irq      */
  vec[15] = (u32)crash_hang;       /* fiq      */

  /* Clear the cross-core crash block so a stale/garbage magic (FCRAM is not
   * cleared at boot) can't be mistaken for an ARM11 crash. */
  ((volatile CrashShared *)CRASH_SHARED_ADDR)->magic = 0;

  os_cache_sync(); /* flush D-cache + invalidate I-cache so fetches are fresh */
}
