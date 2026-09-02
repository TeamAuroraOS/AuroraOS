/*
 * AuroraOS audio: ARM9 <-> ARM11 shared contract.
 *
 * The 3DS sound hardware (CSND mixer at 0x10103000, SNDEXCNT at 0x10145000, and
 * the audio codec) lives on the ARM11, but the Aurora OS runs on the ARM9. So
 * audio is handled by a small ARM11 "audio core" (src/os/audio11.c) that the
 * ARM9 OS starts and then drives through a command block in shared FCRAM.
 *
 * Boot path (see audio9.c): the firm leaves the ARM11 spinning on the wake
 * mailbox (src/arm11_start.s). The ARM9 OS copies the audio core to
 * AUDIO_CORE_ADDR, flushes its cache, and writes the core's entry to the
 * mailbox -- waking the ARM11 into the core. No firm changes required.
 *
 * NOTE: the CSND/codec register programming in audio11.c is a documented best
 * effort (3dbrew / GBATEK / ctrulib) and has not been validated on hardware yet.
 */
#ifndef AURORA_AUDIO_H
#define AURORA_AUDIO_H

#include <stdint.h>

/* --- Shared FCRAM layout (physical). Clear of the ARM9 OS image + stack
 *     (0x22000000..0x23000000) and the app-launch staging area (0x24000000+). */
#define AUDIO_CORE_ADDR     0x23000000u /* ARM11 audio core code (load + entry) */
#define AUDIO_CTRL_ADDR     0x23300000u /* AudioCtrl command block              */
#define AUDIO_PCM_ADDR      0x23400000u /* PCM sample buffer (core-owned)       */
#define AUDIO_ARM11_MAILBOX 0x27000000u /* firm ARM11 wake mailbox (loader.h)   */

/* Set by the ARM11 core once it is alive ('ADIO', little-endian). */
#define AUDIO_MAGIC 0x4F494441u

/* Bumped every time the ARM11 core changes, so the OS can tell whether the
 * resident core is current. FCRAM survives a warm reboot, so a stale core can
 * linger unless the console is fully powered off -- the Sound Test screen shows
 * this version so a mismatch is visible. */
#define AUDIO_CORE_VERSION 11

/* Max PCM the shared buffer holds (10 MB, clear of the app-stage at 0x24000000).
 * Longer tracks are truncated to this. */
#define AUDIO_PCM_MAX (10u * 1024u * 1024u)

/* Commands the ARM9 posts. The core acks by copying cmd_seq -> ack_seq. */
enum {
  AUDIO_CMD_NONE = 0,
  AUDIO_CMD_TONE = 1, /* play the built-in test tone (arg0 = frequency in Hz) */
  AUDIO_CMD_STOP = 2, /* silence the channel                                  */
  AUDIO_CMD_PCM  = 3, /* play PCM already loaded at AUDIO_PCM_ADDR:
                       * arg1 = sample count, arg2 = sample rate (Hz),
                       * arg3 = bit depth (8 or 16). One-shot.               */
};

/* ARM11 progress codes, surfaced on-screen to debug the bring-up. */
enum {
  AUDIO_ST_NONE  = 0,
  AUDIO_ST_BOOT  = 1, /* core entered                    */
  AUDIO_ST_CODEC = 2, /* codec unmute attempted          */
  AUDIO_ST_READY = 3, /* CSND enabled, waiting for a cmd */
  AUDIO_ST_PLAY  = 4, /* a tone is playing               */
  AUDIO_ST_IDLE  = 5, /* stopped                         */
};

typedef struct {
  volatile uint32_t magic;   /* AUDIO_MAGIC once the core is alive */
  volatile uint32_t status;  /* AUDIO_ST_*                         */
  volatile uint32_t cmd_seq; /* ARM9 bumps this to post a command  */
  volatile uint32_t ack_seq; /* core sets = cmd_seq when handled   */
  volatile uint32_t cmd;     /* AUDIO_CMD_*                        */
  volatile uint32_t arg0;
  volatile uint32_t arg1;
  volatile uint32_t arg2;
  volatile uint32_t arg3;
  volatile uint32_t version; /* AUDIO_CORE_VERSION of the resident core */
  /* Bring-up diagnostics filled by the core (shown on the Sound Test screen):
   *  diag0 = codec IDs: reg0.2 | reg0.3<<8 | (101.11 readback)<<16
   *          (all-0x00 or all-0xFF => codec SPI is not talking)
   *  diag1 = SPI busy-wait timeout count (0 = all transfers completed)
   *  diag2 = CSND master-control readback (expect ~0xC0008000)
   *  diag3 = CSND ch0 control readback after a tone (expect bit15 set)
   *  diag4 = CFG11 SPI-enable register readback (expect 0x0007)
   *  diag5 = NSPI codec-bus control-register readback after a transfer
   *  diag6 = codec write-then-read-back verify (wrote 0x2A; expect it back)
   *  diag7 = reserved */
  volatile uint32_t diag0;
  volatile uint32_t diag1;
  volatile uint32_t diag2;
  volatile uint32_t diag3;
  volatile uint32_t diag4;
  volatile uint32_t diag5;
  volatile uint32_t diag6;
  volatile uint32_t diag7;
} AudioCtrl;

/* ---- ARM9-side API (src/os/audio9.c) ---- */

/* Copy the ARM11 core into place and wake the ARM11. Idempotent: does nothing
 * if the core is already reporting alive. Blocks briefly for the handshake. */
void audio_boot(void);

/* 1 if the ARM11 core reported alive (its magic is present in the block). */
int audio_alive(void);

/* Latest ARM11 status code (AUDIO_ST_*). */
uint32_t audio_status(void);

/* Version of the resident ARM11 core (compare with AUDIO_CORE_VERSION). */
uint32_t audio_version(void);

/* Bring-up diagnostic word (idx 0..3); see AudioCtrl. */
uint32_t audio_diag(int idx);

/* Post commands to the ARM11 core. */
void audio_play_tone(uint32_t freq_hz);
void audio_stop(void);

/* Play PCM already written to AUDIO_PCM_ADDR (caller loaded + is responsible for
 * the data being <= AUDIO_PCM_MAX). depth is 8 or 16. Flushes the buffer to RAM
 * for CSND, then posts the play command. */
void audio_play_pcm(uint32_t samples, uint32_t rate, uint32_t depth);

#endif /* AURORA_AUDIO_H */
