/* Auric runtime shim: implementation.
 *
 * Bridges the Auric built-ins onto AuroraOS's existing API. The drawing
 * primitives (draw_string, clear_screen, ...) come from AuroraOS's own
 * src/screen.c, which aurc compiles alongside this file; the input and timing
 * helpers live here so an Auric app does not have to pull in the whole Home
 * Menu (os_main.c).
 *
 * Everything targets the TOP screen (400x240): print and clear both draw there,
 * origin top-left, 8x8 font, keeping the coordinate model a single flat space.
 */
#include "aurora.h"
#include "gpu.h"             /* GPU-accelerated present               */
#include "i2c.h"             /* MCU access for the HOME button        */
#include "loader.h"          /* shared HOME-return contract addresses */
#include "auric_runtime.h"

/* ---- ARM9 hardware timers ----------------------------------------------
 * Four 16-bit timers at 0x10003000, laid out like the NDS's: a value/reload
 * register then a control register, 4 bytes per timer. Control bits: 0-1
 * prescaler (0=/1, 1=/64, 2=/256, 3=/1024), bit2 count-up, bit7 enable.
 *
 * Timer 0 runs free at /1024 and timer 1 counts its overflows, giving a 32-bit
 * tick counter that wraps after about 18 hours, far longer than any session,
 * so millis() never needs to handle a rollover.
 *
 * The ARM9 timer base clock is 67.03 MHz, so /1024 is 65457 ticks per second,
 * i.e. ~65 per millisecond. This is the only value here that depends on the
 * clock frequency: if timing runs off by a clean factor, correct it and
 * everything else scales with it. */
#define TIMER_BASE     0x10003000u
#define TIMER0_VAL     (*(volatile u16 *)(TIMER_BASE + 0x00))
#define TIMER0_CNT     (*(volatile u16 *)(TIMER_BASE + 0x02))
#define TIMER1_VAL     (*(volatile u16 *)(TIMER_BASE + 0x04))
#define TIMER1_CNT     (*(volatile u16 *)(TIMER_BASE + 0x06))
#define TIMER_PRESCALE_1024 3u
#define TIMER_COUNT_UP      (1u << 2)
#define TIMER_ENABLE        (1u << 7)
#define TICKS_PER_MS        65u

static int aur_timer_ready = 0;

static void aur_timer_init(void) {
  TIMER0_CNT = 0; /* stop both before reprogramming */
  TIMER1_CNT = 0;
  TIMER0_VAL = 0; /* reload value: count the full 16-bit range */
  TIMER1_VAL = 0;
  TIMER1_CNT = (u16)(TIMER_ENABLE | TIMER_COUNT_UP);
  TIMER0_CNT = (u16)(TIMER_ENABLE | TIMER_PRESCALE_1024);
  aur_timer_ready = 1;
}

static u32 aur_ticks(void) {
  u32 hi, lo, hi2;

  if (!aur_timer_ready)
    aur_timer_init();
  /* Re-read the low half if the high half moved between the two reads. */
  hi = TIMER1_VAL;
  lo = TIMER0_VAL;
  hi2 = TIMER1_VAL;
  if (hi != hi2) {
    hi = hi2;
    lo = TIMER0_VAL;
  }
  return (hi << 16) | lo;
}

int aur_millis(void) { return (int)(aur_ticks() / TICKS_PER_MS); }

/* Current text-background colour, updated by aur_clear so text drawn after a
 * clear() sits on a matching background instead of a black box. */
static Color aur_bg = {0x00, 0x00, 0x00};

static Color aur_unpack(int packed) {
  Color c;
  c.r = (u8)((packed >> 16) & 0xFF);
  c.g = (u8)((packed >> 8) & 0xFF);
  c.b = (u8)(packed & 0xFF);
  return c;
}

/* ---- input / timing (mirrors os_main.c so apps stay self-contained) ----- */
void delay(volatile u32 cycles) {
  while (cycles--)
    __asm__ volatile("nop");
}

/* Pad bits 0..9 are A/B/Select/Start/D-pad/R/L; 10 and 11 are X and Y. */
u32 get_keys(void) { return ~REG_HID_PAD & 0xFFF; }

static u32 aur_prev_keys = 0;
u32 get_keys_down(void) {
  u32 cur = get_keys();
  u32 down = cur & ~aur_prev_keys;
  aur_prev_keys = cur;
  return down;
}

/* HOME is not on the HID pad; it is reported by the MCU. Reading MCU IRQ
 * register 0x10 clears its flags; bit 2 of byte 0 signals a HOME press. */
static int aur_i2c_ready = 0;

static int aur_home_pressed(void) {
  if (!aur_i2c_ready) {
    I2C_init();
    aur_i2c_ready = 1;
  }
  u8 irq[4];
  if (!I2C_readRegBuf(I2C_DEV_MCU, 0x10, irq, sizeof(irq)))
    return 0;
  return (irq[0] & 0x04) != 0; /* bit 2 = HOME pressed */
}

/* The descriptor magic gates the return: when an app is booted directly (not
 * launched by the Home Menu) there is no OS to go back to, so HOME is ignored. */
static void aur_home_poll_now(void) {
  volatile u32 *desc = (volatile u32 *)AURORA_RETURN_DESC_ADDR;
  if (desc[0] != AURORA_RETURN_READY_MAGIC)
    return;
  if (aur_home_pressed())
    ((void (*)(void))AURORA_RETURN_STUB_ADDR)(); /* restores the OS; no return */
}

