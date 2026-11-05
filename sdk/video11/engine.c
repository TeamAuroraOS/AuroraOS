/* The video engine: FFmpeg's H.264 and AAC decoders on the ARM11 cores other than
 * core 0, as a job started by AUDIO_CMD_SMP. include/video.h has the split of the
 * work, docs/video.md the reasoning.
 *
 * The first core runs this file's loop: it feeds the decoders, converts frames for
 * the screen and keeps the time. Any other cores wait in threads11.c to run
 * FFmpeg's frame threads. */
#include <stdint.h>
#include <string.h>

#include "libavcodec/avcodec.h"
#include "libavutil/channel_layout.h"
#include "libavutil/frame.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"

#include "engine.h"
#include "video.h"

static VideoShared *const s = (VideoShared *)VIDEO_SHARED_ADDR;
static VideoMail *const mail = (VideoMail *)VIDEO_MAIL_ADDR;

static AVCodecContext *vctx, *actx;
static AVFrame *vframe, *aframe;
static AVPacket *pkt;
static int frame_ready;

#define PCM_SAMPLES (VIDEO_PCM_BYTES / 2u)
/* Sound kept ahead of the speakers: enough for the longest a frame can hold the
 * loop up on a slow console, and no more than the ring can tell apart. */
#define PCM_AHEAD (PCM_SAMPLES / 2u)
/* Where writing starts past the playing position, after a gap. */
#define PCM_RESTART 4096u
/* The most that can be written ahead when a pause comes. */
#define PCM_HOLD (PCM_AHEAD + PCM_RESTART)

static int16_t *const pcm_l = (int16_t *)VIDEO_PCM_L_ADDR;
static int16_t *const pcm_r = (int16_t *)VIDEO_PCM_R_ADDR;

/* All in samples since the channels started, which the ring wraps. */
static uint64_t pcm_write, pcm_clear, pcm_played;
static uint32_t pcm_rate, pcm_rem, pcm_last_us;
static int pcm_on;
/* The media time of sample pcm_write, in thousandths of a millisecond. */
static int64_t pcm_write_ms_x1k;
static int clock_held;
static int32_t held_ms;
/* Sound written but not played when a pause began. The rings go on turning, so it
 * waits here and goes back in afterwards. */
static int16_t *held_l, *held_r;
static uint32_t held_n;

/* The rate CSND really runs at for this one asked for: Audio11.c's divider. */
static uint32_t csnd_rate(uint32_t rate) { return 0x3FEC3FCu / (0x3FEC3FCu / rate); }

static uint32_t now_us(void) { return mail->now_us; }

static void pcm_start(uint32_t rate) {
  pcm_rate = csnd_rate(rate);
  memset((void *)pcm_l, 0, VIDEO_PCM_BYTES);
  memset((void *)pcm_r, 0, VIDEO_PCM_BYTES);
  pcm_write = PCM_RESTART;
  pcm_clear = 0;
  pcm_played = 0;
  pcm_rem = 0;
  mail->audio_left = VIDEO_PCM_L_ADDR;
  mail->audio_right = VIDEO_PCM_R_ADDR;
  mail->audio_bytes = VIDEO_PCM_BYTES;
  mail->audio_rate = rate;
  mail->audio_vol = 0x8000u;
  cpu_dmb();
  mail->audio_seq = mail->audio_seq + 1u;
  while (mail->audio_seen != mail->audio_seq && !s->stop && !smp_stop())
    ;
  pcm_last_us = mail->audio_start_us;
  pcm_on = 1;
}

/* Where CSND has got to, by the clock: its position cannot be read. */
static void pcm_track(void) {
  uint32_t t = now_us(), d = t - pcm_last_us;
  uint64_t n;
  if (!pcm_on || d < 1000u)
    return;
  pcm_last_us = t;
  n = (uint64_t)d * pcm_rate + pcm_rem;
  pcm_played += n / 1000000u;
  pcm_rem = (uint32_t)(n % 1000000u);

  /* What has been played goes silent, so a stall replays nothing old. */
  while (pcm_clear < pcm_played) {
    uint32_t at = (uint32_t)(pcm_clear % PCM_SAMPLES), k = PCM_SAMPLES - at;
    if (k > pcm_played - pcm_clear)
      k = (uint32_t)(pcm_played - pcm_clear);
    memset((void *)&pcm_l[at], 0, k * 2u);
    memset((void *)&pcm_r[at], 0, k * 2u);
    pcm_clear += k;
  }
}

static inline int16_t clip16(int v) {
  return v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v;
}

