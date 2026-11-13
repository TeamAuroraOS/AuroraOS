#include "core11.h"
#include "touch.h"
#include "wifi.h"

/* GPIO1 bit 2 follows the magnetic lid sensor (0 open, 1 closed). */
#define GPIO1_DATA          0x10147000u
#define GPIO1_SHELL_CLOSED  (1u << 2)

/* Exception stubs from audio11_start.s, which capture a fault into the
 * cross-core crash block. */
extern void crash_vec_undef11(void);
extern void crash_vec_pabt11(void);
extern void crash_vec_dabt11(void);
extern void crash_hang11(void);

static void crash11_init(void) {
  uint32_t sctlr;
  __asm__ volatile("mrc p15, 0, %0, c1, c0, 0" : "=r"(sctlr));
  volatile uint32_t *vec =
      (volatile uint32_t *)((sctlr & (1u << 13)) ? 0xFFFF0000u : 0u);

  for (int i = 0; i < 8; i++)
    vec[i] = 0xE59FF018u; /* LDR PC, [PC, #0x18] */
  vec[8]  = (uint32_t)crash_hang11;      /* reset */
  vec[9]  = (uint32_t)crash_vec_undef11; /* undef */
  vec[10] = (uint32_t)crash_hang11;      /* swi */
  vec[11] = (uint32_t)crash_vec_pabt11;  /* prefetch */
  vec[12] = (uint32_t)crash_vec_dabt11;  /* data */
  vec[13] = (uint32_t)crash_hang11;      /* reserved */
  vec[14] = (uint32_t)crash_hang11;      /* irq */
  vec[15] = (uint32_t)crash_hang11;      /* fiq */

  __asm__ volatile("mcr p15, 0, %0, c7, c10, 0" ::"r"(0)); /* clean D-cache */
  __asm__ volatile("mcr p15, 0, %0, c7, c5, 0" ::"r"(0));  /* invalidate I */
  __asm__ volatile("mcr p15, 0, %0, c7, c10, 4" ::"r"(0)); /* DSB */
}

/* Each panel's LCD block (3dbrew "LCD Registers"): +0x00 bit 0 turns ABL on,
 * +0x40 is the backlight PWM duty (bits 0-9, 0 is off), +0x44 the period
 * minus one (bits 0-9) with bit 16 the enable. */
#define LCD_TOP        0x10202200u
#define LCD_BOT        0x10202A00u
#define LCD_FLAGS(b)   MMIO32((b) + 0x00u)
#define LCD_BL_DUTY(b) MMIO32((b) + 0x40u)
#define LCD_BL_PWM(b)  MMIO32((b) + 0x44u)
#define LCD_ABL_ON     (1u << 0)
#define LCD_BL_PWM_EN  (1u << 16)
#define LCD_BL_PERIOD  575u /* the 0x1023E that boot code and libn3ds set */

int stereo11_is3d(void);

/* Duty per level for 20, 43, 73, 95 and 117 cd/m2 (the 3DS's five steps),
 * from libn3ds's default calibration (launch-day Old 3DS): a quadratic per
 * panel, divided by 512 / 575. The top panel needs less drive than the
 * bottom one in 2D and more in 3D. */
static const uint16_t bl_top2d[5] = {15, 33, 57, 75, 94};
static const uint16_t bl_top3d[5] = {36, 80, 140, 187, 237};
static const uint16_t bl_bot[5]   = {32, 71, 123, 162, 203};
static int bl_level = -1;
static int lid_blanked;
static uint32_t lid_saved_top, lid_saved_bot;

static void bl_panel(uint32_t base, uint32_t duty) {
  uint32_t pwm = LCD_BL_PWM(base);
  if (!(pwm & LCD_BL_PWM_EN)) {
    pwm = LCD_BL_PWM_EN | (LCD_BL_PERIOD - 1u);
    LCD_BL_PWM(base) = pwm;
  }
  /* With ABL on the hardware moves the duty with the picture, so a set level
   * would not hold; libn3ds turns it off for the same reason. */
  LCD_FLAGS(base) &= ~LCD_ABL_ON;
  LCD_BL_DUTY(base) =
      (duty * ((pwm & 0x3FFu) + 1u) + LCD_BL_PERIOD / 2u) / LCD_BL_PERIOD;
}

void backlight11_refresh(void) {
  if (bl_level < 0 || lid_blanked)
    return;
  bl_panel(LCD_TOP, (stereo11_is3d() ? bl_top3d : bl_top2d)[bl_level]);
  bl_panel(LCD_BOT, bl_bot[bl_level]);
  dsb();
}

void backlight11_set(AudioCtrl *ct, uint32_t level) {
  bl_level = (int)(level > 4u ? 4u : level);
  backlight11_refresh();
  ct->diag4 = LCD_FLAGS(LCD_TOP);
  ct->diag5 = LCD_BL_DUTY(LCD_TOP);
  ct->diag6 = LCD_BL_PWM(LCD_TOP);
  ct->diag7 = LCD_BL_DUTY(LCD_BOT);
}

