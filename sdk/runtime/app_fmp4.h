#ifndef AURORA_APP_FMP4_H
#define AURORA_APP_FMP4_H

#include <stdint.h>

/* A demuxer for one-track fragmented MP4, the form DASH uses, which takes its bytes
 * in whatever pieces they arrive and hands out samples whole. No memory is its own:
 * the caller gives it room for a moof and for the sample table. */

#define FMP4_EXTRA_MAX 512u

typedef struct {
  /* What the current sample goes to. begin returns 0 when there is no room yet: the
   * feed stops there and the same sample is offered again next time. */
  int (*begin)(void *ctx, uint32_t size, int32_t pts_ms, int key);
  void (*data)(void *ctx, const uint8_t *p, uint32_t n);
  void (*end)(void *ctx);
  void *ctx;
} Fmp4Sink;

typedef struct {
  uint64_t offset;
  int32_t start_ms;
} Fmp4Segment;

typedef struct {
  uint32_t size;
  uint32_t flags; /* FMP4_SAMPLE_KEY */
  int32_t pts_ms;
} Fmp4Sample;

#define FMP4_SAMPLE_KEY 1u

typedef struct {
  /* From the initialisation segment. */
  uint32_t timescale;
  int is_audio;
  uint32_t width, height;
  uint32_t rate, channels;
  uint8_t extra[FMP4_EXTRA_MAX];    /* avcC's contents, or the AudioSpecificConfig */
  uint32_t extra_len;
  uint32_t trex_duration, trex_size, trex_flags;

  /* The stream position. */
  uint8_t *moof;
  uint32_t moof_cap;
  Fmp4Sample *samples;
  uint32_t samples_cap;

  int state;
  uint8_t head[16];
  uint32_t head_have, head_need;
  uint64_t box_size, box_done;
  uint32_t moof_have;
  uint32_t n_samples, sample_i, sample_done;
  uint64_t mdat_skip;
  int in_sample;
  int failed;
} Fmp4;

void fmp4_init(Fmp4 *d, uint8_t *moof, uint32_t moof_cap, Fmp4Sample *samples,
               uint32_t samples_cap);
/* ftyp + moov, whole. 1 when it held one track of H.264 or AAC. */
int fmp4_parse_init(Fmp4 *d, const uint8_t *p, uint32_t n);
/* A sidx box, whole, read from file offset `at`: fills at most `max` segments and
 * returns how many. */
uint32_t fmp4_parse_sidx(const Fmp4 *d, const uint8_t *p, uint32_t n, uint64_t at,
                         Fmp4Segment *out, uint32_t max);
/* The next bytes are the start of a box: after a seek. */
void fmp4_reset(Fmp4 *d);
/* Takes up to n bytes and returns how many it used, which is less than n only when
 * the sink has no room. d->failed is set on a stream it cannot follow. */
uint32_t fmp4_feed(Fmp4 *d, const uint8_t *p, uint32_t n, const Fmp4Sink *sink);

#endif
