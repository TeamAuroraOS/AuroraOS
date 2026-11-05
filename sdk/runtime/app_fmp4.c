/* See app_fmp4.h. Box layouts are ISO/IEC 14496-12's, as DASH encoders write them:
 * one traf in each moof, and the samples of a moof in the mdat right after it. */
#include "app_fmp4.h"

#include <string.h>

#define TAG(a, b, c, d)                                                   \
  (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | \
   (uint32_t)(d))

enum { ST_HEAD, ST_MOOF, ST_MDAT, ST_SKIP };

static uint32_t be32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint64_t be64(const uint8_t *p) {
  return ((uint64_t)be32(p) << 32) | be32(p + 4);
}

static uint32_t be16(const uint8_t *p) { return ((uint32_t)p[0] << 8) | p[1]; }

/* The box at p (n bytes left): its type, where its body starts, how long it is.
 * 0 when there is no complete box. */
static int box_at(const uint8_t *p, uint32_t n, uint32_t *type, const uint8_t **body,
                  uint32_t *len) {
  uint64_t size;
  uint32_t head = 8;
  if (n < 8u)
    return 0;
  size = be32(p);
  *type = be32(p + 4);
  if (size == 1u) {
    if (n < 16u)
      return 0;
    size = be64(p + 8);
    head = 16;
  } else if (size == 0u) {
    size = n;
  }
  if (size < head || size > n)
    return 0;
  *body = p + head;
  *len = (uint32_t)size - head;
  return 1;
}

static const uint8_t *find_box(const uint8_t *p, uint32_t n, uint32_t want, uint32_t *len) {
  uint32_t t, l;
  const uint8_t *b;
  while (box_at(p, n, &t, &b, &l)) {
    if (t == want) {
      *len = l;
      return b;
    }
    n -= (uint32_t)(b + l - p);
    p = b + l;
  }
  return 0;
}

void fmp4_init(Fmp4 *d, uint8_t *moof, uint32_t moof_cap, Fmp4Sample *samples,
               uint32_t samples_cap) {
  memset(d, 0, sizeof(*d));
  d->moof = moof;
  d->moof_cap = moof_cap;
  d->samples = samples;
  d->samples_cap = samples_cap;
  fmp4_reset(d);
}

void fmp4_reset(Fmp4 *d) {
  d->state = ST_HEAD;
  d->head_have = 0;
  d->head_need = 8;
  d->n_samples = d->sample_i = d->sample_done = 0;
  d->in_sample = 0;
  d->failed = 0;
}

/* An MPEG-4 descriptor's length: up to four bytes, seven bits each. */
static uint32_t desc_len(const uint8_t **p, const uint8_t *e) {
  uint32_t v = 0;
  for (int i = 0; i < 4 && *p < e; i++) {
    uint8_t c = *(*p)++;
    v = (v << 7) | (c & 0x7Fu);
    if (!(c & 0x80u))
      break;
  }
  return v;
}

/* esds: ES_Descriptor(3) > DecoderConfigDescriptor(4) > DecoderSpecificInfo(5). */
static int parse_esds(Fmp4 *d, const uint8_t *p, uint32_t n) {
  const uint8_t *e = p + n;
  if (n < 4u)
    return 0;
  p += 4;
  while (p < e) {
    uint8_t tag = *p++;
    uint32_t len = desc_len(&p, e);
    if (tag == 3u) {
      uint8_t fl;
      if (p + 3 > e)
        return 0;
      fl = p[2];
      p += 3;
      if (fl & 0x80u)
        p += 2;
      if ((fl & 0x40u) && p < e)
        p += 1u + *p;
      if (fl & 0x20u)
        p += 2;
    } else if (tag == 4u) {
      if (p + 13 > e)
        return 0;
      p += 13;
    } else if (tag == 5u) {
      if (len > FMP4_EXTRA_MAX || p + len > e)
        return 0;
      memcpy(d->extra, p, len);
      d->extra_len = len;
      return len != 0;
    } else {
      p += len;
    }
  }
  return 0;
}