/* This core's MPCore private timer runs free from start-up. It counts at half
 * the CPU clock, so a prescaler of 134 gives a tick a microsecond at 268 MHz
 * and three at 804 MHz. Clock switches and RUN11 code may borrow it. */
#define PTIMER_LOAD  0x17E00600u
#define PTIMER_COUNT 0x17E00604u
#define PTIMER_CNT   0x17E00608u
#define PTIMER_STAT  0x17E0060Cu
#define PTIMER_EN    (1u << 0)
#define PTIMER_RUN   ((133u << 8) | 2u | 1u) /* prescaler, reload, enable */

/* A millisecond at 804 MHz, in those ticks. It keeps a wait from ever being
 * short; at 268 MHz it lasts three times as long, which the codec settle delays
 * allow. */
#define PTIMER_TICKS_PER_MS 3000u

static int ptimer_state; /* 0 unchecked, 1 counts, -1 does not */

static int ptimer_counts(void) {
  uint32_t a, b;
  MMIO32(PTIMER_CNT) = 0;
  MMIO32(PTIMER_LOAD) = 0xFFFFFFFFu;
  MMIO32(PTIMER_CNT) = PTIMER_EN;
  a = MMIO32(PTIMER_COUNT);
  spin(200);
  b = MMIO32(PTIMER_COUNT);
  MMIO32(PTIMER_CNT) = 0;
  return b < a;
}

static uint32_t tb_raw, tb_us, tb_rem, tb_div = 1u;

static inline uint32_t ptimer_ticks(void) { return ~MMIO32(PTIMER_COUNT); }

uint32_t core11_clock_div(void) { return tb_div; }

/* Ticks folded into microseconds as they pass, so the result still wraps at
 * 2^32 at any clock, which `now - then` needs. */
uint32_t core11_now_us(void) {
  uint32_t raw = ptimer_ticks(), d = raw - tb_raw;
  tb_raw = raw;
  if (tb_div == 1u) {
    tb_us += d;
    return tb_us;
  }
  d += tb_rem;
  /* The reciprocal of 3, since a pass here should cost next to nothing. */
  uint32_t q = tb_div == 3u
                   ? (uint32_t)(((uint64_t)d * 0xAAAAAAABu) >> 33)
                   : d >> 1;
  tb_rem = d - q * tb_div;
  tb_us += q;
  return tb_us;
}

/* Starts the timer, or restarts it after Clock11.c had it. The clock carries on
 * from its last reading, at the new rate, so the moments of a switch do not
 * count, and nothing waiting across one sees a jump. Publishes the clock too. */
static void timebase_start(AudioCtrl *ct) {
  MMIO32(PTIMER_CNT) = 0;
  MMIO32(PTIMER_STAT) = 1u;
  MMIO32(PTIMER_LOAD) = 0xFFFFFFFFu;
  MMIO32(PTIMER_CNT) = PTIMER_RUN;
  tb_raw = ptimer_ticks();
  tb_rem = 0;
  tb_div = clock11_div(ct);
  ct->clock_div = tb_div;
}

void core11_clock_restart(AudioCtrl *ct) { timebase_start(ct); }

/* Timed rather than counted: how long a nop loop takes depends on the caches
 * and the clock. */
void sleep_ms(uint32_t ms) {
  if (ptimer_state < 0) {
    spin(ms * 300000u);
    return;
  }
  while (ms) {
    uint32_t part = ms > 1000u ? 1000u : ms;
    uint32_t t0 = ptimer_ticks();
    ms -= part;
    while (ptimer_ticks() - t0 < part * PTIMER_TICKS_PER_MS)
      ;
  }
}

/* The mailbox wait in audio11_start.s, position-independent. */
extern const uint8_t core11_park_stub[];
extern const uint8_t core11_park_stub_end[];

/* AUDIO_CMD_PARK. The stub is copied out before the ack, so once the ARM9 sees
 * the ack only a few instructions of this image are left to run, and the ARM9
 * still waits for the stub itself to report AUDIO_ST_PARKED. Does not return. */
static void core11_park(AudioCtrl *ct) {
  const uint8_t *s = core11_park_stub;
  uint8_t *d = (uint8_t *)AUDIO_PARK_ADDR;
  uint32_t n = (uint32_t)(core11_park_stub_end - core11_park_stub);

  audio11_stop_all();
  stereo11_off();
  gpu11_show_a(); /* the next core's OS draws to framebuffer A */
  for (uint32_t i = 0; i < n; i++)
    d[i] = s[i];
  MMIO32(AUDIO_ARM11_MAILBOX) = 0;
  /* The next core starts from nothing and may load over this image. */
  mmu11_off();
  ct->magic = 0;
  ct->ack_seq = ct->cmd_seq;
  dsb();
  __asm__ volatile("mcr p15, 0, %0, c7, c5, 0" ::"r"(0)); /* invalidate I */
  ((void (*)(void))AUDIO_PARK_ADDR)();
}