/* Into both rings at pcm_write; `ms` is the media time of the first sample. */
static void pcm_put(const AVFrame *f, int64_t ms) {
  const int stereo = f->ch_layout.nb_channels > 1;
  const float *a = (const float *)f->extended_data[0];
  const float *b = (const float *)f->extended_data[stereo ? 1 : 0];
  int n = f->nb_samples;

  if (pcm_write < pcm_played + PCM_RESTART / 2u) {
    /* The speakers overtook what had been written: start again a little ahead. */
    pcm_write = pcm_played + PCM_RESTART;
    s->audio_underruns++;
  }
  for (int i = 0; i < n; i++) {
    uint32_t at = (uint32_t)((pcm_write + (uint32_t)i) % PCM_SAMPLES);
    /* RunFast rounds toward zero well enough for sound. */
    pcm_l[at] = clip16((int)(a[i] * 32767.0f));
    pcm_r[at] = clip16((int)(b[i] * 32767.0f));
  }
  pcm_write += (uint32_t)n;
  pcm_write_ms_x1k =
      ms * 1000 + (int64_t)n * 1000000 / (int64_t)(pcm_rate ? pcm_rate : 44100u);
}

/* A pause: silence at once, and what was to come kept. */
static void pcm_hold(void) {
  uint32_t n = 0;
  if (!pcm_on)
    return;
  pcm_track();
  if (pcm_write > pcm_played) {
    uint64_t ahead = pcm_write - pcm_played;
    n = ahead > PCM_HOLD ? PCM_HOLD : (uint32_t)ahead;
    for (uint32_t i = 0; i < n; i++) {
      uint32_t at = (uint32_t)((pcm_write - n + i) % PCM_SAMPLES);
      held_l[i] = pcm_l[at];
      held_r[i] = pcm_r[at];
    }
  }
  held_n = n;
  memset((void *)pcm_l, 0, VIDEO_PCM_BYTES);
  memset((void *)pcm_r, 0, VIDEO_PCM_BYTES);
  pcm_write = pcm_clear = pcm_played;
}

/* The end of a pause: it goes back in a little ahead of where the rings have got to.
 * pcm_write_ms_x1k still describes its end. */
static void pcm_release(void) {
  if (!pcm_on)
    return;
  pcm_track();
  pcm_write = pcm_played + PCM_RESTART;
  for (uint32_t i = 0; i < held_n; i++) {
    uint32_t at = (uint32_t)((pcm_write + i) % PCM_SAMPLES);
    pcm_l[at] = held_l[i];
    pcm_r[at] = held_r[i];
  }
  pcm_write += held_n;
  held_n = 0;
}

static int32_t clock_now(void) {
  int64_t behind;
  if (!pcm_on)
    return s->flush_pts_ms;
  if (pcm_write <= pcm_played) {
    /* Out of sound it holds, for a stall. With no more to come time goes on, or a
     * video that outlasts its sound would never reach its end. */
    int32_t ms = (int32_t)(pcm_write_ms_x1k / 1000);
    if (s->end_of_stream && s->aring_read == s->aring_write)
      ms += (int32_t)((pcm_played - pcm_write) * 1000u / pcm_rate);
    return ms;
  }
  behind = (int64_t)(pcm_write - pcm_played) * 1000000 / (int64_t)pcm_rate;
  return (int32_t)((pcm_write_ms_x1k - behind) / 1000);
}

/* One packet out of a ring and into pkt; 0 when the ring has none. */
static int ring_get(uint32_t base, uint32_t size, uint32_t write, volatile uint32_t *read,
                    AVPacket *out) {
  uint32_t r = *read;
  VideoPkt h;
  if (r == write)
    return 0;
  /* With no room for a marker the writer just starts again from the top. */
  if (size - r < sizeof(h)) {
    r = 0;
  } else {
    memcpy(&h, (const void *)(base + r), sizeof(h));
    if (h.size == VIDEO_PKT_WRAP)
      r = 0;
  }
  if (r == 0u && *read != 0u) {
    *read = 0;
    if (write == 0u)
      return 0;
  }
  memcpy(&h, (const void *)(base + r), sizeof(h));
  if (h.size > size || av_new_packet(out, (int)h.size) < 0)
    return 0;
  memcpy(out->data, (const void *)(base + r + sizeof(h)), h.size);
  out->pts = out->dts = h.pts_ms;
  out->flags = (h.flags & VIDEO_PKT_KEY) ? AV_PKT_FLAG_KEY : 0;
  r += (uint32_t)sizeof(h) + ((h.size + 3u) & ~3u);
  *read = r >= size ? 0u : r;
  return 1;
}

static void fail(uint32_t why) {
  s->error = why;
  s->state = VIDEO_ST_FAILED;
}

static void decoders_close(void) {
  if (frame_ready)
    av_frame_unref(vframe);
  frame_ready = 0;
  avcodec_free_context(&vctx);
  avcodec_free_context(&actx);
}