/* Called from every built-in so any app returns to the Home Menu on HOME.
 * Reading the MCU costs a full I2C transaction, and a game can call built-ins
 * hundreds of times per frame (one fill_rect per board cell), so sample every
 * so often rather than on every call. A handful of built-ins run in well under
 * a millisecond, so HOME still responds immediately to a human. Blocking calls
 * poll unthrottled via aur_home_poll_now(). */
static int aur_home_tick = 0;

void aur_check_home(void) {
  if (++aur_home_tick < 64)
    return;
  aur_home_tick = 0;
  aur_home_poll_now();
}

/* Frame control. In immediate mode (the default) every drawing built-in pushes
 * the screen straight away, which is what simple programs expect. A game turns
 * on buffered mode and calls present() once per frame instead, so a frame is
 * never shown half-drawn. */
static int aur_buffered_mode = 0;

static void aur_auto_present(void) {
  if (!aur_buffered_mode)
    screen_present_top();
}

void aur_buffered(int on) {
  aur_check_home();
  aur_buffered_mode = on ? 1 : 0;
  if (aur_buffered_mode && g_screen_blit == 0) {
    /* Present through the GPU where possible. The ARM11 core stays resident
     * while an app runs, so a full-screen present becomes one DMA instead of a
     * 288 KB CPU copy. Only attempted when the Home Menu did the launching: the
     * descriptor that gates the HOME return also indicates the OS, and
     * therefore the ARM11 core, is present to answer. A directly booted app
     * keeps the CPU copy rather than stalling on a GPU that never replies. */
    volatile u32 *desc = (volatile u32 *)AURORA_RETURN_DESC_ADDR;
    if (desc[0] == AURORA_RETURN_READY_MAGIC && gpu_init())
      g_screen_blit = gpu_texcopy;
  }
  screen_use_backbuffer(aur_buffered_mode);
}

void aur_present(void) {
  aur_check_home();
  screen_present_top();
}

/* ---- built-ins ---------------------------------------------------------- */
void aur_print(const char *text, int x, int y, int color) {
  aur_check_home();
  draw_string(VRAM_TOP_LA, x, y, TOP_SCREEN_HEIGHT, text, aur_unpack(color),
              aur_bg);
  aur_auto_present();
}

/* Signed decimal, built back-to-front into a small stack buffer. */
void aur_print_int(int value, int x, int y, int color) {
  char buf[13];
  int i = sizeof(buf) - 1;
  unsigned int mag;
  int negative = value < 0;

  aur_check_home();
  buf[i] = '\0';
  mag = negative ? (unsigned int)(-(long)value) : (unsigned int)value;
  do {
    buf[--i] = (char)('0' + (mag % 10u));
    mag /= 10u;
  } while (mag && i > 1);
  if (negative && i > 0)
    buf[--i] = '-';

  draw_string(VRAM_TOP_LA, x, y, TOP_SCREEN_HEIGHT, &buf[i], aur_unpack(color),
              aur_bg);
  aur_auto_present();
}

void aur_clear(int color) {
  aur_check_home();
  aur_bg = aur_unpack(color);
  clear_screen(VRAM_TOP_LA, TOP_FB_SIZE, aur_bg);
  aur_auto_present();
}

void aur_fill_rect(int x, int y, int w, int h, int color) {
  aur_check_home();
  draw_filled_rect(VRAM_TOP_LA, x, y, w, h, TOP_SCREEN_HEIGHT, aur_unpack(color));
  aur_auto_present();
}

int aur_keys_down(void) {
  aur_check_home();
  return (int)get_keys_down();
}

int aur_keys_held(void) {
  aur_check_home();
  return (int)get_keys();
}

/* xorshift32, seeded once from the MCU clock so runs differ. If the MCU does
 * not answer, the fixed fallback still gives a usable sequence. */
static u32 aur_rng = 0;

static void aur_seed_rng(void) {
  u8 t[3];
  aur_rng = 0x2545F491u;
  if (aur_i2c_ready == 0) {
    I2C_init();
    aur_i2c_ready = 1;
  }
  if (I2C_readRegBuf(I2C_DEV_MCU, 0x30, t, sizeof(t)))
    aur_rng ^= (u32)t[0] | ((u32)t[1] << 8) | ((u32)t[2] << 16);
  if (aur_rng == 0)
    aur_rng = 1; /* xorshift is stuck at zero */
}

int aur_rand(int n) {
  aur_check_home();
  if (n <= 1)
    return 0;
  if (aur_rng == 0)
    aur_seed_rng();
  aur_rng ^= aur_rng << 13;
  aur_rng ^= aur_rng >> 17;
  aur_rng ^= aur_rng << 5;
  return (int)((aur_rng >> 1) % (u32)n);
}

void aur_wait_key(int button) {
  /* Prime the edge detector so a button already held on entry is ignored. */
  get_keys_down();
  while (1) {
    aur_home_poll_now(); /* blocking call: poll HOME every iteration */
    if (get_keys_down() & (u32)button)
      return;
    delay(60000);
  }
}

void aur_delay(int cycles) {
  aur_check_home();
  if (cycles > 0)
    delay((u32)cycles);
}