/* The main loop's rhythm, in time, since a pass's length depends on the caches
 * and the clock. */
#define CORE_PASS_US  50u
#define CORE_TOUCH_US 5000u
#define CORE_MENU_US  5000u
#define CORE_LID_US   100000u

void core11_sleep_poll(void) {
  if (MMIO8(GPIO1_DATA) & GPIO1_SHELL_CLOSED) {
    if (!lid_blanked) {
      lid_saved_top = LCD_BL_DUTY(LCD_TOP);
      lid_saved_bot = LCD_BL_DUTY(LCD_BOT);
      lid_blanked = 1;
      LCD_BL_DUTY(LCD_TOP) = 0;
      LCD_BL_DUTY(LCD_BOT) = 0;
    }
  } else if (lid_blanked) {
    lid_blanked = 0;
    if (bl_level >= 0)
      backlight11_refresh();
    else {
      LCD_BL_DUTY(LCD_TOP) = lid_saved_top;
      LCD_BL_DUTY(LCD_BOT) = lid_saved_bot;
    }
  }
  dsb();
}

void audio11_main(void) {
  AudioCtrl *ct = (AudioCtrl *)AUDIO_CTRL_ADDR;
  uint32_t lid_t, menu_t, touch_t;

  ct->status = AUDIO_ST_BOOT;
  ct->ack_seq = ct->cmd_seq;
  ct->version = AUDIO_CORE_VERSION;
  ct->diag0 = ct->diag1 = ct->diag2 = ct->diag3 = 0;
  ct->diag4 = ct->diag5 = ct->diag6 = ct->diag7 = 0;
  ct->socinfo = *(volatile uint32_t *)0x10140FFC; /* CFG11_SOCINFO */
  ptimer_state = ptimer_counts() ? 1 : -1;
  timebase_start(ct);
  /* mmu11_init reads the clock div through l2_start, so after the timebase. */
  mmu11_init();
  video11_init();
  wifi11_init();
  ct->magic = AUDIO_MAGIC;
  dcache_clean();

  /* Not called: writing the ARM11 vector page is unverified, and can hang the
   * core before its main loop. */
  (void)crash11_init;

  codec11_bus_init();
  audio11_init(ct);

  /* After audio is up, so a touch problem cannot stop audio from initialising. */
  touch11_init();

  lid_t = menu_t = touch_t = core11_now_us();
  for (;;) {
    uint32_t now = core11_now_us();
    if (now - lid_t >= CORE_LID_US) {
      lid_t = now;
      core11_sleep_poll();
    }

    /* Polled before a pending command is taken, so an app waiting on its
     * present stays held there while the menu is open. */
    if (now - menu_t >= CORE_MENU_US) {
      menu_t = now;
      system_menu11_poll();
    }
    if (ct->cmd_seq != ct->ack_seq) {
      dcache_clean_inval(); /* a command is here: pull everything it needs */
      uint32_t cmd = ct->cmd;
      uint32_t arg0 = ct->arg0;
      if (!audio11_command(ct, cmd, arg0)) {
        if (cmd == AUDIO_CMD_WIFI)
          wifi11_probe();
        else if (cmd == AUDIO_CMD_WIFI_BOOT)
          wifi11_boot(arg0);
        else if (cmd == AUDIO_CMD_WIFI_NET)
          wifi11_net(arg0);
        else if (cmd == AUDIO_CMD_GPU)
          gpu11_run();
        else if (cmd == AUDIO_CMD_N3DS) {
          /* As libn3ds says of the same switch, with no map on any core. */
          mmu11_off();
          clock11_set(ct, arg0);
          timebase_start(ct);
          mmu11_on();
        }
        else if (cmd == AUDIO_CMD_BRIGHTNESS)
          backlight11_set(ct, arg0);
        else if (cmd == AUDIO_CMD_RUN11) {
          /* External code may change the prescaler, reload the counter or stop
           * it altogether. Fold in OS time before handing it over, then reclaim
           * it without treating guest ticks as elapsed OS microseconds. */
          (void)core11_now_us();
          run11(ct, arg0, ct->arg1);
          timebase_start(ct);
        }
        else if (cmd == AUDIO_CMD_SMP)
          smp11_command(ct, arg0, ct->arg1);
        else if (cmd == AUDIO_CMD_PARK)
          core11_park(ct);
      }
      ct->ack_seq = ct->cmd_seq;
      dcache_clean();
    }

    if (now - touch_t >= CORE_TOUCH_US) {
      touch_t = now;
      touch11_poll();
    }
    stereo11_tick();
    wifi11_tick();
    video11_tick();
    /* Reads only this core's own timer, so the ARM9 keeps the bus meanwhile. */
    while (core11_now_us() - now < CORE_PASS_US)
      ;
  }
}