static int decoders_open(void) {
  const AVCodec *c;
  uint8_t *ex;
  decoders_close();

  c = avcodec_find_decoder(AV_CODEC_ID_H264);
  vctx = c ? avcodec_alloc_context3(c) : 0;
  if (!vctx)
    return VIDEO_ERR_NOMEM;
  ex = av_mallocz(VIDEO_EXTRA_MAX + AV_INPUT_BUFFER_PADDING_SIZE);
  if (!ex)
    return VIDEO_ERR_NOMEM;
  memcpy(ex, s->video_extra, s->video_extra_len);
  vctx->extradata = ex;
  vctx->extradata_size = (int)s->video_extra_len;
  vctx->width = (int)s->video_w;
  vctx->height = (int)s->video_h;
  vctx->thread_count = s->threads ? (int)s->threads : 1;
  vctx->thread_type = FF_THREAD_FRAME;
  vctx->flags2 |= AV_CODEC_FLAG2_FAST;
  if (avcodec_open2(vctx, c, 0) < 0)
    return VIDEO_ERR_VIDEO_OPEN;

  if (s->audio_extra_len) {
    c = avcodec_find_decoder(AV_CODEC_ID_AAC);
    actx = c ? avcodec_alloc_context3(c) : 0;
    if (!actx)
      return VIDEO_ERR_NOMEM;
    ex = av_mallocz(VIDEO_EXTRA_MAX + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!ex)
      return VIDEO_ERR_NOMEM;
    memcpy(ex, s->audio_extra, s->audio_extra_len);
    actx->extradata = ex;
    actx->extradata_size = (int)s->audio_extra_len;
    if (avcodec_open2(actx, c, 0) < 0)
      return VIDEO_ERR_AUDIO_OPEN;
  }
  return VIDEO_ERR_NONE;
}

static void audio_pump(void) {
  if (!actx)
    return;
  while (s->aring_read != s->aring_write &&
         (!pcm_on || pcm_write < pcm_played + PCM_AHEAD)) {
    if (!ring_get(VIDEO_ARING_ADDR, VIDEO_ARING_SIZE, s->aring_write, &s->aring_read,
                  pkt))
      return;
    int64_t pts = pkt->pts;
    int r = avcodec_send_packet(actx, pkt);
    av_packet_unref(pkt);
    if (r < 0)
      continue;
    while (avcodec_receive_frame(actx, aframe) >= 0) {
      if (!pcm_on)
        pcm_start((uint32_t)(actx->sample_rate ? actx->sample_rate : 44100));
      pcm_put(aframe, pts);
      pts += (int64_t)aframe->nb_samples * 1000 /
             (actx->sample_rate ? actx->sample_rate : 44100);
      av_frame_unref(aframe);
    }
  }
}

static uint32_t frame_slot;
static int video_draining, video_eof;
static int wall_set;
static int32_t wall_base;

/* A decoded frame onto the screen, if its time has come. */
static void video_show(void) {
  int32_t pts, now, late;
  if (!frame_ready)
    return;
  pts = (int32_t)vframe->pts;
  if (actx) {
    now = clock_now();
  } else {
    if (!wall_set) {
      wall_base = (int32_t)(now_us() / 1000u) - pts;
      wall_set = 1;
    }
    now = (int32_t)(now_us() / 1000u) - wall_base;
  }
  late = now - pts;
  if (late < -8)
    return;
  if (late > 60) {
    /* Better to catch up than to show the past. */
    s->frames_dropped++;
    av_frame_unref(vframe);
    frame_ready = 0;
    return;
  }
  /* The next slot must not be the one still being copied. */
  if (mail->present_done != mail->present_seq)
    return;
  frame_slot = (frame_slot + 1u) % VIDEO_FRAMES;
  uint32_t t0 = now_us();
  video11_convert(vframe, (uint8_t *)VIDEO_FRAME_ADDR(frame_slot));
  s->convert_us += now_us() - t0;
  cpu_dsb();
  mail->present_src = VIDEO_FRAME_ADDR(frame_slot);
  cpu_dmb();
  mail->present_seq = mail->present_seq + 1u;
  s->frames_shown++;
  av_frame_unref(vframe);
  frame_ready = 0;
}

