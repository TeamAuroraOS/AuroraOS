/* Video for apps: fetches and demuxes on the ARM9 and hands packets to the engine
 * on the other ARM11 cores (sdk/video11). Linked into apps built with --video, since
 * the engine is about 2 MB. docs/video.md has the design; include/video.h the
 * memory they share. */
#include "app_internal.h"
#include "app_fmp4.h"
#include "appnet.h"
#include "audio.h"
#include "ff.h"
#include "model.h"
#include "smp.h"
#include "video.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

extern const u8 _binary_video11_bin_start[], _binary_video11_bin_end[];
extern void os_cache_sync(void);
extern void os_dcache_clean_range(const void *p, u32 n);

static VideoShared *const vs = (VideoShared *)VIDEO_SHARED_ADDR;

#define AWAIT_MS       5000u
#define MOOF_MAX       (256u * 1024u)
#define SAMPLES_MAX    8192u
#define SEGMENTS_MAX   1024u
#define INIT_MAX       (256u * 1024u)
/* How far ahead of the speakers to keep fetching, and how much to have before
 * starting or starting again. */
#define AHEAD_MS       15000
#define START_MS       2500
#define LOW_MS         250
/* Asked for in one go. The audio's is larger in time, so turns stay few. */
#define PIECE_VIDEO    (512u * 1024u)
#define PIECE_AUDIO    (256u * 1024u)
#define READ_CHUNK     (32u * 1024u)
#define RETRIES        4
#define RETRY_MS       500u

typedef struct {
  Fmp4 d;
  /* One of these two is the source. */
  FIL file;
  int is_file;
  char *url; /* where it is now, after any redirect */
  char *headers;
  uint64_t pos, size;
  Fmp4Segment *segs;
  uint32_t n_segs;
  int32_t last_pts;
  u8 *buf;
  uint32_t buf_len, buf_off;
  /* A sample going into the ring. */
  uint32_t w_at, w_len;
  int w_key;
  int32_t w_pts;
  uint32_t ring_base, ring_size;
  uint32_t *ring_write, *ring_read;
  int done;
} Track;

static Track g_v, g_a;
static int g_open, g_urls, g_buffering, g_user_paused, g_retries, g_ended;
static Track *g_turn; /* whose request is on the connection */
static uint64_t g_turn_from, g_turn_end;
static u32 g_retry_at;
static const char *g_err;
static uint32_t g_started_ms;
static uint32_t g_shown_at_start;
static int g_cores;

static void inval(const volatile void *p, u32 n) {
  u32 a = (u32)p & ~31u, e = (u32)p + n;
  for (; a < e; a += 32u)
    __asm__ volatile("mcr p15, 0, %0, c7, c6, 1" ::"r"(a) : "memory");
}

static void engine_view(void) { inval(&vs->state, 64u); }

static void arm9_publish(const volatile void *p, u32 n) {
  os_dcache_clean_range((const void *)p, n);
}

const char *video_error(void) {
  static const char *const why[] = {
      "", "the video engine ran out of memory", "the video could not be opened",
      "the sound could not be opened", "this video's format is not supported"};
  engine_view();
  if (vs->magic == VIDEO_MAGIC && vs->state == VIDEO_ST_FAILED && vs->error < 5u)
    return why[vs->error];
  return g_err ? g_err : "";
}

/* AUDIO_CMD_SMP can take a while: a core start can switch the clock. */
static bool smp_post(u32 op, u32 arg) {
  AudioCtrl *ct = (AudioCtrl *)AUDIO_CTRL_ADDR;
  app_core_post(AUDIO_CMD_SMP, op, arg, 0, 0);
  for (u32 t0 = sys_millis(); sys_millis() - t0 < 8000u;) {
    inval(&ct->ack_seq, 4u);
    if (ct->ack_seq == ct->cmd_seq)
      return true;
  }
  return false;
}

static const SmpBlock *smp_view(void) {
  inval((const void *)SMP_BLOCK_ADDR, sizeof(SmpBlock));
  return (const SmpBlock *)SMP_BLOCK_ADDR;
}

/* Full power says whether this console should try the extra cores: at 268 MHz
 * only one is worth the trouble, and that is the mode they are least tried in. */
