/* Core 0's part of video playback: every pass of its loop it publishes the clock
 * and does what VideoMail asks (include/video.h). A mail rather than a command,
 * so a frame or a sound is never held up by, and never overwrites, whatever the
 * ARM9 has in the one command slot. */
#include "core11.h"
#include "video.h"

void gpu11_video_tick(VideoMail *m);
void audio11_stream(uint32_t left, uint32_t right, uint32_t bytes, uint32_t rate,
                    uint32_t vol);

static VideoMail *const m = (VideoMail *)VIDEO_MAIL_ADDR;
static int live;

void video11_tick(void) {
  if (m->magic != VIDEO_MAIL_MAGIC) {
    if (live) {
      live = 0;
      audio11_stream(0, 0, 0, 0, 0);
    }
    return;
  }
  live = 1;
  uint32_t now = core11_now_us();
  m->now_us = now;
  if (m->audio_seen != m->audio_seq) {
    uint32_t seq = m->audio_seq;
    audio11_stream(m->audio_left, m->audio_right, m->audio_bytes, m->audio_rate,
                   m->audio_vol);
    m->audio_start_us = core11_now_us();
    m->audio_seen = seq;
  }
  gpu11_video_tick(m);
}

void video11_reset(void) {
  m->magic = 0;
  dsb();
  video11_tick();
}

/* At the core's start: seq and done must agree, whatever FCRAM kept. */
void video11_init(void) {
  volatile uint32_t *w = (volatile uint32_t *)m;
  for (uint32_t i = 0; i < sizeof(*m) / 4u; i++)
    w[i] = 0;
  dsb();
}
