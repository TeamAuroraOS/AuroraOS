/*
 * ARM11 core: entry point and command loop.
 *
 * The firm leaves the ARM11 spinning on the wake mailbox; the ARM9 OS copies
 * this core to AUDIO_CORE_ADDR and wakes it (see include/audio.h). From there
 * this file owns the command loop and hands each request to the subsystem that
 * owns it: Audio11, Touch11, WiFi11, Gpu11.
 */
#include "core11.h"
#include "touch.h"
#include "wifi.h"

/* ARM11 exception stubs live in audio11_start.s. Install them so a fault in the
 * audio core is captured into the cross-core crash block for the ARM9 to show.
 * Best effort: assumes the ARM11 vector page is writable (like the audio
 * bring-up, this is untested register territory). */
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


void audio11_main(void) {
  AudioCtrl *ct = (AudioCtrl *)AUDIO_CTRL_ADDR;
  int poll_tick = 0;

  ct->status = AUDIO_ST_BOOT;
  ct->ack_seq = ct->cmd_seq;
  ct->version = AUDIO_CORE_VERSION;
  ct->diag0 = ct->diag1 = ct->diag2 = ct->diag3 = 0;
  ct->diag4 = ct->diag5 = ct->diag6 = ct->diag7 = 0;
  ct->magic = AUDIO_MAGIC;
  dcache_clean();

  /* NOTE: crash11_init() (ARM11 exception-vector install) is DISABLED for now.
   * Its vector write is untested register territory and is the prime suspect
   * for hanging the core before the main loop (seq stuck at 0, no audio). Bring
   * it back only once the vector page is confirmed writable on ARM11. */
  (void)crash11_init;

  codec11_bus_init();
  audio11_init(ct);

  /* Configure the codec touchscreen ADC after audio is fully up, so a problem
   * here can't stop audio/CSND from initialising. */
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
          wifi11_boot();
        else if (cmd == AUDIO_CMD_GPU)
          gpu11_run();
      }
      ct->ack_seq = ct->cmd_seq;
      dcache_clean();
    }

    /* Sample the touchscreen at its old rate, but keep looking for work in
     * between, so a request is picked up in microseconds rather than after a
     * whole touch interval. */
    if (++poll_tick >= 16) {
      poll_tick = 0;
      touch11_poll();
    }
    spin(1200);
  }
}