static u32 cores_wanted(void) {
  const AudioCtrl *ct = (const AudioCtrl *)AUDIO_CTRL_ADDR;
  inval(&ct->clock_div, 4u);
  return aurora_is_new3ds() && ct->clock_div >= 2u ? 0xEu : 0x2u;
}

int video_height_limit(void) { return cores_wanted() == 0xEu ? 480 : 240; }

int video_cores(void) { return g_cores; }

bool video_engine_running(void) {
  engine_view();
  return vs->magic == VIDEO_MAGIC &&
         (vs->state == VIDEO_ST_READY || vs->state == VIDEO_ST_PLAYING);
}

bool video_engine_start(void) {
  SmpJob *job = (SmpJob *)(VIDEO_SHARED_ADDR + 0x8000u);
  u32 want, mask, heap_end, threads = 0;
  u32 bytes = (u32)(_binary_video11_bin_end - _binary_video11_bin_start);
  const SmpBlock *b;

  g_err = 0;
  if (video_engine_running())
    return true;
  if (!app_core() || audio_version() < AUDIO_VIDEO_VERSION) {
    g_err = "video needs a newer AuroraOS";
    return false;
  }
  want = cores_wanted();
  if (!smp_post(SMP_OP_CORES, want)) {
    g_err = "the ARM11 core did not answer";
    return false;
  }
  b = smp_view();
  mask = want & b->started;
  if (!(mask & 2u)) {
    g_err = "the second ARM11 core did not start";
    return false;
  }
  for (u32 c = 2u; c < SMP_CORES; c++)
    threads += (mask >> c) & 1u;
  g_cores = (int)threads + 1;

  memcpy((void *)VIDEO_ENGINE_ADDR, _binary_video11_bin_start, bytes);
  arm9_publish((const void *)VIDEO_ENGINE_ADDR, bytes);

  memset(vs, 0, sizeof(*vs));
  vs->magic = VIDEO_MAGIC;
  vs->threads = threads ? threads : 1u;
  arm9_publish(vs, sizeof(*vs));

  heap_end = aurora_is_new3ds() ? VIDEO_HEAP_END_NEW : VIDEO_HEAP_END_OLD;
  memset(job, 0, sizeof(*job));
  job->entry = VIDEO_ENGINE_ADDR;
  /* The heap's end and the mask of cores share the argument. */
  job->arg = heap_end | mask;
  job->cores = mask;
  for (u32 c = 1u; c < SMP_CORES; c++)
    job->sp[c] = VIDEO_STACK_TOP(c);
  job->map_count = 2;
  job->map[0].first_mb = VIDEO_ENGINE_FIRST_MB;
  job->map[0].mbs = (heap_end >> 20) - VIDEO_ENGINE_FIRST_MB;
  job->map[0].attr = SMP_MAP_CACHED;
  job->map[1].first_mb = VIDEO_SHARED_FIRST_MB;
  job->map[1].mbs = VIDEO_SHARED_MBS;
  job->map[1].attr = SMP_MAP_SHARED;
  arm9_publish(job, sizeof(*job));
  os_cache_sync();

  if (!smp_post(SMP_OP_RUN, (u32)job)) {
    g_err = "the ARM11 core did not answer";
    return false;
  }
  for (u32 t0 = sys_millis(); sys_millis() - t0 < AWAIT_MS;) {
    engine_view();
    if (vs->state == VIDEO_ST_READY)
      return true;
    if (vs->state == VIDEO_ST_FAILED)
      return false;
  }
  g_err = "the video engine did not start";
  return false;
}

void video_engine_stop(void) {
  if (vs->magic != VIDEO_MAGIC)
    return;
  video_close();
  if (vs->magic == VIDEO_MAGIC) {
    vs->stop = 1u;
    arm9_publish(&vs->stop, 4u);
  }
  if (app_core() && audio_version() >= AUDIO_VIDEO_VERSION)
    smp_post(SMP_OP_END, 0);
  vs->magic = 0;
  arm9_publish(&vs->magic, 4u);
}