static int parse_stsd(Fmp4 *d, const uint8_t *p, uint32_t n) {
  uint32_t t, l;
  const uint8_t *b;
  if (n < 8u)
    return 0;
  p += 8;
  n -= 8;
  if (!box_at(p, n, &t, &b, &l))
    return 0;
  if (t == TAG('a', 'v', 'c', '1') || t == TAG('a', 'v', 'c', '3')) {
    const uint8_t *c;
    uint32_t cl;
    if (l < 78u)
      return 0;
    d->width = be16(b + 24);
    d->height = be16(b + 26);
    c = find_box(b + 78, l - 78u, TAG('a', 'v', 'c', 'C'), &cl);
    if (!c || cl > FMP4_EXTRA_MAX || !cl)
      return 0;
    memcpy(d->extra, c, cl);
    d->extra_len = cl;
    d->is_audio = 0;
    return 1;
  }
  if (t == TAG('m', 'p', '4', 'a')) {
    const uint8_t *c;
    uint32_t cl;
    if (l < 28u)
      return 0;
    d->channels = be16(b + 16);
    d->rate = be32(b + 24) >> 16;
    d->is_audio = 1;
    c = find_box(b + 28, l - 28u, TAG('e', 's', 'd', 's'), &cl);
    return c && parse_esds(d, c, cl);
  }
  return 0;
}

int fmp4_parse_init(Fmp4 *d, const uint8_t *p, uint32_t n) {
  uint32_t l, ml;
  const uint8_t *moov = find_box(p, n, TAG('m', 'o', 'o', 'v'), &ml), *b;
  const uint8_t *trak, *mdia, *mdhd, *minf, *stbl, *stsd;
  uint32_t tl, mdl, minl, sbl;
  if (!moov)
    return 0;
  trak = find_box(moov, ml, TAG('t', 'r', 'a', 'k'), &tl);
  if (!trak)
    return 0;
  mdia = find_box(trak, tl, TAG('m', 'd', 'i', 'a'), &mdl);
  if (!mdia)
    return 0;
  mdhd = find_box(mdia, mdl, TAG('m', 'd', 'h', 'd'), &l);
  if (!mdhd || l < 24u)
    return 0;
  d->timescale = mdhd[0] == 1u ? be32(mdhd + 20) : be32(mdhd + 12);
  if (!d->timescale)
    return 0;
  minf = find_box(mdia, mdl, TAG('m', 'i', 'n', 'f'), &minl);
  if (!minf)
    return 0;
  stbl = find_box(minf, minl, TAG('s', 't', 'b', 'l'), &sbl);
  if (!stbl)
    return 0;
  stsd = find_box(stbl, sbl, TAG('s', 't', 's', 'd'), &l);
  if (!stsd || !parse_stsd(d, stsd, l))
    return 0;
  b = find_box(moov, ml, TAG('m', 'v', 'e', 'x'), &l);
  if (b) {
    uint32_t tr;
    const uint8_t *trex = find_box(b, l, TAG('t', 'r', 'e', 'x'), &tr);
    if (trex && tr >= 24u) {
      d->trex_duration = be32(trex + 12);
      d->trex_size = be32(trex + 16);
      d->trex_flags = be32(trex + 20);
    }
  }
  return 1;
}

