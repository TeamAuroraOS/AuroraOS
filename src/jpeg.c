/* Baseline and extended sequential JPEG: 8-bit, 1 or 3 components, 4:4:4, 4:2:2
 * or 4:2:0, restart intervals. Progressive and arithmetic coding are rejected.
 * Components decode into full planes in IMAGE_RAW_ADDR, then are upsampled and
 * colour-converted. */

#include "image.h"

#define MAXCOMP 3

typedef struct {
  u8 bits[17];  /* number of codes of each length, 1..16 */
  u8 vals[256];
  int mincode[17], maxcode[18], valptr[17];
  int present;
} Huff;

typedef struct {
  int id, h, v, tq;
  int td, ta;
  int dc;
  u8 *plane;
  int pw, ph; /* plane size, a whole number of MCUs */
} Comp;

typedef struct {
  const u8 *d;
  u32 len, pos;
  u32 buf;
  int cnt;
  int eof;
} Bits;

static const u8 zigzag[64] = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

/* Inside entropy-coded data a literal 0xFF is written as FF 00; any other
 * FFxx is the next marker, which ends the scan. */
static int nextbit(Bits *b) {
  if (b->cnt == 0) {
    if (b->pos >= b->len) {
      b->eof = 1;
      return 0;
    }
    u8 c = b->d[b->pos++];
    if (c == 0xFF) {
      u8 m = (b->pos < b->len) ? b->d[b->pos] : 0xD9;
      if (m == 0x00) {
        b->pos++;
      } else {
        b->eof = 1; /* leave pos on the FF so the caller sees the marker */
        b->pos--;
        return 0;
      }
    }
    b->buf = c;
    b->cnt = 8;
  }
  b->cnt--;
  return (int)((b->buf >> b->cnt) & 1u);
}

static int getbits(Bits *b, int n) {
  int v = 0;
  while (n-- > 0)
    v = (v << 1) | nextbit(b);
  return v;
}

/* JPEG stores coefficients as a magnitude category plus that many bits. */
static int extend(int v, int t) {
  return (t && v < (1 << (t - 1))) ? v - (1 << t) + 1 : v;
}

static void huff_prepare(Huff *h) {
  int code = 0, k = 0;
  for (int l = 1; l <= 16; l++) {
    h->valptr[l] = k;
    h->mincode[l] = code;
    code += h->bits[l];
    k += h->bits[l];
    h->maxcode[l] = code - 1;
    if (!h->bits[l])
      h->maxcode[l] = -1;
    code <<= 1;
  }
  h->maxcode[17] = 0x7FFFFFFF;
}

static int huff_decode(Bits *b, const Huff *h) {
  int code = nextbit(b);
  int l = 1;
  while (l <= 16) {
    if (h->maxcode[l] >= 0 && code <= h->maxcode[l])
      return h->vals[h->valptr[l] + code - h->mincode[l]];
    code = (code << 1) | nextbit(b);
    l++;
    if (b->eof)
      return 0;
  }
  b->eof = 1;
  return 0;
}

/* Separable integer IDCT in the usual scaled-butterfly form: rows at 12-bit
 * fixed point, then columns, with the level shift folded into the final
 * rounding constant. */
#define F(x) ((int)((x) * 4096 + 0.5))

static void idct_1d(int s0, int s1, int s2, int s3, int s4, int s5, int s6,
                    int s7, int *o0, int *o1, int *o2, int *o3, int *o4,
                    int *o5, int *o6, int *o7) {
  int p1, p2, p3, p4, p5, t0, t1, t2, t3, x0, x1, x2, x3;

  p2 = s2;
  p3 = s6;
  p1 = (p2 + p3) * F(0.5411961f);
  t2 = p1 + p3 * F(-1.847759065f);
  t3 = p1 + p2 * F(0.765366865f);
  p2 = s0;
  p3 = s4;
  t0 = (p2 + p3) * 4096;
  t1 = (p2 - p3) * 4096;
  x0 = t0 + t3;
  x3 = t0 - t3;
  x1 = t1 + t2;
  x2 = t1 - t2;

  t0 = s7;
  t1 = s5;
  t2 = s3;
  t3 = s1;
  p3 = t0 + t2;
  p4 = t1 + t3;
  p1 = t0 + t3;
  p2 = t1 + t2;
  p5 = (p3 + p4) * F(1.175875602f);
  t0 = t0 * F(0.298631336f);
  t1 = t1 * F(2.053119869f);
  t2 = t2 * F(3.072711026f);
  t3 = t3 * F(1.501321110f);
  p1 = p5 + p1 * F(-0.899976223f);
  p2 = p5 + p2 * F(-2.562915447f);
  p3 = p3 * F(-1.961570560f);
  p4 = p4 * F(-0.390180644f);
  t3 += p1 + p4;
  t2 += p2 + p3;
  t1 += p2 + p4;
  t0 += p1 + p3;

  *o0 = x0 + t3;
  *o7 = x0 - t3;
  *o1 = x1 + t2;
  *o6 = x1 - t2;
  *o2 = x2 + t1;
  *o5 = x2 - t1;
  *o3 = x3 + t0;
  *o4 = x3 - t0;
}