static int ring_begin(void *ctx, uint32_t size, int32_t pts, int key) {
  Track *t = ctx;
  uint32_t need = (uint32_t)sizeof(VideoPkt) + ((size + 3u) & ~3u);
  uint32_t w = *t->ring_write, r;
  inval(t->ring_read, 4u);
  r = *t->ring_read;
  if (need >= t->ring_size)
    return 0;
  /* It must fit in one run, with a gap left so full never looks like empty. */
  if (r <= w) {
    if (w + need >= t->ring_size) {
      if (r == 0u || need >= r)
        return 0;
      if (t->ring_size - w >= sizeof(VideoPkt)) {
        VideoPkt *wrap = (VideoPkt *)(t->ring_base + w);
        wrap->size = VIDEO_PKT_WRAP;
        wrap->pts_ms = 0;
        wrap->flags = 0;
        arm9_publish(wrap, sizeof(*wrap));
      }
      w = 0;
    }
  } else if (w + need >= r) {
    return 0;
  }
  t->w_at = w;
  t->w_len = 0;
  t->w_key = key;
  t->w_pts = pts;
  return 1;
}

static void ring_data(void *ctx, const uint8_t *p, uint32_t n) {
  Track *t = ctx;
  u8 *d = (u8 *)(t->ring_base + t->w_at + sizeof(VideoPkt) + t->w_len);
  memcpy(d, p, n);
  arm9_publish(d, n);
  t->w_len += n;
}

static void ring_end(void *ctx) {
  Track *t = ctx;
  VideoPkt *h = (VideoPkt *)(t->ring_base + t->w_at);
  uint32_t next = t->w_at + (uint32_t)sizeof(VideoPkt) + ((t->w_len + 3u) & ~3u);
  h->size = t->w_len;
  h->pts_ms = t->w_pts;
  h->flags = t->w_key ? VIDEO_PKT_KEY : 0u;
  arm9_publish(h, sizeof(*h));
  *t->ring_write = next;
  arm9_publish(t->ring_write, 4u);
  if (t->w_pts > t->last_pts)
    t->last_pts = t->w_pts;
}

static const Fmp4Sink sink_v = {ring_begin, ring_data, ring_end, &g_v};
static const Fmp4Sink sink_a = {ring_begin, ring_data, ring_end, &g_a};

static const Fmp4Sink *sink_of(const Track *t) { return t == &g_v ? &sink_v : &sink_a; }

static void track_free(Track *t) {
  if (t->is_file)
    f_close(&t->file);
  free(t->d.moof);
  free(t->d.samples);
  free(t->segs);
  free(t->buf);
  free(t->url);
  free(t->headers);
  memset(t, 0, sizeof(*t));
}

static bool track_alloc(Track *t) {
  u8 *moof = malloc(MOOF_MAX);
  Fmp4Sample *smp = malloc(SAMPLES_MAX * sizeof(Fmp4Sample));
  t->segs = malloc(SEGMENTS_MAX * sizeof(Fmp4Segment));
  t->buf = malloc(READ_CHUNK);
  if (!moof || !smp || !t->segs || !t->buf) {
    free(moof);
    free(smp);
    g_err = "not enough memory for the video";
    return false;
  }
  fmp4_init(&t->d, moof, MOOF_MAX, smp, SAMPLES_MAX);
  t->last_pts = 0;
  return true;
}

/* The boxes before the first moof, in memory: the track's description and index. */
static bool track_head(Track *t, const u8 *p, uint32_t n) {
  uint32_t off = 0;
  bool have_init = false;
  while (off + 8u <= n) {
    uint32_t sz = ((uint32_t)p[off] << 24) | ((uint32_t)p[off + 1] << 16) |
                  ((uint32_t)p[off + 2] << 8) | p[off + 3];
    const u8 *name = p + off + 4;
    if (sz < 8u || sz > n - off)
      break;
    if (!memcmp(name, "moov", 4)) {
      if (!fmp4_parse_init(&t->d, p, off + sz))
        return false;
      have_init = true;
    } else if (!memcmp(name, "sidx", 4)) {
      t->n_segs = fmp4_parse_sidx(&t->d, p + off, sz, off, t->segs, SEGMENTS_MAX);
    } else if (!memcmp(name, "moof", 4)) {
      break;
    }
    off += sz;
    t->pos = off;
  }
  return have_init;
}