uint32_t fmp4_parse_sidx(const Fmp4 *d, const uint8_t *p, uint32_t n, uint64_t at,
                         Fmp4Segment *out, uint32_t max) {
  uint32_t l, ver, scale, count, i = 0, done = 0;
  const uint8_t *b = find_box(p, n, TAG('s', 'i', 'd', 'x'), &l), *q;
  uint64_t t, off;
  (void)d;
  if (!b || l < 12u)
    return 0;
  ver = b[0];
  scale = be32(b + 8);
  if (!scale)
    return 0;
  if (ver == 0u) {
    if (l < 24u)
      return 0;
    t = be32(b + 12);
    off = be32(b + 16);
    q = b + 20;
  } else {
    if (l < 32u)
      return 0;
    t = be64(b + 12);
    off = be64(b + 20);
    q = b + 28;
  }
  count = be16(q + 2);
  q += 4;
  /* Offsets count from the first byte after the sidx box. */
  off += at + (uint64_t)(b + l - p);
  while (i < count && done < max && (uint32_t)(q - b) + 12u <= l) {
    uint32_t size = be32(q) & 0x7FFFFFFFu, dur = be32(q + 4);
    out[done].offset = off;
    out[done].start_ms = (int32_t)(t * 1000u / scale);
    done++;
    off += size;
    t += dur;
    q += 12;
    i++;
  }
  return done;
}

/* A whole moof into the sample table. */
static int parse_moof(Fmp4 *d, const uint8_t *p, uint32_t n, uint32_t moof_size) {
  uint32_t tl, l, flags, count;
  const uint8_t *traf = find_box(p, n, TAG('t', 'r', 'a', 'f'), &tl), *b, *q;
  uint32_t dur = d->trex_duration, size = d->trex_size, sflags = d->trex_flags;
  uint64_t dts = 0;
  int64_t data_offset = (int64_t)moof_size + 8;

  if (!traf)
    return 0;
  b = find_box(traf, tl, TAG('t', 'f', 'h', 'd'), &l);
  if (!b || l < 8u)
    return 0;
  flags = be32(b);
  q = b + 8;
  if (flags & 0x000001u)
    q += 8;
  if (flags & 0x000002u)
    q += 4;
  if (flags & 0x000008u) {
    dur = be32(q);
    q += 4;
  }
  if (flags & 0x000010u) {
    size = be32(q);
    q += 4;
  }
  if (flags & 0x000020u) {
    sflags = be32(q);
    q += 4;
  }
  b = find_box(traf, tl, TAG('t', 'f', 'd', 't'), &l);
  if (b && l >= 8u)
    dts = b[0] == 1u && l >= 12u ? be64(b + 4) : be32(b + 4);

  b = find_box(traf, tl, TAG('t', 'r', 'u', 'n'), &l);
  if (!b || l < 8u)
    return 0;
  {
    uint32_t ver = b[0], f = be32(b) & 0xFFFFFFu, first = 0;
    int have_first = 0;
    count = be32(b + 4);
    q = b + 8;
    if (count > d->samples_cap)
      return 0;
    if (f & 0x000001u) {
      data_offset = (int32_t)be32(q);
      q += 4;
    }
    if (f & 0x000004u) {
      first = be32(q);
      have_first = 1;
      q += 4;
    }
    for (uint32_t i = 0; i < count; i++) {
      uint32_t sd = dur, ss = size, sf = sflags;
      int32_t cts = 0;
      if (f & 0x000100u) {
        sd = be32(q);
        q += 4;
      }
      if (f & 0x000200u) {
        ss = be32(q);
        q += 4;
      }
      if (f & 0x000400u) {
        sf = be32(q);
        q += 4;
      }
      if (f & 0x000800u) {
        cts = (int32_t)be32(q);
        q += 4;
        if (!ver && cts < 0)
          cts = (int32_t)(uint32_t)cts;
      }
      if (i == 0u && have_first)
        sf = first;
      d->samples[i].size = ss;
      d->samples[i].pts_ms = (int32_t)(((int64_t)dts + cts) * 1000 / d->timescale);
      /* sample_is_non_sync_sample */
      d->samples[i].flags = (sf & 0x00010000u) ? 0u : FMP4_SAMPLE_KEY;
      dts += sd;
    }
  }
  d->n_samples = count;
  d->sample_i = 0;
  d->sample_done = 0;
  d->in_sample = 0;
  /* What lies between the mdat's header and the first sample. */
  d->mdat_skip = data_offset > (int64_t)moof_size + 8
                     ? (uint64_t)(data_offset - (int64_t)moof_size - 8)
                     : 0u;
  return 1;
}

