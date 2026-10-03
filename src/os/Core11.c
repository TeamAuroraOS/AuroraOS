#include "core11.h"
#include "touch.h"
#include "wifi.h"

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
  vec[8]  = (uint32_t)crash_hang11;      /* reset    */
  vec[9]  = (uint32_t)crash_vec_undef11; /* undef    */
  vec[10] = (uint32_t)crash_hang11;      /* swi      */
  vec[11] = (uint32_t)crash_vec_pabt11;  /* prefetch */
  vec[12] = (uint32_t)crash_vec_dabt11;  /* data     */
  vec[13] = (uint32_t)crash_hang11;      /* reserved */
  vec[14] = (uint32_t)crash_hang11;      /* irq      */
  vec[15] = (uint32_t)crash_hang11;      /* fiq      */

  __asm__ volatile("mcr p15, 0, %0, c7, c10, 0" ::"r"(0)); /* clean D-cache  */
  __asm__ volatile("mcr p15, 0, %0, c7, c5, 0" ::"r"(0));  /* invalidate I   */
  __asm__ volatile("mcr p15, 0, %0, c7, c10, 4" ::"r"(0)); /* DSB            */
}

/* This core's MPCore private timer, used one-shot. Clock11.c borrows it and
 * puts it back. */
#define PTIMER_LOAD  0x17E00600u
#define PTIMER_COUNT 0x17E00604u
#define PTIMER_CNT   0x17E00608u
#define PTIMER_STAT  0x17E0060Cu
#define PTIMER_EN    (1u << 0)
#define PTIMER_EVENT (1u << 0)

/* The timer counts at half the CPU clock. Taking the New 3DS's 804 MHz keeps a
 * wait from ever being short; at 268 MHz it lasts three times as long, which
 * the codec settle delays allow. */
#define PTIMER_PER_MS (804000000u / 2u / 1000u)

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

/* Timed rather than counted: this core runs uncached, where how long a nop
 * loop takes is anyone's guess (docs/audio.md "A core's start-up"). */
void sleep_ms(uint32_t ms) {
  if (!ptimer_state)
    ptimer_state = ptimer_counts() ? 1 : -1;
  if (ptimer_state < 0) {
    spin(ms * 300000u);
    return;
  }
  while (ms) {
    uint32_t part = ms > 1000u ? 1000u : ms;
    ms -= part;
    MMIO32(PTIMER_CNT) = 0;
    MMIO32(PTIMER_STAT) = PTIMER_EVENT;
    MMIO32(PTIMER_LOAD) = part * PTIMER_PER_MS;
    MMIO32(PTIMER_CNT) = PTIMER_EN;
    /* One-shot: it stops at zero and flags the event. */
    while (!(MMIO32(PTIMER_STAT) & PTIMER_EVENT) && MMIO32(PTIMER_COUNT))
      ;
    MMIO32(PTIMER_CNT) = 0;
    MMIO32(PTIMER_STAT) = PTIMER_EVENT;
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
  ct->magic = 0;
  ct->ack_seq = ct->cmd_seq;
  dcache_clean();
  __asm__ volatile("mcr p15, 0, %0, c7, c5, 0" ::"r"(0)); /* invalidate I */
  ((void (*)(void))AUDIO_PARK_ADDR)();
}

void audio11_main(void) {
  AudioCtrl *ct = (AudioCtrl *)AUDIO_CTRL_ADDR;
  int poll_tick = 0;

  ct->status = AUDIO_ST_BOOT;
  ct->ack_seq = ct->cmd_seq;
  ct->version = AUDIO_CORE_VERSION;
  ct->diag0 = ct->diag1 = ct->diag2 = ct->diag3 = 0;
  ct->diag4 = ct->diag5 = ct->diag6 = ct->diag7 = 0;
  ct->socinfo = *(volatile uint32_t *)0x10140FFC; /* CFG11_SOCINFO */
  ct->magic = AUDIO_MAGIC;
  dcache_clean();

  /* Not called: writing the ARM11 vector page is unverified, and can hang the
   * core before its main loop. */
  (void)crash11_init;

  codec11_bus_init();
  audio11_init(ct);

  /* After audio is up, so a touch problem cannot stop audio from initialising. */
  touch11_init();

  for (;;) {
    dcache_inval_line((const void *)&ct->cmd_seq);
    if (ct->cmd_seq != ct->ack_seq) {
      dcache_clean_inval(); /* a command is here: pull everything it needs */
      uint32_t cmd = ct->cmd;
      uint32_t arg0 = ct->arg0;
      if (!audio11_command(ct, cmd, arg0)) {
        if (cmd == AUDIO_CMD_WIFI)
          wifi11_probe();
        else if (cmd == AUDIO_CMD_WIFI_BOOT)
          wifi11_boot(arg0);
        else if (cmd == AUDIO_CMD_GPU)
          gpu11_run();
        else if (cmd == AUDIO_CMD_N3DS)
          clock11_set(ct, arg0);
        else if (cmd == AUDIO_CMD_PARK)
          core11_park(ct);
      }
      ct->ack_seq = ct->cmd_seq;
      dcache_clean();
    }

    if (++poll_tick >= 16) {
      poll_tick = 0;
      touch11_poll();
    }
    stereo11_tick();
    spin(1200);
  }
}