static u8 clamp8(int v) { return (u8)(v < 0 ? 0 : (v > 255 ? 255 : v)); }

static void idct_block(const int *in, u8 *out, int stride) {
  int tmp[64];
  int i;

  for (i = 0; i < 8; i++) {
    const int *s = in + i * 8;
    int *d = tmp + i * 8;
    /* An all-zero AC row is the common case and collapses to a constant. */
    if (!s[1] && !s[2] && !s[3] && !s[4] && !s[5] && !s[6] && !s[7]) {
      /* The general path below rounds and shifts by 10; the shortcut has to
       * land on the same scale, so 4096 >> 10 is 4. */
      int dc = s[0] * 4;
      d[0] = d[1] = d[2] = d[3] = d[4] = d[5] = d[6] = d[7] = dc;
      continue;
    }
    idct_1d(s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7], &d[0], &d[1], &d[2],
            &d[3], &d[4], &d[5], &d[6], &d[7]);
    for (int k = 0; k < 8; k++)
      d[k] = (d[k] + 512) >> 10;
  }

  for (i = 0; i < 8; i++) {
    int o[8];
    idct_1d(tmp[i], tmp[8 + i], tmp[16 + i], tmp[24 + i], tmp[32 + i],
            tmp[40 + i], tmp[48 + i], tmp[56 + i], &o[0], &o[1], &o[2], &o[3],
            &o[4], &o[5], &o[6], &o[7]);
    for (int k = 0; k < 8; k++)
      out[k * stride + i] = clamp8(((o[k] + (1 << 16)) >> 17) + 128);
  }
}

