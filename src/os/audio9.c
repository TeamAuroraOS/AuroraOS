/*
 * AuroraOS audio -- ARM9 side.
 *
 * Starts the ARM11 audio core and posts commands to it through the shared block
 * at AUDIO_CTRL_ADDR. The ARM11 core binary is embedded here as a byte blob
 * (build/audio11_blob.h, generated from audio11.bin) so the OS can copy it into
 * place and wake the ARM11 itself -- no firm changes and cache coherency stays
 * under the OS's control via os_cache_sync().
 */
#include "aurora.h"
#include "audio.h"
#include "audio11_blob.h" /* generated: audio11_bin[], audio11_bin_len */
#include "wifi.h"

/* Clean+invalidate the ARM9 caches (src/os/os_launch.s). Used both to push our
 * writes to physical RAM for the ARM11/CSND and to re-read the ARM11's replies. */
extern void os_cache_sync(void);

static AudioCtrl *const ctrl = (AudioCtrl *)AUDIO_CTRL_ADDR;

int audio_alive(void) {
  os_cache_sync();
  return ctrl->magic == AUDIO_MAGIC;
}

uint32_t audio_status(void) {
  os_cache_sync();
  return ctrl->status;
}

uint32_t audio_version(void) {
  os_cache_sync();
  return ctrl->version;
}

uint32_t audio_diag(int idx) {
  os_cache_sync();
  switch (idx) {
    case 0:  return ctrl->diag0;
    case 1:  return ctrl->diag1;
    case 2:  return ctrl->diag2;
    case 3:  return ctrl->diag3;
    case 4:  return ctrl->diag4;
    case 5:  return ctrl->diag5;
    case 6:  return ctrl->diag6;
    default: return ctrl->diag7;
  }
}

void audio_boot(void) {
  os_cache_sync();
  if (ctrl->magic == AUDIO_MAGIC)
    return; /* core already running (e.g. after a HOME return) */

  /* Reset the command block. */
  ctrl->magic = 0;
  ctrl->status = AUDIO_ST_NONE;
  ctrl->cmd_seq = 0;
  ctrl->ack_seq = 0;
  ctrl->cmd = AUDIO_CMD_NONE;

  /* Copy the ARM11 core into place. */
  volatile unsigned char *dst = (volatile unsigned char *)AUDIO_CORE_ADDR;
  for (unsigned i = 0; i < audio11_bin_len; i++)
    dst[i] = audio11_bin[i];

  os_cache_sync(); /* flush core code + cleared block to physical RAM */

  /* Wake the ARM11: the firm stub is spinning on this mailbox. */
  *(volatile uint32_t *)AUDIO_ARM11_MAILBOX = AUDIO_CORE_ADDR;
  os_cache_sync();

  /* Wait (bounded) for the core to report alive. */
  for (int t = 0; t < 200; t++) {
    os_cache_sync();
    if (ctrl->magic == AUDIO_MAGIC)
      break;
    delay(200000);
  }
}

static void post(uint32_t cmd, uint32_t arg0) {
  os_cache_sync();
  ctrl->cmd = cmd;
  ctrl->arg0 = arg0;
  ctrl->cmd_seq = ctrl->cmd_seq + 1;
  os_cache_sync(); /* publish the command to the ARM11 */
}

void audio_play_tone(uint32_t freq_hz) { post(AUDIO_CMD_TONE, freq_hz); }

void audio_stop(void) { post(AUDIO_CMD_STOP, 0); }

void wifi_probe(void) {
  os_cache_sync();
  ctrl->cmd = AUDIO_CMD_WIFI;
  ctrl->cmd_seq = ctrl->cmd_seq + 1;
  os_cache_sync();
}

void wifi_get(WifiShared *out) {
  os_cache_sync(); /* pull the shared block fresh from RAM */
  volatile unsigned char *s = (volatile unsigned char *)WIFI_SHARED_ADDR;
  unsigned char *d = (unsigned char *)out;
  for (unsigned i = 0; i < sizeof(WifiShared); i++)
    d[i] = s[i];
  out->sdmmcctl = *(volatile uint16_t *)0x10000020u; /* CFG9 SDMMCCTL (ARM9) */
}

void audio_play_pcm(uint32_t samples, uint32_t rate, uint32_t depth) {
  os_cache_sync(); /* flush the caller's PCM writes at AUDIO_PCM_ADDR to RAM */
  ctrl->cmd = AUDIO_CMD_PCM;
  ctrl->arg0 = AUDIO_PCM_ADDR;
  ctrl->arg1 = samples;
  ctrl->arg2 = rate;
  ctrl->arg3 = depth;
  ctrl->cmd_seq = ctrl->cmd_seq + 1;
  os_cache_sync();
}