uint32_t fmp4_feed(Fmp4 *d, const uint8_t *p, uint32_t n, const Fmp4Sink *sink) {
  uint32_t used = 0;
  while (used < n && !d->failed) {
    uint32_t left = n - used, k;
    switch (d->state) {
      case ST_HEAD:
        k = d->head_need - d->head_have;
        if (k > left)
          k = left;
        memcpy(d->head + d->head_have, p + used, k);
        d->head_have += k;
        used += k;
        if (d->head_have < d->head_need)
          break;
        {
          uint64_t size = be32(d->head);
          uint32_t type = be32(d->head + 4);
          if (size == 1u) {
            if (d->head_need == 8u) {
              d->head_need = 16u;
              break;
            }
            size = be64(d->head + 8);
          }
          if (size < d->head_need) {
            d->failed = 1;
            break;
          }
          d->box_size = size;
          d->box_done = d->head_need;
          d->head_have = 0;
          d->head_need = 8u;
          if (type == TAG('m', 'o', 'o', 'f')) {
            if (size > d->moof_cap) {
              d->failed = 1;
              break;
            }
            d->moof_have = 0;
            d->state = ST_MOOF;
          } else if (type == TAG('m', 'd', 'a', 't')) {
            d->state = ST_MDAT;
          } else {
            d->state = ST_SKIP;
          }
          if (d->box_done == d->box_size && d->state != ST_MOOF)
            d->state = ST_HEAD;
        }
        break;
      case ST_MOOF:
        k = (uint32_t)(d->box_size - d->box_done);
        if (k > left)
          k = left;
        memcpy(d->moof + d->moof_have, p + used, k);
        d->moof_have += k;
        d->box_done += k;
        used += k;
        if (d->box_done == d->box_size) {
          if (!parse_moof(d, d->moof, d->moof_have, (uint32_t)d->box_size))
            d->failed = 1;
          d->state = ST_HEAD;
        }
        break;
      case ST_SKIP:
        k = d->box_size - d->box_done > left ? left
                                             : (uint32_t)(d->box_size - d->box_done);
        d->box_done += k;
        used += k;
        if (d->box_done == d->box_size)
          d->state = ST_HEAD;
        break;
      case ST_MDAT: {
        uint64_t in_box = d->box_size - d->box_done;
        if (d->mdat_skip) {
          k = d->mdat_skip > left ? left : (uint32_t)d->mdat_skip;
          if (k > in_box)
            k = (uint32_t)in_box;
          d->mdat_skip -= k;
        } else if (d->sample_i < d->n_samples) {
          const Fmp4Sample *s = &d->samples[d->sample_i];
          if (!d->in_sample) {
            if (!sink->begin(sink->ctx, s->size, s->pts_ms,
                             (s->flags & FMP4_SAMPLE_KEY) != 0))
              return used;
            d->in_sample = 1;
            d->sample_done = 0;
          }
          k = s->size - d->sample_done;
          if (k > left)
            k = left;
          if (k > in_box)
            k = (uint32_t)in_box;
          if (k)
            sink->data(sink->ctx, p + used, k);
          d->sample_done += k;
          if (d->sample_done == s->size) {
            sink->end(sink->ctx);
            d->in_sample = 0;
            d->sample_i++;
          }
        } else {
          /* The rest of the box is not ours. */
          k = in_box > left ? left : (uint32_t)in_box;
        }
        d->box_done += k;
        used += k;
        if (d->box_done == d->box_size)
          d->state = ST_HEAD;
        break;
      }
    }
    /* A box that ended exactly on the last byte waits for the next call. */
  }
  return used;
}