ImageResult jpeg_decode(const u8 *d, u32 len, u8 *rgb, u32 rgb_max, int *ow,
                        int *oh) {
  static u16 qt[4][64];
  static Huff hdc[4], hac[4];
  Comp comp[MAXCOMP];
  u8 *plane_pool = (u8 *)IMAGE_RAW_ADDR;
  u32 plane_used = 0;
  int ncomp = 0, width = 0, height = 0;
  int hmax = 1, vmax = 1, restart = 0;
  u32 pos = 2; /* past SOI */

  for (int i = 0; i < 4; i++) {
    hdc[i].present = 0;
    hac[i].present = 0;
  }

  for (;;) {
    if (pos + 4 > len)
      return IMG_ERR_DATA;
    if (d[pos] != 0xFF)
      return IMG_ERR_DATA;
    u8 m = d[pos + 1];
    pos += 2;
    if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7))
      continue;
    if (m == 0xD9)
      return IMG_ERR_DATA; /* EOI before a scan */

    if (pos + 2 > len)
      return IMG_ERR_DATA;
    u32 seglen = ((u32)d[pos] << 8) | d[pos + 1];
    if (seglen < 2 || pos + seglen > len)
      return IMG_ERR_DATA;
    const u8 *p = d + pos + 2;
    u32 n = seglen - 2;

    if (m == 0xC0 || m == 0xC1) { /* baseline / extended sequential */
      if (n < 6)
        return IMG_ERR_DATA;
      if (p[0] != 8)
        return IMG_ERR_UNSUPPORTED; /* only 8-bit precision */
      height = ((int)p[1] << 8) | p[2];
      width = ((int)p[3] << 8) | p[4];
      ncomp = p[5];
      if (ncomp != 1 && ncomp != 3)
        return IMG_ERR_UNSUPPORTED;
      if (n < 6 + (u32)ncomp * 3)
        return IMG_ERR_DATA;
      if (width <= 0 || height <= 0 || width > IMAGE_MAX_DIM ||
          height > IMAGE_MAX_DIM)
        return IMG_ERR_TOO_BIG;
      if ((u32)width * (u32)height * 3u > rgb_max)
        return IMG_ERR_TOO_BIG;
      for (int i = 0; i < ncomp; i++) {
        comp[i].id = p[6 + i * 3];
        comp[i].h = p[7 + i * 3] >> 4;
        comp[i].v = p[7 + i * 3] & 15;
        comp[i].tq = p[8 + i * 3];
        comp[i].dc = 0;
        if (comp[i].h < 1 || comp[i].h > 4 || comp[i].v < 1 || comp[i].v > 4 ||
            comp[i].tq > 3)
          return IMG_ERR_UNSUPPORTED;
        if (comp[i].h > hmax) hmax = comp[i].h;
        if (comp[i].v > vmax) vmax = comp[i].v;
      }
    } else if (m == 0xC2 || m == 0xC3 || (m >= 0xC5 && m <= 0xCF && m != 0xC8 &&
                                          m != 0xCC)) {
      return IMG_ERR_UNSUPPORTED; /* progressive, lossless, arithmetic */
    } else if (m == 0xC4) {       /* DHT */
      u32 o = 0;
      while (o + 17 <= n) {
        int tc = p[o] >> 4, th = p[o] & 15;
        if (tc > 1 || th > 3)
          return IMG_ERR_UNSUPPORTED;
        Huff *h = tc ? &hac[th] : &hdc[th];
        int total = 0;
        h->bits[0] = 0;
        for (int i = 1; i <= 16; i++) {
          h->bits[i] = p[o + i];
          total += h->bits[i];
        }
        if (total > 256 || o + 17 + (u32)total > n)
          return IMG_ERR_DATA;
        for (int i = 0; i < total; i++)
          h->vals[i] = p[o + 17 + i];
        huff_prepare(h);
        h->present = 1;
        o += 17 + (u32)total;
      }
    } else if (m == 0xDB) { /* DQT */
      u32 o = 0;
      while (o < n) {
        int prec = p[o] >> 4, id = p[o] & 15;
        if (id > 3)
          return IMG_ERR_DATA;
        o++;
        for (int i = 0; i < 64; i++) {
          if (o >= n)
            return IMG_ERR_DATA;
          if (prec) {
            qt[id][i] = (u16)(((u32)p[o] << 8) | p[o + 1]);
            o += 2;
          } else {
            qt[id][i] = p[o++];
          }
        }
      }
    } else if (m == 0xDD) { /* DRI */
      if (n < 2)
        return IMG_ERR_DATA;
      restart = ((int)p[0] << 8) | p[1];
    } else if (m == 0xDA) { /* SOS: the scan follows this segment */
      if (n < 1 || !width)
        return IMG_ERR_DATA;
      int ns = p[0];
      if (ns != ncomp || n < 1 + (u32)ns * 2 + 3)
        return IMG_ERR_UNSUPPORTED; /* non-interleaved scans are progressive */
      for (int i = 0; i < ns; i++) {
        int cid = p[1 + i * 2];
        int k = -1;
        for (int j = 0; j < ncomp; j++)
          if (comp[j].id == cid)
            k = j;
        if (k < 0)
          return IMG_ERR_DATA;
        comp[k].td = p[2 + i * 2] >> 4;
        comp[k].ta = p[2 + i * 2] & 15;
        if (comp[k].td > 3 || comp[k].ta > 3)
          return IMG_ERR_DATA;
      }
      pos += seglen;

      int mcux = (width + 8 * hmax - 1) / (8 * hmax);
      int mcuy = (height + 8 * vmax - 1) / (8 * vmax);

      for (int i = 0; i < ncomp; i++) {
        comp[i].pw = mcux * comp[i].h * 8;
        comp[i].ph = mcuy * comp[i].v * 8;
        u32 need = (u32)comp[i].pw * (u32)comp[i].ph;
        if (need > IMAGE_RAW_MAX || plane_used > IMAGE_RAW_MAX - need)
          return IMG_ERR_TOO_BIG;
        comp[i].plane = plane_pool + plane_used;
        plane_used += need;
        comp[i].dc = 0;
      }

      Bits b;
      b.d = d;
      b.len = len;
      b.pos = pos;
      b.buf = 0;
      b.cnt = 0;
      b.eof = 0;

      int todo = restart;
      for (int my = 0; my < mcuy; my++) {
        for (int mx = 0; mx < mcux; mx++) {
          if (restart && todo == 0) {
            /* Resynchronise: drop partial bits, step over RSTn, reset DC. */
            b.cnt = 0;
            while (b.pos + 1 < b.len &&
                   !(b.d[b.pos] == 0xFF && b.d[b.pos + 1] >= 0xD0 &&
                     b.d[b.pos + 1] <= 0xD7))
              b.pos++;
            if (b.pos + 1 < b.len)
              b.pos += 2;
            b.eof = 0;
            for (int i = 0; i < ncomp; i++)
              comp[i].dc = 0;
            todo = restart;
          }

          for (int c = 0; c < ncomp; c++) {
            const Huff *hd = &hdc[comp[c].td];
            const Huff *ha = &hac[comp[c].ta];
            const u16 *q = qt[comp[c].tq];
            if (!hd->present || !ha->present)
              return IMG_ERR_DATA;

            for (int by = 0; by < comp[c].v; by++) {
              for (int bx = 0; bx < comp[c].h; bx++) {
                int blk[64];
                for (int i = 0; i < 64; i++)
                  blk[i] = 0;

                int t = huff_decode(&b, hd);
                if (t > 15)
                  return IMG_ERR_DATA;
                int diff = t ? extend(getbits(&b, t), t) : 0;
                comp[c].dc += diff;
                blk[0] = comp[c].dc * (int)q[0];

                for (int k = 1; k < 64;) {
                  int rs = huff_decode(&b, ha);
                  int r = rs >> 4, s = rs & 15;
                  if (!s) {
                    if (r != 15)
                      break; /* EOB */
                    k += 16;
                    continue;
                  }
                  k += r;
                  if (k > 63)
                    break;
                  blk[zigzag[k]] = extend(getbits(&b, s), s) * (int)q[k];
                  k++;
                }
                if (b.eof && (my * mcux + mx) < mcux * mcuy - 1) {
                  /* Truncated file: keep what decoded rather than fail. */
                  my = mcuy;
                  mx = mcux;
                }

                int px = (mx * comp[c].h + bx) * 8;
                int py = (my * comp[c].v + by) * 8;
                if (px + 8 <= comp[c].pw && py + 8 <= comp[c].ph)
                  idct_block(blk, comp[c].plane + (u32)py * (u32)comp[c].pw + px,
                             comp[c].pw);
              }
            }
          }
          if (restart)
            todo--;
        }
      }

      for (int y = 0; y < height; y++) {
        u8 *out = rgb + (u32)y * (u32)width * 3u;
        for (int x = 0; x < width; x++) {
          if (ncomp == 1) {
            const Comp *c = &comp[0];
            u8 v = c->plane[(u32)y * (u32)c->pw + x];
            *out++ = v;
            *out++ = v;
            *out++ = v;
          } else {
            int s[3];
            for (int i = 0; i < 3; i++) {
              const Comp *c = &comp[i];
              int sx = x * c->h / hmax, sy = y * c->v / vmax;
              if (sx >= c->pw) sx = c->pw - 1;
              if (sy >= c->ph) sy = c->ph - 1;
              s[i] = c->plane[(u32)sy * (u32)c->pw + sx];
            }
            int Y = s[0], cb = s[1] - 128, cr = s[2] - 128;
            *out++ = clamp8(Y + ((91881 * cr) >> 16));
            *out++ = clamp8(Y - ((22554 * cb + 46802 * cr) >> 16));
            *out++ = clamp8(Y + ((116130 * cb) >> 16));
          }
        }
      }
      *ow = width;
      *oh = height;
      return IMG_OK;
    } else if (m == 0xDE || m == 0xDF) {
      return IMG_ERR_UNSUPPORTED; /* hierarchical */
    }
    pos += seglen;
  }
}
