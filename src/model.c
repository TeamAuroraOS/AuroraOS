/* CFG11_SOCINFO bit 1 marks the New 3DS family. The register is ARM11 config
 * space, read once by Core11.c into AudioCtrl.socinfo; the ARM9 is not used for
 * it because it has data-aborted on ARM11 registers before. */

#include "model.h"
#include "audio.h"
#include "gpu.h"
#include "timer.h"

#define SOCINFO_NEW3DS 0x2u

u32 aurora_socinfo(void) {
  const AudioCtrl *ct = (const AudioCtrl *)AUDIO_CTRL_ADDR;
  /* Zero until the ARM11 core has started and published it. */
  return (ct->magic == AUDIO_MAGIC) ? ct->socinfo : 0u;
}

AuroraModel aurora_model(void) {
  return (aurora_socinfo() & SOCINFO_NEW3DS) ? AURORA_MODEL_NEW
                                             : AURORA_MODEL_OLD;
}

int aurora_is_new3ds(void) { return aurora_model() == AURORA_MODEL_NEW; }

const char *aurora_model_tag(void) {
  return aurora_is_new3ds() ? "N" : "";
}

/* CFG11_SOCINFO bit 2 marks the retail New 3DS SoC (LGR2), the one GBATEK says
 * may use 804 MHz. Bit 1 alone is the LGR1 prototype, which tops out at 536. */
#define SOCINFO_LGR2   0x4u
#define CLKCNT_MODE_804 5u
#define CLKCNT_MODE_536 3u

extern void os_cache_sync(void);
extern void delay(volatile u32 cycles);

AuroraN3dsHw aurora_n3ds_hardware(void) {
  AudioCtrl *ct = (AudioCtrl *)AUDIO_CTRL_ADDR;
  u32 info = aurora_socinfo();
  u32 t0, spins = 0;

  if (!(info & SOCINFO_NEW3DS))
    return AURORA_N3DS_HW_OLD;

  /* The command block holds one request at a time, and a posted GPU blit uses
   * it too: overwriting that would lose the blit and stall its completion. */
  gpu_wait_idle();

  os_cache_sync();
  if (ct->magic != AUDIO_MAGIC)
    return AURORA_N3DS_HW_NO_CORE;
  ct->n3ds_status = AUDIO_N3DS_PENDING;
  ct->cmd = AUDIO_CMD_N3DS;
  ct->arg0 = (info & SOCINFO_LGR2) ? CLKCNT_MODE_804 : CLKCNT_MODE_536;
  ct->cmd_seq = ct->cmd_seq + 1;
  os_cache_sync();

  /* The ARM11 waits about a second at most; three seconds only guards against a
   * core that has stopped answering. */
  t0 = timer_ticks();
  for (;;) {
    os_cache_sync();
    if (ct->ack_seq == ct->cmd_seq)
      break;
    if (timer_calibrated() ? timer_us_since(t0) > 3000000u : ++spins > 30000u)
      return AURORA_N3DS_HW_NO_CORE;
    delay(2000);
  }

  switch (ct->n3ds_status) {
    case AUDIO_N3DS_APPLIED: return AURORA_N3DS_HW_APPLIED;
    case AUDIO_N3DS_ALREADY: return AURORA_N3DS_HW_ALREADY;
    case AUDIO_N3DS_NO_WAKE: return AURORA_N3DS_HW_NO_IRQ;
    default:                 return AURORA_N3DS_HW_REFUSED;
  }
}

void aurora_n3ds_clock(u32 *before, u32 *after) {
  const AudioCtrl *ct = (const AudioCtrl *)AUDIO_CTRL_ADDR;
  os_cache_sync();
  *before = ct->n3ds_before;
  *after = ct->n3ds_after;
}

const char *aurora_n3ds_hw_text(AuroraN3dsHw r) {
  switch (r) {
    case AURORA_N3DS_HW_APPLIED: return "New 3DS: fast clock mode on";
    case AURORA_N3DS_HW_ALREADY: return "New 3DS: fast clock was already on";
    case AURORA_N3DS_HW_REFUSED: return "New 3DS: clock switch did not take";
    case AURORA_N3DS_HW_NO_CORE: return "New 3DS: ARM11 core did not answer";
    case AURORA_N3DS_HW_NO_IRQ:  return "New 3DS: no wake-up IRQ, not switched";
    default:                     return "Old 3DS: standard clock";
  }
}