static bool track_open_file(Track *t, const char *path) {
  char full[300];
  UINT got = 0;
  u8 *head = 0;
  bool ok = false;
  if (!app_fs_mount() || !app_fs_path(path, full, sizeof(full))) {
    g_err = "the SD card could not be read";
    return false;
  }
  if (f_open(&t->file, full, FA_READ) != FR_OK) {
    g_err = "the video file could not be opened";
    return false;
  }
  t->is_file = 1;
  t->size = f_size(&t->file);
  head = malloc(INIT_MAX);
  if (head && f_read(&t->file, head, INIT_MAX, &got) == FR_OK && track_head(t, head, got))
    ok = f_lseek(&t->file, (FSIZE_t)t->pos) == FR_OK;
  free(head);
  if (!ok)
    g_err = "that file is not a fragmented MP4 of H.264 or AAC";
  return ok;
}

static bool configure(void) {
  vs->video_w = g_v.d.width;
  vs->video_h = g_v.d.height;
  vs->video_extra_len = g_v.d.extra_len;
  memcpy(vs->video_extra, g_v.d.extra, g_v.d.extra_len);
  vs->audio_rate = g_a.d.rate;
  vs->audio_channels = g_a.d.channels;
  vs->audio_extra_len = g_a.d.extra_len;
  memcpy(vs->audio_extra, g_a.d.extra, g_a.d.extra_len);
  vs->vring_write = vs->aring_write = 0;
  vs->paused = 1u;
  vs->end_of_stream = 0;
  vs->flush_pts_ms = 0;
  arm9_publish(vs, offsetof(VideoShared, state));
  vs->config_seq = vs->config_seq + 1u;
  arm9_publish(&vs->config_seq, 4u);
  for (u32 t0 = sys_millis(); sys_millis() - t0 < AWAIT_MS;) {
    engine_view();
    if (vs->config_ack == vs->config_seq)
      return true;
    if (vs->state == VIDEO_ST_FAILED)
      return false;
  }
  g_err = "the video engine did not answer";
  return false;
}

static void tracks_rings(void) {
  g_v.ring_base = VIDEO_VRING_ADDR;
  g_v.ring_size = VIDEO_VRING_SIZE;
  g_v.ring_write = &vs->vring_write;
  g_v.ring_read = &vs->vring_read;
  g_a.ring_base = VIDEO_ARING_ADDR;
  g_a.ring_size = VIDEO_ARING_SIZE;
  g_a.ring_write = &vs->aring_write;
  g_a.ring_read = &vs->aring_read;
}

static void opened(void) {
  g_open = 1;
  g_buffering = 1;
  g_user_paused = 0;
  g_ended = 0;
  g_retries = 0;
  g_retry_at = sys_millis();
  g_turn = 0;
  engine_view();
  g_started_ms = 0;
  g_shown_at_start = 0;
}

bool video_open_files(const char *video_path, const char *audio_path) {
  video_close();
  g_err = 0;
  if (!track_alloc(&g_v) || !track_alloc(&g_a))
    goto fail;
  if (!track_open_file(&g_v, video_path) || !track_open_file(&g_a, audio_path))
    goto fail;
  if (g_v.d.is_audio || !g_a.d.is_audio) {
    g_err = "expected a video file and an audio file";
    goto fail;
  }
  g_urls = 0;
  tracks_rings();
  if (!configure())
    goto fail;
  opened();
  return true;
fail:
  track_free(&g_v);
  track_free(&g_a);
  return false;
}

static bool track_request(Track *t, uint64_t from, uint64_t to) {
  /* What appnet_stream_get takes. */
  static char lines[512];
  int n = snprintf(lines, sizeof(lines), "%sRange: bytes=%lu-%lu\r\n",
                   t->headers ? t->headers : "", (unsigned long)from, (unsigned long)to);
  if (n < 0 || (size_t)n >= sizeof(lines)) {
    g_err = "the stream's request headers are too long";
    return false;
  }
  if (!appnet_stream_get(t->url, lines)) {
    g_err = appnet_error();
    return false;
  }
  return true;
}

