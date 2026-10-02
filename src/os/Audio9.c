#include "aurora.h"
#include "audio.h"
#include "timer.h"
#include "audio11_blob.h" /* generated: audio11_bin[], audio11_bin_len */

/* Clean+invalidate the ARM9 caches: pushes writes out for the ARM11 and CSND,
 * and re-reads the ARM11's replies. */
extern void os_cache_sync(void);

static AudioCtrl *const ctrl = (AudioCtrl *)AUDIO_CTRL_ADDR;

/* A core sets its magic before it initialises the codec and touchscreen and
 * only takes requests after, so audio_ready() posts a request and waits for
 * the answer. */
static int ready, probing;
static u32 probe_seq, woken_at, ready_ms;

static void sync_line(const volatile void *p) {
  __asm__ volatile("mcr p15, 0, %0, c7, c14, 1" ::"r"(p) : "memory");
  __asm__ volatile("mcr p15, 0, %0, c7, c10, 4" ::"r"(0) : "memory");
}

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

static void post(uint32_t cmd, uint32_t arg0);

/* Polls of the command block before a core counts as gone, each a cache sync
 * and a short delay: long enough for the touch poll the core does between
 * commands. */
#define AUDIO_ACK_POLLS 2000

/* Posts a command and waits for the core to take it. 0 means nothing did. */
static int post_wait(uint32_t cmd, uint32_t arg0) {
  post(cmd, arg0);
  for (int t = 0; t < AUDIO_ACK_POLLS; t++) {
    os_cache_sync();
    if (ctrl->ack_seq == ctrl->cmd_seq)
      return 1;
    delay(2000);
  }
  return 0;
}

AudioBoot audio_boot(void) {
  AudioBoot result = AUDIO_BOOT_LOADED;

  os_cache_sync();
  /* FCRAM keeps this block through a reboot that resets the ARM11, so the magic
   * alone does not prove a core is running: it also has to answer. */
  if (ctrl->magic == AUDIO_MAGIC && post_wait(AUDIO_CMD_NONE, 0)) {
    if (ctrl->version == AUDIO_CORE_VERSION) {
      ready = 1;
      return AUDIO_BOOT_RUNNING;
    }
    if (ctrl->version < AUDIO_PARK_VERSION || !post_wait(AUDIO_CMD_PARK, 0))
      return AUDIO_BOOT_STALE;
    /* Once the park is acknowledged the core is leaving its image whatever
     * happens next, so a missing report only costs the wait. */
    for (int t = 0; t < AUDIO_ACK_POLLS; t++) {
      os_cache_sync();
      if (ctrl->status == AUDIO_ST_PARKED)
        break;
      delay(2000);
    }
    result = AUDIO_BOOT_REPLACED;
  }

  ctrl->magic = 0;
  ctrl->status = AUDIO_ST_NONE;
  ctrl->cmd_seq = 0;
  ctrl->ack_seq = 0;
  ctrl->cmd = AUDIO_CMD_NONE;

  volatile unsigned char *dst = (volatile unsigned char *)AUDIO_CORE_ADDR;
  for (unsigned i = 0; i < audio11_bin_len; i++)
    dst[i] = audio11_bin[i];

  os_cache_sync();

  /* Wake the ARM11: the firm stub is spinning on this mailbox. */
  *(volatile uint32_t *)AUDIO_ARM11_MAILBOX = AUDIO_CORE_ADDR;
  os_cache_sync();
  woken_at = timer_ticks();
  ready = probing = 0;

  for (int t = 0; t < 200; t++) {
    os_cache_sync();
    if (ctrl->magic == AUDIO_MAGIC)
      return result;
    delay(200000);
  }
  return AUDIO_BOOT_FAILED;
}

static void post(uint32_t cmd, uint32_t arg0) {
  os_cache_sync();
  ctrl->cmd = cmd;
  ctrl->arg0 = arg0;
  ctrl->cmd_seq = ctrl->cmd_seq + 1;
  os_cache_sync();
}

int audio_ready(void) {
  if (ready)
    return 1;
  sync_line(&ctrl->magic);
  sync_line(&ctrl->ack_seq);
  if (ctrl->magic != AUDIO_MAGIC)
    return 0;
  if (!probing) {
    post(AUDIO_CMD_NONE, 0);
    probe_seq = ctrl->cmd_seq;
    probing = 1;
    return 0;
  }
  if ((int32_t)(ctrl->ack_seq - probe_seq) < 0)
    return 0;
  ready = 1;
  ready_ms = timer_us_since(woken_at) / 1000u;
  return 1;
}

int audio_wait_ready(uint32_t ms) {
  u32 t0 = timer_ticks();
  while (!audio_ready())
    if (timer_us_since(t0) >= ms * 1000u)
      return 0;
  return 1;
}

int audio_ready_ms(void) {
  if (!ready)
    return -1;
  return (int)ready_ms;
}

void audio_play_tone(uint32_t freq_hz) { post(AUDIO_CMD_TONE, freq_hz); }

void audio_stop(void) { post(AUDIO_CMD_STOP, 0); }

void audio_error_beep(void) { post(AUDIO_CMD_ERROR, 0); }

void audio_voices_stop(void) {
  if (!audio_alive())
    return;
  /* The GPU is about to use the one-request block, so wait until the core has
   * taken this request. An older core acks it without acting on it. */
  post_wait(AUDIO_CMD_VOICE_STOP, AUDIO_VOICE_ALL);
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