static void video_pump(void) {
  uint32_t t0;
  int r;
  if (frame_ready || !vctx)
    return;
  t0 = now_us();
  r = avcodec_receive_frame(vctx, vframe);
  if (r >= 0) {
    if (vframe->format != AV_PIX_FMT_YUV420P &&
        vframe->format != AV_PIX_FMT_YUVJ420P) {
      fail(VIDEO_ERR_FORMAT);
      return;
    }
    if (vframe->pts == AV_NOPTS_VALUE)
      vframe->pts = vframe->best_effort_timestamp;
    frame_ready = 1;
    s->frames_decoded++;
    s->decode_us += now_us() - t0;
    return;
  }
  if (r == AVERROR_EOF) {
    video_eof = 1;
    return;
  }
  /* AVERROR(EAGAIN): it wants a packet, or to be told there are no more. */
  if (s->vring_read == s->vring_write) {
    if (s->end_of_stream && !video_draining) {
      avcodec_send_packet(vctx, 0);
      video_draining = 1;
    }
    return;
  }
  if (!ring_get(VIDEO_VRING_ADDR, VIDEO_VRING_SIZE, s->vring_write, &s->vring_read, pkt))
    return;
  r = avcodec_send_packet(vctx, pkt);
  av_packet_unref(pkt);
  s->decode_us += now_us() - t0;
  (void)r;
}

/* A seek: everything queued is for the old position. */
static void do_flush(void) {
  if (vctx)
    avcodec_flush_buffers(vctx);
  if (actx)
    avcodec_flush_buffers(actx);
  if (frame_ready) {
    av_frame_unref(vframe);
    frame_ready = 0;
  }
  s->vring_read = s->vring_write;
  s->aring_read = s->aring_write;
  if (pcm_on) {
    pcm_track();
    pcm_write = pcm_played + PCM_RESTART;
    pcm_clear = pcm_played;
    memset((void *)pcm_l, 0, VIDEO_PCM_BYTES);
    memset((void *)pcm_r, 0, VIDEO_PCM_BYTES);
    pcm_write_ms_x1k = (int64_t)s->flush_pts_ms * 1000;
  }
  video_draining = video_eof = 0;
  s->ended = 0;
  clock_held = 0;
  held_n = 0;
  wall_set = 0;
  s->clock_ms = s->flush_pts_ms;
  s->flush_ack = s->flush_seq;
}

/* A new stream, on an engine that may have played another. The ARM9 has put its ends
 * of the rings back at the start. The sound starts again with the first of its
 * frames, since it may have another rate. */
static void stream_reset(void) {
  s->vring_read = s->aring_read = 0;
  if (pcm_on) {
    memset((void *)pcm_l, 0, VIDEO_PCM_BYTES);
    memset((void *)pcm_r, 0, VIDEO_PCM_BYTES);
    pcm_on = 0;
  }
  video_draining = video_eof = 0;
  s->ended = 0;
  clock_held = 0;
  held_n = 0;
  wall_set = 0;
  s->clock_ms = s->flush_pts_ms;
  s->frames_decoded = s->frames_shown = s->frames_dropped = 0;
  s->audio_underruns = 0;
  s->decode_us = s->convert_us = 0;
}

static int sound_done(void) {
  return !actx || (s->aring_read == s->aring_write && (!pcm_on || pcm_write <= pcm_played));
}

void video11_main(void) {
  uint32_t config_seen = 0, flush_seen = 0;

  av_log_set_level(AV_LOG_QUIET);
  vframe = av_frame_alloc();
  aframe = av_frame_alloc();
  pkt = av_packet_alloc();
  held_l = av_malloc(PCM_HOLD * sizeof(int16_t));
  held_r = av_malloc(PCM_HOLD * sizeof(int16_t));
  if (!vframe || !aframe || !pkt || !held_l || !held_r) {
    fail(VIDEO_ERR_NOMEM);
    return;
  }
  mail->magic = VIDEO_MAIL_MAGIC;
  s->state = VIDEO_ST_READY;

  while (!s->stop && !smp_stop()) {
    s->heartbeat++;
    if (s->config_ack != s->config_seq) {
      config_seen = s->config_seq;
      int e = decoders_open();
      if (e) {
        fail((uint32_t)e);
        break;
      }
      stream_reset();
      s->config_ack = config_seen;
      s->state = VIDEO_ST_PLAYING;
    }
    if (s->flush_seq != flush_seen) {
      flush_seen = s->flush_seq;
      do_flush();
    }
    /* Polled: the ARM9 cannot send this core an event. */
    if (s->state != VIDEO_ST_PLAYING)
      continue;
    pcm_track();
    if (s->paused) {
      if (!clock_held) {
        held_ms = clock_now();
        pcm_hold();
        clock_held = 1;
      }
      s->clock_ms = held_ms;
      continue;
    }
    if (clock_held) {
      pcm_release();
      clock_held = 0;
    }
    s->clock_ms = clock_now();
    audio_pump();
    video_show();
    video_pump();
    if (video_eof && !frame_ready && sound_done())
      s->ended = 1;
  }

  /* Back to where a new job can start cleanly. The heap goes with the job. */
  decoders_close();
  threads11_stop();
}