/* Only a 206 is the part asked for: a 200 is the whole file from its first byte. */
static bool reply_is_part(void) {
  static char msg[48];
  int code = appnet_stream_status();
  if (code == 206)
    return true;
  if (code == 200) {
    g_err = "this server cannot send part of a file";
  } else {
    snprintf(msg, sizeof(msg), "the server said %d to a video request", code);
    g_err = msg;
  }
  return false;
}

/* Another server has this track now, and is asked directly from here on, so the
 * stream's own rule no longer covers what was meant for the first. */
static void headers_drop_keys(char *h) {
  char *out = h;
  while (*h) {
    char *e = h;
    while (*e && *e != '\n')
      e++;
    if (*e)
      e++;
    if (strncasecmp(h, "authorization:", 14) && strncasecmp(h, "cookie:", 7)) {
      memmove(out, h, (size_t)(e - h));
      out += e - h;
    }
    h = e;
  }
  *out = 0;
}

/* Blocks: only as a video opens. */
static bool track_open_url(Track *t, const VideoTrackUrl *src) {
  u8 *head;
  uint32_t have = 0, want;
  bool ok = false, refused = false;
  t->url = strdup(src->url);
  t->headers = src->headers ? strdup(src->headers) : 0;
  t->size = src->size;
  want = src->index_end + 1u;
  if (!t->url || (src->headers && !t->headers)) {
    g_err = "not enough memory for the video";
    return false;
  }
  if (!want || want > INIT_MAX || want >= src->size) {
    g_err = "this stream has no usable index";
    return false;
  }
  /* track_request names its bytes in 32 bits. */
  if (src->size > 0xFFFFFFFFu) {
    g_err = "this stream is over 4 GB";
    return false;
  }
  head = malloc(want);
  if (!head) {
    g_err = "not enough memory for the video";
    return false;
  }
  if (track_request(t, 0, want - 1u)) {
    for (u32 t0 = sys_millis(); have < want && sys_millis() - t0 < 30000u;) {
      int st = appnet_stream_poll();
      if (st == APPNET_STREAM_FAILED)
        break;
      if (st == APPNET_STREAM_BODY || st == APPNET_STREAM_DONE) {
        uint32_t got;
        if (!reply_is_part()) {
          refused = true;
          break;
        }
        got = appnet_stream_read(head + have, want - have);
        have += got;
        if (st == APPNET_STREAM_DONE && !got)
          break;
      }
      app_poll_home();
    }
    ok = !refused && have == want && track_head(t, head, have);
    /* Whatever redirect that took is where this track lives now. */
    if (ok) {
      char *now = strdup(appnet_stream_url());
      if (now) {
        free(t->url);
        t->url = now;
        if (t->headers && appnet_stream_moved())
          headers_drop_keys(t->headers);
      }
    } else if (refused) {
      /* reply_is_part said why. */
    } else if (appnet_error()[0]) {
      g_err = appnet_error();
    } else {
      g_err = "the stream's header could not be read";
    }
  }
  free(head);
  return ok;
}

bool video_open_urls(const VideoTrackUrl *video, const VideoTrackUrl *audio) {
  video_close();
  g_err = 0;
  if (!track_alloc(&g_v) || !track_alloc(&g_a))
    goto fail;
  if (!track_open_url(&g_v, video) || !track_open_url(&g_a, audio))
    goto fail;
  if (g_v.d.is_audio || !g_a.d.is_audio) {
    g_err = "expected a video stream and an audio stream";
    goto fail;
  }
  g_urls = 1;
  tracks_rings();
  if (!configure())
    goto fail;
  opened();
  return true;
fail:
  appnet_stream_close();
  track_free(&g_v);
  track_free(&g_a);
  return false;
}

void video_close(void) {
  if (g_urls)
    appnet_stream_close();
  if (g_open && vs->magic == VIDEO_MAGIC) {
    vs->paused = 1u;
    arm9_publish(&vs->paused, 4u);
  }
  g_open = 0;
  g_urls = 0;
  g_turn = 0;
  track_free(&g_v);
  track_free(&g_a);
}

/* Bytes it already has into the demuxer; true while the ring can take them. */
static bool track_drain(Track *t) {
  while (t->buf_off < t->buf_len) {
    uint32_t u = fmp4_feed(&t->d, t->buf + t->buf_off, t->buf_len - t->buf_off, sink_of(t));
    if (t->d.failed) {
      g_err = "the stream could not be read";
      return false;
    }
    t->buf_off += u;
    if (!u)
      return true;
  }
  t->buf_off = t->buf_len = 0;
  return true;
}

