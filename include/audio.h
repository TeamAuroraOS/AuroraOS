/* ARM9 <-> ARM11 audio contract: the ARM9 posts commands into a shared FCRAM
 * block and the ARM11 core services them. See docs/audio.md. */
#ifndef AURORA_AUDIO_H
#define AURORA_AUDIO_H

#include <stdint.h>

/* Shared FCRAM, clear of the OS image (0x22000000..0x23000000) and the
 * app-launch staging area (0x24000000+). */
#define AUDIO_CORE_ADDR     0x23000000u /* ARM11 core code (load + entry)      */
#define AUDIO_CTRL_ADDR     0x23300000u /* AudioCtrl command block             */
#define AUDIO_PCM_ADDR      0x23400000u /* PCM sample buffer                   */
#define AUDIO_ARM11_MAILBOX 0x27000000u /* firm ARM11 wake mailbox (loader.h)  */

#define AUDIO_MAGIC 0x4F494441u /* 'ADIO': set by the core once it is alive */

/* FCRAM survives a warm reboot, so a stale core keeps running unless the
 * console is fully powered off. Bump this whenever the core changes. */
#define AUDIO_CORE_VERSION 82

#define AUDIO_PCM_MAX (10u * 1024u * 1024u) /* longer tracks are truncated */

/* Crash beep, rendered at boot so the ARM11 fault stub can start it without a
 * stack. audio11_start.s hard-codes these values; change both together. */
#define AUDIO_ERR_ADDR    0x233D0000u /* clear of GpuShared and the PCM buffer */
#define AUDIO_ERR_RATE    8000u
#define AUDIO_ERR_FREQ    880u
#define AUDIO_ERR_BEEP    600u /* 75 ms */
#define AUDIO_ERR_GAP     400u /* 50 ms */
#define AUDIO_ERR_SAMPLES (AUDIO_ERR_BEEP * 3u + AUDIO_ERR_GAP * 2u)
#define AUDIO_ERR_BYTES   (AUDIO_ERR_SAMPLES * 2u)

/* The core acks a command by copying cmd_seq into ack_seq. */
enum {
  AUDIO_CMD_NONE = 0,
  AUDIO_CMD_TONE = 1,      /* arg0 = frequency in Hz, looped                  */
  AUDIO_CMD_STOP = 2,
  AUDIO_CMD_PCM  = 3,      /* arg1 = samples, arg2 = rate, arg3 = bit depth   */
  AUDIO_CMD_WIFI = 4,      /* SDIO probe, results in WifiShared               */
  AUDIO_CMD_WIFI_BOOT = 5, /* upload the SD-staged firmware and boot the chip */
  AUDIO_CMD_GPU = 6,       /* params and results in GpuShared                 */
  AUDIO_CMD_ERROR = 7,     /* the three-beep crash tone                       */
  AUDIO_CMD_N3DS  = 8,     /* arg0 = CLKCNT mode to request; see n3ds_*       */
  AUDIO_CMD_VOICE = 9,     /* arg0 = voice | flags, arg1 = address,           */
                           /* arg2 = bytes, arg3 = rate; see AUDIO_VOICE_*    */
  AUDIO_CMD_VOICE_STOP = 10, /* arg0 = bit mask of the voices to stop       */
};

enum {
  AUDIO_ST_NONE  = 0,
  AUDIO_ST_BOOT  = 1,
  AUDIO_ST_CODEC = 2,
  AUDIO_ST_READY = 3,
  AUDIO_ST_PLAY  = 4,
  AUDIO_ST_IDLE  = 5,
};

