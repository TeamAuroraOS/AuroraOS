#ifndef AURORA_VIDEO_H
#define AURORA_VIDEO_H

#include <stdint.h>

/* Video playback, split three ways (docs/video.md). The app on the ARM9 fetches
 * and demuxes, and fills the rings below. The engine (sdk/video11, FFmpeg's
 * decoders) runs on the other ARM11 cores through AUDIO_CMD_SMP. Core 0, which
 * alone touches the GPU and CSND, shows finished frames and runs the sound's two
 * channels when VideoMail asks, without a command, so nothing waits for the one
 * command slot. */

#define AUDIO_VIDEO_VERSION 125 /* the first core that reads VideoMail */

/* Strongly ordered for every ARM11 core, so it needs no cache handling there. */
#define VIDEO_MAIL_ADDR 0x233E8000u
#define VIDEO_MAIL_MAGIC 0x4C49414Du /* 'MAIL' */

typedef struct {
  volatile uint32_t magic;       /* the engine: this block is live */
  volatile uint32_t now_us;      /* core 0: its clock, every pass of its loop */
  /* A frame in FCRAM for the top screen, as BGR in framebuffer layout. */
  volatile uint32_t present_seq; /* the engine bumps this... */
  volatile uint32_t present_src; /* ...after setting this */
  volatile uint32_t present_done; /* core 0: the last seq on screen */
  /* The sound: two looping mono rings, one a side. */
  volatile uint32_t audio_seq;   /* the engine bumps this after the fields below */
  volatile uint32_t audio_left, audio_right, audio_bytes, audio_rate;
  volatile uint32_t audio_vol;   /* 0x8000 is full */
  volatile uint32_t audio_seen;  /* core 0: the last seq it acted on */
  volatile uint32_t audio_start_us; /* core 0: now_us as the channels started */
} VideoMail;

/* FCRAM an app has to itself while its engine runs. The shared part is not
 * cached by the engine; the ARM9 cleans what it writes and invalidates what it
 * reads. HOME's snapshot at 0x26000000 stays clear of all of it. */
#define VIDEO_SHARED_ADDR 0x26100000u
#define VIDEO_VRING_ADDR  0x26200000u
#define VIDEO_VRING_SIZE  0x00600000u
#define VIDEO_ARING_ADDR  0x26800000u
#define VIDEO_ARING_SIZE  0x00080000u
#define VIDEO_PCM_L_ADDR  0x26880000u
#define VIDEO_PCM_R_ADDR  0x268C0000u
#define VIDEO_PCM_BYTES   0x00040000u
#define VIDEO_FRAMES      3u
#define VIDEO_FRAME_ADDR(i) (0x26900000u + (uint32_t)(i) * 0x50000u)
#define VIDEO_FRAME_BYTES (400u * 240u * 3u)
#define VIDEO_SHARED_FIRST_MB 0x261u
#define VIDEO_SHARED_MBS      10u

/* The engine, linked to run here (video11.ld): code and data, the cores' stacks,
 * then its heap, cached and coherent between the cores. */
#define VIDEO_ENGINE_ADDR 0x27100000u
#define VIDEO_ENGINE_MAX  0x00300000u
#define VIDEO_STACK_TOP(core) (0x27400000u + (uint32_t)(core) * 0x40000u)
#define VIDEO_HEAP_ADDR   0x27500000u
#define VIDEO_HEAP_END_OLD 0x28000000u
#define VIDEO_HEAP_END_NEW 0x2C000000u
#define VIDEO_ENGINE_FIRST_MB 0x271u

#define VIDEO_MAGIC 0x31444956u /* 'VID1' */

/* In a ring: this, then `size` bytes, then padding to a word. A size of
 * VIDEO_PKT_WRAP says the next packet is at the ring's start. */
typedef struct {
  uint32_t size;
  int32_t pts_ms;
  uint32_t flags;
} VideoPkt;

#define VIDEO_PKT_WRAP 0xFFFFFFFFu
#define VIDEO_PKT_KEY  1u

#define VIDEO_EXTRA_MAX 512u

enum {
  VIDEO_ST_NONE = 0,
  VIDEO_ST_STARTING, /* the engine runs, and is setting itself up */
  VIDEO_ST_READY,    /* takes a configuration */
  VIDEO_ST_PLAYING,  /* decoders open */
  VIDEO_ST_FAILED,   /* VideoShared.error says why */
};

enum {
  VIDEO_ERR_NONE = 0,
  VIDEO_ERR_NOMEM,
  VIDEO_ERR_VIDEO_OPEN,
  VIDEO_ERR_AUDIO_OPEN,
  VIDEO_ERR_FORMAT, /* decoded to something other than 8-bit 4:2:0 */
};

/* Each side writes only its own part, a whole number of the ARM9's 32-byte cache
 * lines, since a line both wrote would be written back over the other's words. */
typedef struct {
  uint32_t magic;

  /* The ARM9's. */
  uint32_t stop;       /* the engine returns */
  uint32_t paused;     /* no sound, and the clock holds */
  uint32_t config_seq; /* bumped once the fields below describe a new stream */
  uint32_t flush_seq;  /* the rings emptied, the decoders flushed, a new pts */
  int32_t flush_pts_ms;
  uint32_t vring_write, aring_write;
  uint32_t video_w, video_h;
  uint32_t audio_rate, audio_channels;
  uint32_t video_extra_len, audio_extra_len;
  uint32_t threads;    /* frame threads wanted: 1 on one core */
  uint32_t end_of_stream;
  uint8_t video_extra[VIDEO_EXTRA_MAX];
  uint8_t audio_extra[VIDEO_EXTRA_MAX];
  uint32_t arm9_pad[8];

  /* The engine's, on a line of its own. */
  __attribute__((aligned(32))) uint32_t state; /* VIDEO_ST_* */
  uint32_t error;
  uint32_t config_ack, flush_ack;
  uint32_t vring_read, aring_read;
  int32_t clock_ms;    /* the media time on the speakers now */
  uint32_t heartbeat;  /* moves while the engine's main loop does */
  uint32_t frames_decoded, frames_shown, frames_dropped;
  uint32_t audio_underruns;
  uint32_t decode_us;  /* time spent decoding, for the speed readout */
  uint32_t convert_us;
  uint32_t ended;      /* everything sent has been shown and played */
  uint32_t cores_used;
} VideoShared;

#endif