static bool pump_file(Track *t, int32_t ahead_of) {
  UINT got = 0;
  if (t->buf_off < t->buf_len)
    return track_drain(t);
  if (t->done || t->last_pts - ahead_of >= AHEAD_MS)
    return true;
  if (f_read(&t->file, t->buf, READ_CHUNK, &got) != FR_OK) {
    g_err = "the video file could not be read";
    return false;
  }
  if (!got) {
    t->done = 1;
    return true;
  }
  t->pos += got;
  t->buf_len = got;
  t->buf_off = 0;
  return track_drain(t);
}

/* A connection a server dropped, or a moment of bad Wi-Fi: where it stopped is known,
 * so it can just ask again, a little later each time. false once that has not helped. */
static bool try_again(void) {
  if (++g_retries > RETRIES) {
    if (!g_err)
      g_err = appnet_error()[0] ? appnet_error() : "the connection was lost";
    return false;
  }
  g_err = 0;
  g_turn = 0;
  g_retry_at = sys_millis() + (u32)g_retries * RETRY_MS;
  /* A failed stream goes on saying so until it is closed. */
  appnet_stream_close();
  return true;
}

/* One connection, so one request at a time: the track furthest behind gets it. */
static bool pump_urls(int32_t clock) {
  int st;
  if (g_v.buf_off < g_v.buf_len && !track_drain(&g_v))
    return false;
  if (g_a.buf_off < g_a.buf_len && !track_drain(&g_a))
    return false;
  st = appnet_stream_poll();
  if (st == APPNET_STREAM_FAILED)
    return try_again();
  if (g_turn && (st == APPNET_STREAM_BODY || st == APPNET_STREAM_DONE)) {
    Track *t = g_turn;
    if (!reply_is_part())
      return false;
    if (t->buf_off >= t->buf_len) {
      uint32_t got = appnet_stream_read(t->buf, READ_CHUNK);
      if (got) {
        g_retries = 0;
        t->pos += got;
        t->buf_len = got;
        t->buf_off = 0;
        if (!track_drain(t))
          return false;
      }
    }
    if (t->pos >= g_turn_end || (st == APPNET_STREAM_DONE && t->buf_off >= t->buf_len)) {
      /* A reply with nothing in it would otherwise be asked for again for ever. */
      if (t->pos == g_turn_from)
        return try_again();
      if (t->pos >= t->size)
        t->done = 1;
      g_turn = 0;
    }
    return true;
  }
  if (!g_turn && (int32_t)(sys_millis() - g_retry_at) >= 0) {
    Track *t = 0;
    int v_wants = !g_v.done && g_v.last_pts - clock < AHEAD_MS;
    int a_wants = !g_a.done && g_a.last_pts - clock < AHEAD_MS;
    if (v_wants && a_wants)
      t = g_a.last_pts < g_v.last_pts ? &g_a : &g_v;
    else if (v_wants)
      t = &g_v;
    else if (a_wants)
      t = &g_a;
    if (t) {
      uint64_t piece = t == &g_v ? PIECE_VIDEO : PIECE_AUDIO;
      uint64_t end = t->pos + piece;
      if (end > t->size)
        end = t->size;
      if (!track_request(t, t->pos, end - 1u))
        return try_again();
      g_turn = t;
      g_turn_from = t->pos;
      g_turn_end = end;
    }
  }
  return true;
}