typedef struct {
  volatile uint32_t magic;
  volatile uint32_t status;  /* AUDIO_ST_*                        */
  volatile uint32_t cmd_seq; /* ARM9 bumps this to post a command */
  volatile uint32_t ack_seq;
  volatile uint32_t cmd;
  volatile uint32_t arg0;
  volatile uint32_t arg1;
  volatile uint32_t arg2;
  volatile uint32_t arg3;
  volatile uint32_t version;
  /* Bring-up state the core records while initialising:
   *  0 codec IDs (all 0x00 or 0xFF means the codec SPI is not talking)
   *  1 SPI busy-wait timeouts (0 = every transfer completed)
   *  2 CSND master control readback    3 CSND ch0 control after a tone
   *  4 CFG11 SPI enable readback       5 NSPI control after a transfer
   *  6 codec write-then-read verify (wrote 0x2A)   7 reserved */
  volatile uint32_t diag0;
  volatile uint32_t diag1;
  volatile uint32_t diag2;
  volatile uint32_t diag3;
  volatile uint32_t diag4;
  volatile uint32_t diag5;
  volatile uint32_t diag6;
  volatile uint32_t diag7;
  /* CFG11_SOCINFO, read once by the ARM11; the ARM9 cannot read that register. */
  volatile uint32_t socinfo;
  /* AUDIO_CMD_N3DS: CFG11_MPCORE_CLKCNT as read before and after the attempt,
   * and the AUDIO_N3DS_* outcome. */
  volatile uint32_t n3ds_before;
  volatile uint32_t n3ds_after;
  volatile uint32_t n3ds_status;
} AudioCtrl;

typedef enum {
  AUDIO_N3DS_PENDING     = 0,
  AUDIO_N3DS_APPLIED     = 1, /* the requested mode is now current           */
  AUDIO_N3DS_ALREADY     = 2, /* it already was; nothing was done            */
  AUDIO_N3DS_REFUSED     = 3, /* woke on the safety timer, mode unchanged    */
  AUDIO_N3DS_UNSUPPORTED = 4, /* SOCINFO does not allow the requested mode   */
  AUDIO_N3DS_NO_WAKE     = 5, /* no interrupt reached the core; not tried    */
} AudioN3dsStatus;

/* Voices: sounds an app plays, each on its own CSND channel (1..AUDIO_VOICES),
 * so music and effects overlap. Channel 0 stays with the OS's tone, PCM and
 * crash beep. The samples are read in place, so they must be in physical RAM
 * (flushed from the ARM9 cache) and stay put while the voice plays.
 *
 * AUDIO_CMD_VOICE arg0: bits 0-7 the voice, or AUDIO_VOICE_ANY for whichever
 * of voices 1..7 is free (voice 0 is never picked, so a caller can keep it for
 * music); bit 8 loop; bit 9 16-bit samples (else 8-bit); bits 16-31 volume,
 * 0x8000 being full. Samples are signed mono. */
#define AUDIO_VOICES        8u
#define AUDIO_VOICE_ANY     0xFFu
#define AUDIO_VOICE_ALL     0xFFu /* AUDIO_CMD_VOICE_STOP: every voice */
#define AUDIO_VOICE_LOOP    (1u << 8)
#define AUDIO_VOICE_PCM16   (1u << 9)
#define AUDIO_VOICE_VOL(v)  (((v) & 0xFFFFu) << 16)
#define AUDIO_VOICE_VERSION 82 /* the first core that understands voices */

/* Copies the core into place and wakes the ARM11. Idempotent, and blocks
 * briefly for the handshake. */
void audio_boot(void);

int audio_alive(void);
uint32_t audio_status(void);
uint32_t audio_version(void);
uint32_t audio_diag(int idx); /* idx 0..7 */

void audio_play_tone(uint32_t freq_hz);
void audio_stop(void);

/* Only posts a command, so it is safe from the ARM9 fault handler. Does
 * nothing when the ARM11 is what died; that core's fault stub starts the same
 * sound itself. */
void audio_error_beep(void);

/* The caller must already have written the samples to AUDIO_PCM_ADDR, within
 * AUDIO_PCM_MAX. depth is 8 or 16. */
void audio_play_pcm(uint32_t samples, uint32_t rate, uint32_t depth);

/* Stops every app voice and waits for the core to take the request. */
void audio_voices_stop(void);

#endif