bool video_pump(void) {
  int32_t clock, have;
  int eos;
  if (!g_open)
    return false;
  engine_view();
  if (vs->state == VIDEO_ST_FAILED)
    return false;
  clock = vs->clock_ms;
  if (g_urls) {
    if (!pump_urls(clock))
      return false;
  } else if (!pump_file(&g_v, clock) || !pump_file(&g_a, clock)) {
    return false;
  }

  eos = g_v.done && g_a.done && g_v.buf_off >= g_v.buf_len && g_a.buf_off >= g_a.buf_len;
  if (eos && !vs->end_of_stream) {
    vs->end_of_stream = 1u;
    arm9_publish(&vs->end_of_stream, 4u);
  }
  /* Buffering: the speakers wait for more, and only one track being short counts. */
  have = (g_v.last_pts < g_a.last_pts ? g_v.last_pts : g_a.last_pts) - clock;
  if (g_buffering && (have >= START_MS || eos)) {
    g_buffering = 0;
    if (!g_user_paused) {
      vs->paused = 0;
      arm9_publish(&vs->paused, 4u);
    }
    if (!g_started_ms) {
      g_started_ms = sys_millis();
      g_shown_at_start = vs->frames_shown;
    }
  } else if (!g_buffering && !g_user_paused && !eos && have < LOW_MS) {
    g_buffering = 1;
    vs->paused = 1u;
    arm9_publish(&vs->paused, 4u);
  }
  if (vs->ended)
    g_ended = 1;
  return !g_ended;
}

void video_pause(bool on) {
  g_user_paused = on;
  if (!g_open)
    return;
  if (on || !g_buffering) {
    vs->paused = on ? 1u : 0u;
    arm9_publish(&vs->paused, 4u);
  }
}

static void track_seek(Track *t, int32_t ms) {
  uint32_t i = 0;
  while (i + 1u < t->n_segs && t->segs[i + 1u].start_ms <= ms)
    i++;
  if (!t->n_segs)
    return;
  t->pos = t->segs[i].offset;
  t->last_pts = t->segs[i].start_ms;
  t->done = 0;
  t->buf_off = t->buf_len = 0;
  fmp4_reset(&t->d);
  if (t->is_file)
    f_lseek(&t->file, (FSIZE_t)t->pos);
}

bool video_seek(int32_t ms) {
  /* Both or neither: one track alone would go on from the old place. */
  if (!g_open || !g_v.n_segs || !g_a.n_segs)
    return false;
  if (ms < 0)
    ms = 0;
  if (g_urls)
    appnet_stream_close();
  g_turn = 0;
  g_retries = 0;
  g_retry_at = sys_millis();
  vs->paused = 1u;
  vs->end_of_stream = 0;
  g_ended = 0;
  track_seek(&g_v, ms);
  track_seek(&g_a, ms);
  vs->flush_pts_ms = g_a.last_pts;
  arm9_publish(vs, offsetof(VideoShared, state));
  vs->flush_seq = vs->flush_seq + 1u;
  arm9_publish(&vs->flush_seq, 4u);
  for (u32 t0 = sys_millis(); sys_millis() - t0 < AWAIT_MS;) {
    engine_view();
    if (vs->flush_ack == vs->flush_seq) {
      /* The engine took the rings empty, whatever was in them. */
      inval(&vs->vring_read, 8u);
      vs->vring_write = vs->vring_read;
      vs->aring_write = vs->aring_read;
      arm9_publish(&vs->vring_write, 8u);
      g_buffering = 1;
      return true;
    }
  }
  g_err = "the video engine did not answer";
  return false;
}

void video_stats(VideoStats *o) {
  uint32_t ms;
  memset(o, 0, sizeof(*o));
  engine_view();
  if (vs->magic != VIDEO_MAGIC)
    return;
  o->state = (int)vs->state;
  o->playing = g_open && !g_buffering && !g_user_paused;
  o->buffering = g_open && g_buffering;
  o->ended = g_ended;
  o->width = g_v.d.width;
  o->height = g_v.d.height;
  o->clock_ms = vs->clock_ms;
  o->decoded = vs->frames_decoded;
  o->shown = vs->frames_shown;
  o->dropped = vs->frames_dropped;
  o->underruns = vs->audio_underruns;
  o->cores = g_cores;
  o->decode_us = vs->decode_us;
  o->convert_us = vs->convert_us;
  o->heartbeat = vs->heartbeat;
  o->buffered_ms = g_open ? (g_v.last_pts < g_a.last_pts ? g_v.last_pts : g_a.last_pts) -
                                vs->clock_ms
                          : 0;
  ms = g_started_ms ? sys_millis() - g_started_ms : 0u;
  o->fps_x10 = ms ? (vs->frames_shown - g_shown_at_start) * 10000u / ms : 0u;
}
