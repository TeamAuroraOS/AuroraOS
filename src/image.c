/* BMP and PNG decoding (JPEG is in jpeg.c): for the image viewer, to RGB888 at
 * IMAGE_RGB_ADDR with alpha composited onto the panel colour, since the
 * framebuffer has none; for textures, to RGBA at a smaller size
 * (image_decode_fit). Every length is checked against the buffer caps, so a
 * malformed file fails instead of overrunning. */

#include "image.h"
#include "ff.h"

#define BG_R 0x39
#define BG_G 0x39
#define BG_B 0x3D

static u8 *const file_buf = (u8 *)IMAGE_FILE_ADDR;
static u8 *const raw_buf = (u8 *)IMAGE_RAW_ADDR;
static u8 *const rgb_buf = (u8 *)IMAGE_RGB_ADDR;

const char *image_error(ImageResult r) {
  switch (r) {
    case IMG_OK:              return "";
    case IMG_ERR_OPEN:        return "Could not read the file";
    case IMG_ERR_TOO_BIG:     return "Image is too large";
    case IMG_ERR_FORMAT:      return "Not a picture file";
    case IMG_ERR_UNSUPPORTED: return "Unsupported variant";
    default:                  return "File is damaged";
  }
}

static u32 rd16le(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8); }
static u32 rd32le(const u8 *p) {
  return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}
static u32 rd32be(const u8 *p) {
  return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | (u32)p[3];
}

static int size_ok(int w, int h) {
  if (w <= 0 || h <= 0 || w > IMAGE_MAX_DIM || h > IMAGE_MAX_DIM)
    return 0;
  return (u32)w * (u32)h <= IMAGE_MAX_PIXELS;
}

/* Uncompressed 8/24/32bpp, which is what every tool writes by default. RLE and
 * the 16bpp bitfield forms are rejected rather than half-supported. */
static ImageResult decode_bmp(const u8 *d, u32 len, int *ow, int *oh) {
  if (len < 54)
    return IMG_ERR_DATA;
  u32 data_off = rd32le(d + 10);
  u32 hdr = rd32le(d + 14);
  if (hdr < 40)
    return IMG_ERR_UNSUPPORTED;

  int w = (int)rd32le(d + 18);
  int h = (int)rd32le(d + 22);
  int flip = 1; /* BMP rows run bottom-up unless the height is negative */
  if (h < 0) {
    h = -h;
    flip = 0;
  }
  u32 bpp = rd16le(d + 28);
  u32 comp = rd32le(d + 30);
  if (comp != 0)
    return IMG_ERR_UNSUPPORTED;
  if (bpp != 8 && bpp != 24 && bpp != 32)
    return IMG_ERR_UNSUPPORTED;
  if (!size_ok(w, h))
    return IMG_ERR_TOO_BIG;

  const u8 *pal = d + 14 + hdr;
  u32 pal_n = rd32le(d + 46);
  if (bpp == 8 && pal_n == 0)
    pal_n = 256;

  u32 stride = (((u32)w * bpp + 31u) / 32u) * 4u;
  /* Division rather than stride*h, which could overflow before the compare. */
  if (data_off > len || stride > (len - data_off) / (u32)h)
    return IMG_ERR_DATA;

  for (int y = 0; y < h; y++) {
    const u8 *row = d + data_off + stride * (u32)(flip ? (h - 1 - y) : y);
    u8 *out = rgb_buf + (u32)y * (u32)w * 3u;
    for (int x = 0; x < w; x++) {
      u8 r, g, b;
      if (bpp == 8) {
        u32 i = row[x];
        if (i >= pal_n)
          i = 0;
        b = pal[i * 4 + 0];
        g = pal[i * 4 + 1];
        r = pal[i * 4 + 2];
      } else {
        const u8 *p = row + (u32)x * (bpp / 8u);
        b = p[0];
        g = p[1];
        r = p[2];
      }
      *out++ = r;
      *out++ = g;
      *out++ = b;
    }
  }
  *ow = w;
  *oh = h;
  return IMG_OK;
}

typedef struct {
  const u8 *src;
  u32 len, pos;
  u32 bitbuf;
  int bitcnt;
  int err;
} BitIn;

static int bits(BitIn *b, int n) {
  while (b->bitcnt < n) {
    if (b->pos >= b->len) {
      b->err = 1;
      return 0;
    }
    b->bitbuf |= (u32)b->src[b->pos++] << b->bitcnt;
    b->bitcnt += 8;
  }
  int v = (int)(b->bitbuf & ((1u << n) - 1u));
  b->bitbuf >>= n;
  b->bitcnt -= n;
  return v;
}

/* Canonical Huffman, decoded a bit at a time against per-length counts. */
typedef struct {
  u16 count[16];
  u16 sym[288];
} Huff;

static void huff_build(Huff *h, const u8 *lens, int n) {
  int offs[16], i;
  for (i = 0; i < 16; i++)
    h->count[i] = 0;
  for (i = 0; i < n; i++)
    h->count[lens[i]]++;
  h->count[0] = 0;
  offs[0] = 0;
  for (i = 1; i < 16; i++)
    offs[i] = offs[i - 1] + h->count[i - 1];
  for (i = 0; i < n; i++)
    if (lens[i])
      h->sym[offs[lens[i]]++] = (u16)i;
}

static int huff_get(BitIn *b, const Huff *h) {
  int code = 0, first = 0, index = 0;
  for (int len = 1; len < 16; len++) {
    code |= bits(b, 1);
    if (b->err)
      return -1;
    int count = h->count[len];
    if (code - first < count)
      return h->sym[index + (code - first)];
    index += count;
    first = (first + count) << 1;
    code <<= 1;
  }
  b->err = 1;
  return -1;
}

static const u16 len_base[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,
                                 15, 17, 19, 23, 27, 31, 35, 43, 51,  59,
                                 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const u8 len_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const u16 dist_base[30] = {1,    2,    3,    4,    5,    7,     9,
                                  13,   17,   25,   33,   49,   65,    97,
                                  129,  193,  257,  385,  513,  769,   1025,
                                  1537, 2049, 3073, 4097, 6145, 8193,  12289,
                                  16385, 24577};
static const u8 dist_extra[30] = {0, 0, 0,  0,  1,  1,  2,  2,  3,  3,
                                  4, 4, 5,  5,  6,  6,  7,  7,  8,  8,
                                  9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

/* Where a streaming inflate sends its output: a 32 KB ring holds the window
 * back-references read from, and `sink` gets each stretch as it fills. */
typedef struct {
  u8 *win; /* 32 KB */
  u32 flushed;
  void (*sink)(void *ctx, const u8 *p, u32 n);
  void *ctx;
} Ring;

#define RING_MASK 0x7FFFu

static void ring_flush(Ring *r, u32 out) {
  while (r->flushed < out) {
    u32 at = r->flushed & RING_MASK, n = out - r->flushed;
    if (n > RING_MASK + 1u - at)
      n = RING_MASK + 1u - at;
    r->sink(r->ctx, r->win + at, n);
    r->flushed += n;
  }
}

/* Without a ring, back-references read from the output buffer itself: the
 * whole decompressed stream is resident. With one, `dst` is its window and
 * the stream may be any length. */
static u32 inflate_to(const u8 *src, u32 len, u8 *dst, u32 dst_max, Ring *ring,
                      int *err) {
  BitIn b;
  Huff lit, dist;
  u32 out = 0, mask = ring ? RING_MASK : 0xFFFFFFFFu;
  if (ring) {
    dst = ring->win;
    dst_max = 0xFFFFFFFFu;
    ring->flushed = 0;
  }
  b.src = src;
  b.len = len;
  b.pos = 0;
  b.bitbuf = 0;
  b.bitcnt = 0;
  b.err = 0;
  *err = 0;

  for (;;) {
    int final = bits(&b, 1);
    int type = bits(&b, 2);
    if (b.err)
      break;

    if (type == 0) { /* stored */
      b.bitbuf = 0;
      b.bitcnt = 0;
      if (b.pos + 4 > b.len) {
        b.err = 1;
        break;
      }
      u32 n = rd16le(b.src + b.pos);
      b.pos += 4;
      if (b.pos + n > b.len || out + n > dst_max) {
        b.err = 1;
        break;
      }
      for (u32 i = 0; i < n; i++) {
        dst[out & mask] = b.src[b.pos++];
        out++;
        if (ring && out - ring->flushed >= 0x4000u)
          ring_flush(ring, out);
      }
    } else if (type == 1 || type == 2) {
      u8 lens[320];
      int nlit, ndist;
      if (type == 1) {
        int i;
        for (i = 0; i < 144; i++) lens[i] = 8;
        for (; i < 256; i++)      lens[i] = 9;
        for (; i < 280; i++)      lens[i] = 7;
        for (; i < 288; i++)      lens[i] = 8;
        for (i = 0; i < 30; i++)  lens[288 + i] = 5;
        nlit = 288;
        ndist = 30;
      } else {
        static const u8 ord[19] = {16, 17, 18, 0, 8,  7, 9,  6, 10, 5,
                                   11, 4,  12, 3, 13, 2, 14, 1, 15};
        u8 clens[19];
        Huff cl;
        nlit = bits(&b, 5) + 257;
        ndist = bits(&b, 5) + 1;
        int ncl = bits(&b, 4) + 4;
        if (b.err || nlit > 288 || ndist > 30)
          break;
        for (int i = 0; i < 19; i++)
          clens[i] = 0;
        for (int i = 0; i < ncl; i++)
          clens[ord[i]] = (u8)bits(&b, 3);
        huff_build(&cl, clens, 19);
        int n = 0;
        while (n < nlit + ndist) {
          int s = huff_get(&b, &cl);
          if (s < 0)
            break;
          if (s < 16) {
            lens[n++] = (u8)s;
          } else if (s == 16) {
            if (!n)
              break;
            u8 prev = lens[n - 1];
            int r = 3 + bits(&b, 2);
            while (r-- && n < nlit + ndist)
              lens[n++] = prev;
          } else if (s == 17) {
            int r = 3 + bits(&b, 3);
            while (r-- && n < nlit + ndist)
              lens[n++] = 0;
          } else {
            int r = 11 + bits(&b, 7);
            while (r-- && n < nlit + ndist)
              lens[n++] = 0;
          }
        }
        if (b.err || n != nlit + ndist)
          break;
      }
      huff_build(&lit, lens, nlit);
      huff_build(&dist, lens + nlit, ndist);

      for (;;) {
        int s = huff_get(&b, &lit);
        if (s < 0)
          break;
        if (s < 256) {
          if (out >= dst_max) {
            b.err = 1;
            break;
          }
          dst[out & mask] = (u8)s;
          out++;
        } else if (s == 256) {
          break;
        } else {
          s -= 257;
          if (s >= 29) {
            b.err = 1;
            break;
          }
          u32 l = len_base[s] + (u32)bits(&b, len_extra[s]);
          int ds = huff_get(&b, &dist);
          if (ds < 0 || ds >= 30) {
            b.err = 1;
            break;
          }
          u32 dd = dist_base[ds] + (u32)bits(&b, dist_extra[ds]);
          if (dd > out || out + l > dst_max) {
            b.err = 1;
            break;
          }
          u32 from = out - dd;
          while (l--) {
            dst[out & mask] = dst[from & mask];
            out++;
            from++;
          }
        }
        /* At most 258 bytes went in; the ring has room for far more. */
        if (ring && out - ring->flushed >= 0x4000u)
          ring_flush(ring, out);
      }
      if (b.err)
        break;
    } else {
      b.err = 1;
      break;
    }
    if (final)
      break;
  }
  if (ring)
    ring_flush(ring, out);
  *err = b.err;
  return out;
}

static u32 inflate(const u8 *src, u32 len, u8 *dst, u32 dst_max, int *err) {
  return inflate_to(src, len, dst, dst_max, 0, err);
}

static int paeth(int a, int b, int c) {
  int p = a + b - c;
  int pa = p > a ? p - a : a - p;
  int pb = p > b ? p - b : b - p;
  int pc = p > c ? p - c : c - p;
  if (pa <= pb && pa <= pc)
    return a;
  return pb <= pc ? b : c;
}

static u32 sample(const u8 *row, int depth, int idx) {
  switch (depth) {
    case 8:  return row[idx];
    case 16: return row[idx * 2]; /* keep the high byte: 16bpc drops to 8 */
    case 4:  return (row[idx >> 1] >> (idx & 1 ? 0 : 4)) & 0x0F;
    case 2:  return (row[idx >> 2] >> (6 - 2 * (idx & 3))) & 0x03;
    default: return (row[idx >> 3] >> (7 - (idx & 7))) & 0x01;
  }
}

/* To `dst` as RGB888, alpha composited onto the panel colour, or with `rgba`
 * set as RGBA8888 with its alpha kept. */
static ImageResult decode_png(const u8 *d, u32 len, u8 *dst, u32 dst_max,
                              int rgba, int *ow, int *oh) {
  if (len < 8)
    return IMG_ERR_DATA;

  int w = 0, h = 0, depth = 0, ctype = 0;
  const u8 *plte = 0;
  u32 plte_n = 0;
  const u8 *trns = 0;
  u32 trns_n = 0;
  u32 idat_len = 0;
  u32 pos = 8;

  /* IDAT chunks must be concatenated before inflating; they are gathered at
   * the top of the raw buffer and the scanlines decoded below them. */
  u8 *idat = raw_buf;

  while (pos + 8 <= len) {
    u32 clen = rd32be(d + pos);
    const u8 *tag = d + pos + 4;
    const u8 *body = d + pos + 8;
    if (clen > len || pos + 12 + clen > len)
      return IMG_ERR_DATA;

    if (tag[0] == 'I' && tag[1] == 'H' && tag[2] == 'D' && tag[3] == 'R') {
      if (clen < 13)
        return IMG_ERR_DATA;
      w = (int)rd32be(body);
      h = (int)rd32be(body + 4);
      depth = body[8];
      ctype = body[9];
      if (body[12])
        return IMG_ERR_UNSUPPORTED; /* interlaced */
      if (!size_ok(w, h))
        return IMG_ERR_TOO_BIG;
    } else if (tag[0] == 'P' && tag[1] == 'L' && tag[2] == 'T' && tag[3] == 'E') {
      plte = body;
      plte_n = clen / 3;
    } else if (tag[0] == 't' && tag[1] == 'R' && tag[2] == 'N' && tag[3] == 'S') {
      trns = body;
      trns_n = clen;
    } else if (tag[0] == 'I' && tag[1] == 'D' && tag[2] == 'A' && tag[3] == 'T') {
      if (idat_len + clen > IMAGE_RAW_MAX / 2u)
        return IMG_ERR_TOO_BIG;
      for (u32 i = 0; i < clen; i++)
        idat[idat_len + i] = body[i];
      idat_len += clen;
    } else if (tag[0] == 'I' && tag[1] == 'E' && tag[2] == 'N' && tag[3] == 'D') {
      break;
    }
    pos += 12 + clen;
  }

  if (!w || !idat_len)
    return IMG_ERR_DATA;
  if ((u32)w * (u32)h > dst_max / (rgba ? 4u : 3u))
    return IMG_ERR_TOO_BIG;
  if (depth != 1 && depth != 2 && depth != 4 && depth != 8 && depth != 16)
    return IMG_ERR_UNSUPPORTED;
  if (ctype != 0 && ctype != 2 && ctype != 3 && ctype != 4 && ctype != 6)
    return IMG_ERR_UNSUPPORTED;
  if (ctype == 3 && !plte)
    return IMG_ERR_DATA;

  int chans = (ctype == 2) ? 3 : (ctype == 4) ? 2 : (ctype == 6) ? 4 : 1;
  u32 bpl = ((u32)w * (u32)chans * (u32)depth + 7u) / 8u; /* bytes per line */
  int fbpp = (chans * depth + 7) / 8;                     /* filter step     */

  u8 *scan = raw_buf + ((idat_len + 15u) & ~15u);
  u32 scan_max = IMAGE_RAW_MAX - (u32)(scan - raw_buf);
  if ((bpl + 1u) > scan_max / (u32)h)
    return IMG_ERR_TOO_BIG;

  /* Skip the 2-byte zlib header; the adler32 trailer is not checked. */
  if (idat_len < 3)
    return IMG_ERR_DATA;
  int err = 0;
  u32 got = inflate(idat + 2, idat_len - 2, scan, scan_max, &err);
  (void)err; /* a short stream is the failure that matters, not how it ended */
  if (got < (bpl + 1u) * (u32)h)
    return IMG_ERR_DATA;

  /* Unfilter in place: each row's filter byte is dropped and the row rewritten
   * over the top of it, so the result is bpl-strided. */
  for (int y = 0; y < h; y++) {
    u8 *row = scan + (u32)y * (bpl + 1u);
    int ft = row[0];
    u8 *cur = row + 1;
    const u8 *prev = y ? scan + (u32)(y - 1) * (bpl + 1u) + 1 : 0;
    for (u32 i = 0; i < bpl; i++) {
      int a = (i >= (u32)fbpp) ? cur[i - fbpp] : 0;
      int b = prev ? prev[i] : 0;
      int c = (prev && i >= (u32)fbpp) ? prev[i - fbpp] : 0;
      int x = cur[i];
      switch (ft) {
        case 1: x += a; break;
        case 2: x += b; break;
        case 3: x += (a + b) / 2; break;
        case 4: x += paeth(a, b, c); break;
        default: break;
      }
      cur[i] = (u8)x;
    }
  }

  u32 maxv = (depth >= 8) ? 255u : ((1u << depth) - 1u);
  for (int y = 0; y < h; y++) {
    const u8 *row = scan + (u32)y * (bpl + 1u) + 1;
    u8 *out = dst + (u32)y * (u32)w * (rgba ? 4u : 3u);
    for (int x = 0; x < w; x++) {
      u32 r, g, b, a = 255;
      if (ctype == 3) {
        u32 i = sample(row, depth, x);
        if (i >= plte_n)
          i = 0;
        r = plte[i * 3 + 0];
        g = plte[i * 3 + 1];
        b = plte[i * 3 + 2];
        if (trns && i < trns_n)
          a = trns[i];
      } else if (ctype == 0 || ctype == 4) {
        u32 v = sample(row, depth, x * chans);
        if (depth < 8)
          v = v * 255u / maxv;
        r = g = b = v;
        if (ctype == 4)
          a = sample(row, depth, x * chans + 1);
      } else {
        r = sample(row, depth, x * chans + 0);
        g = sample(row, depth, x * chans + 1);
        b = sample(row, depth, x * chans + 2);
        if (ctype == 6)
          a = sample(row, depth, x * chans + 3);
      }
      if (rgba) {
        *out++ = (u8)r;
        *out++ = (u8)g;
        *out++ = (u8)b;
        *out++ = (u8)a;
        continue;
      }
      if (a != 255) { /* the framebuffer has no alpha; composite now */
        r = (r * a + BG_R * (255u - a)) / 255u;
        g = (g * a + BG_G * (255u - a)) / 255u;
        b = (b * a + BG_B * (255u - a)) / 255u;
      }
      *out++ = (u8)r;
      *out++ = (u8)g;
      *out++ = (u8)b;
    }
  }
  *ow = w;
  *oh = h;
  return IMG_OK;
}

ImageResult image_load(const char *path, Image *out) {
  static FIL f;
  UINT br;
  u32 len;
  ImageResult r;
  int w = 0, h = 0;

  if (f_open(&f, path, FA_READ) != FR_OK)
    return IMG_ERR_OPEN;
  len = (u32)f_size(&f);
  if (len > IMAGE_FILE_MAX) {
    f_close(&f);
    return IMG_ERR_TOO_BIG;
  }
  if (f_read(&f, file_buf, len, &br) != FR_OK || br != len) {
    f_close(&f);
    return IMG_ERR_OPEN;
  }
  f_close(&f);
  if (len < 8)
    return IMG_ERR_FORMAT;

  if (file_buf[0] == 'B' && file_buf[1] == 'M')
    r = decode_bmp(file_buf, len, &w, &h);
  else if (file_buf[0] == 0x89 && file_buf[1] == 'P' && file_buf[2] == 'N' &&
           file_buf[3] == 'G')
    r = decode_png(file_buf, len, rgb_buf, IMAGE_RGB_MAX, 0, &w, &h);
  else if (file_buf[0] == 0xFF && file_buf[1] == 0xD8)
    r = jpeg_decode(file_buf, len, rgb_buf, IMAGE_RGB_MAX, &w, &h);
  else
    return IMG_ERR_FORMAT;

  if (r != IMG_OK)
    return r;
  out->w = w;
  out->h = h;
  out->rgb = rgb_buf;
  return IMG_OK;
}

/* Box-filters rows of RGBA pixels down to tw x th as they arrive, colour
 * weighted by alpha + 1: clear pixels do not darken their neighbours, and an
 * all-clear area keeps its colour, so the GPU's filtering has no dark fringe
 * at cut-out edges. Each output pixel covers at least one source pixel:
 * tw <= sw and th <= sh. */
typedef struct {
  int sw, sh, tw, th, oy;
  u8 *out;
  u32 *acc;  /* r*w, g*w, b*w, w, n for each output pixel of the row */
  u16 *xmap; /* source column -> output column */
} Fit;

static void fit_flush(Fit *f) {
  u8 *o = f->out + (u32)f->oy * (u32)f->tw * 4u;
  for (int x = 0; x < f->tw; x++) {
    u32 *a = f->acc + x * 5;
    u32 n = a[4] ? a[4] : 1u;
    o[0] = (u8)(a[3] ? a[0] / a[3] : 0);
    o[1] = (u8)(a[3] ? a[1] / a[3] : 0);
    o[2] = (u8)(a[3] ? a[2] / a[3] : 0);
    o[3] = (u8)((a[3] - a[4]) / n);
    o += 4;
    a[0] = a[1] = a[2] = a[3] = a[4] = 0;
  }
}

static void fit_row(Fit *f, int sy, const u8 *rgba) {
  int oy = (int)((u32)sy * (u32)f->th / (u32)f->sh);
  if (oy != f->oy) {
    if (f->oy >= 0)
      fit_flush(f);
    f->oy = oy;
  }
  for (int x = 0; x < f->sw; x++, rgba += 4) {
    u32 *a = f->acc + f->xmap[x] * 5u, w = rgba[3] + 1u;
    a[0] += rgba[0] * w;
    a[1] += rgba[1] * w;
    a[2] += rgba[2] * w;
    a[3] += w;
    a[4]++;
  }
}

/* Scratch: xmap, accumulators, then whatever the decoder needs. */
static u8 *fit_init(Fit *f, int sw, int sh, int tw, int th, u8 *out,
                    u8 *scratch) {
  f->sw = sw;
  f->sh = sh;
  f->tw = tw;
  f->th = th;
  f->oy = -1;
  f->out = out;
  f->xmap = (u16 *)scratch;
  f->acc = (u32 *)(scratch + ((u32)sw * 2u + 15u) / 16u * 16u);
  for (int x = 0; x < sw; x++)
    f->xmap[x] = (u16)((u32)x * (u32)tw / (u32)sw);
  for (int i = 0; i < tw * 5; i++)
    f->acc[i] = 0;
  return (u8 *)(f->acc + tw * 5);
}

typedef struct {
  int w, h, depth, ctype, chans, fbpp;
  u32 bpl, fill;
  const u8 *plte, *trns;
  u32 plte_n, trns_n;
  u8 *cur, *prev, *row;
  int y;
  Fit fit;
} PngStream;

static void png_pixel(const PngStream *p, const u8 *line, int x, u8 *o) {
  u32 r, g, b, a = 255;
  u32 maxv = (p->depth >= 8) ? 255u : ((1u << p->depth) - 1u);
  if (p->ctype == 3) {
    u32 i = sample(line, p->depth, x);
    if (i >= p->plte_n)
      i = 0;
    r = p->plte[i * 3 + 0];
    g = p->plte[i * 3 + 1];
    b = p->plte[i * 3 + 2];
    if (p->trns && i < p->trns_n)
      a = p->trns[i];
  } else if (p->ctype == 0 || p->ctype == 4) {
    u32 v = sample(line, p->depth, x * p->chans);
    if (p->depth < 8)
      v = v * 255u / maxv;
    r = g = b = v;
    if (p->ctype == 4)
      a = sample(line, p->depth, x * p->chans + 1);
  } else {
    r = sample(line, p->depth, x * p->chans + 0);
    g = sample(line, p->depth, x * p->chans + 1);
    b = sample(line, p->depth, x * p->chans + 2);
    if (p->ctype == 6)
      a = sample(line, p->depth, x * p->chans + 3);
  }
  o[0] = (u8)r;
  o[1] = (u8)g;
  o[2] = (u8)b;
  o[3] = (u8)a;
}

/* Gets the decompressed stream a piece at a time and turns each finished
 * scanline into pixels for the box filter. */
static void png_sink(void *ctx, const u8 *src, u32 n) {
  PngStream *p = (PngStream *)ctx;
  while (n && p->y < p->h) {
    u32 take = p->bpl + 1u - p->fill;
    if (take > n)
      take = n;
    for (u32 i = 0; i < take; i++)
      p->cur[p->fill + i] = src[i];
    p->fill += take;
    src += take;
    n -= take;
    if (p->fill < p->bpl + 1u)
      break;

    u8 *cur = p->cur + 1;
    const u8 *prev = p->y ? p->prev + 1 : 0;
    int ft = p->cur[0];
    for (u32 i = 0; i < p->bpl; i++) {
      int a = (i >= (u32)p->fbpp) ? cur[i - p->fbpp] : 0;
      int b = prev ? prev[i] : 0;
      int c = (prev && i >= (u32)p->fbpp) ? prev[i - p->fbpp] : 0;
      int x = cur[i];
      switch (ft) {
        case 1: x += a; break;
        case 2: x += b; break;
        case 3: x += (a + b) / 2; break;
        case 4: x += paeth(a, b, c); break;
        default: break;
      }
      cur[i] = (u8)x;
    }
    for (int x = 0; x < p->w; x++)
      png_pixel(p, cur, x, p->row + x * 4);
    fit_row(&p->fit, p->y, p->row);

    u8 *t = p->prev;
    p->prev = p->cur;
    p->cur = t;
    p->fill = 0;
    p->y++;
  }
}

static ImageResult png_stream(const u8 *d, u32 len, u8 *out, int tw, int th,
                              u8 *scratch, u32 scratch_len) {
  PngStream p;
  u32 pos = 8, idat_len = 0;
  u8 *idat = raw_buf;
  Ring ring;
  int err = 0;

  p.w = p.h = p.depth = p.ctype = 0;
  p.plte = p.trns = 0;
  p.plte_n = p.trns_n = 0;
  while (pos + 8 <= len) {
    u32 clen = rd32be(d + pos);
    const u8 *tag = d + pos + 4, *body = d + pos + 8;
    if (clen > len || pos + 12 + clen > len)
      return IMG_ERR_DATA;
    if (tag[0] == 'I' && tag[1] == 'H' && tag[2] == 'D' && tag[3] == 'R') {
      if (clen < 13)
        return IMG_ERR_DATA;
      p.w = (int)rd32be(body);
      p.h = (int)rd32be(body + 4);
      p.depth = body[8];
      p.ctype = body[9];
      if (body[12])
        return IMG_ERR_UNSUPPORTED; /* interlaced */
    } else if (tag[0] == 'P' && tag[1] == 'L' && tag[2] == 'T' && tag[3] == 'E') {
      p.plte = body;
      p.plte_n = clen / 3;
    } else if (tag[0] == 't' && tag[1] == 'R' && tag[2] == 'N' && tag[3] == 'S') {
      p.trns = body;
      p.trns_n = clen;
    } else if (tag[0] == 'I' && tag[1] == 'D' && tag[2] == 'A' && tag[3] == 'T') {
      if (idat_len + clen > IMAGE_RAW_MAX)
        return IMG_ERR_TOO_BIG;
      for (u32 i = 0; i < clen; i++)
        idat[idat_len + i] = body[i];
      idat_len += clen;
    } else if (tag[0] == 'I' && tag[1] == 'E' && tag[2] == 'N' && tag[3] == 'D') {
      break;
    }
    pos += 12 + clen;
  }
  if (p.w <= 0 || p.h <= 0 || p.w > IMAGE_MAX_DIM || p.h > IMAGE_MAX_DIM ||
      idat_len < 3 || tw > p.w || th > p.h)
    return IMG_ERR_DATA;
  if (p.depth != 1 && p.depth != 2 && p.depth != 4 && p.depth != 8 &&
      p.depth != 16)
    return IMG_ERR_UNSUPPORTED;
  if (p.ctype != 0 && p.ctype != 2 && p.ctype != 3 && p.ctype != 4 &&
      p.ctype != 6)
    return IMG_ERR_UNSUPPORTED;
  if (p.ctype == 3 && !p.plte)
    return IMG_ERR_DATA;
  p.chans = (p.ctype == 2) ? 3 : (p.ctype == 4) ? 2 : (p.ctype == 6) ? 4 : 1;
  p.bpl = ((u32)p.w * (u32)p.chans * (u32)p.depth + 7u) / 8u;
  p.fbpp = (p.chans * p.depth + 7) / 8;

  {
    u8 *at = fit_init(&p.fit, p.w, p.h, tw, th, out, scratch);
    u32 line = (p.bpl + 1u + 15u) & ~15u;
    ring.win = at;
    p.cur = at + RING_MASK + 1u;
    p.prev = p.cur + line;
    p.row = p.prev + line;
    if ((u32)(p.row + (u32)p.w * 4u - scratch) > scratch_len)
      return IMG_ERR_TOO_BIG;
  }
  p.fill = 0;
  p.y = 0;
  ring.sink = png_sink;
  ring.ctx = &p;
  /* Skip the 2-byte zlib header; the adler32 trailer is not checked. */
  inflate_to(idat + 2, idat_len - 2, 0, 0, &ring, &err);
  if (p.y < p.h)
    return IMG_ERR_DATA;
  fit_flush(&p.fit);
  return IMG_OK;
}

static ImageResult jpeg_dims(const u8 *d, u32 len, int *w, int *h) {
  u32 pos = 2;
  while (pos + 9 < len) {
    u32 seg;
    if (d[pos] != 0xFF) {
      pos++;
      continue;
    }
    if (d[pos + 1] >= 0xC0 && d[pos + 1] <= 0xCF && d[pos + 1] != 0xC4 &&
        d[pos + 1] != 0xC8 && d[pos + 1] != 0xCC) {
      *h = (d[pos + 5] << 8) | d[pos + 6];
      *w = (d[pos + 7] << 8) | d[pos + 8];
      return IMG_OK;
    }
    seg = ((u32)d[pos + 2] << 8) | d[pos + 3];
    pos += 2 + seg;
  }
  return IMG_ERR_DATA;
}

ImageResult image_probe(const u8 *d, u32 len, int *w, int *h) {
  if (len >= 24 && d[0] == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G') {
    *w = (int)rd32be(d + 16);
    *h = (int)rd32be(d + 20);
    return (*w > 0 && *h > 0) ? IMG_OK : IMG_ERR_DATA;
  }
  if (len >= 4 && d[0] == 0xFF && d[1] == 0xD8)
    return jpeg_dims(d, len, w, h);
  return IMG_ERR_FORMAT;
}

/* Nearest neighbour from an RGB or RGBA image, for the rare target larger
 * than its source. */
static void fit_nearest(const u8 *src, int sw, int sh, int bpp, u8 *out,
                        int tw, int th) {
  for (int y = 0; y < th; y++)
    for (int x = 0; x < tw; x++) {
      const u8 *s = src + ((u32)(y * sh / th) * (u32)sw + (u32)(x * sw / tw)) *
                              (u32)bpp;
      u8 *o = out + ((u32)y * (u32)tw + (u32)x) * 4u;
      o[0] = s[0];
      o[1] = s[1];
      o[2] = s[2];
      o[3] = bpp == 4 ? s[3] : 0xFF;
    }
}

ImageResult image_decode_fit(const u8 *d, u32 len, u8 *out, int tw, int th,
                             u8 *scratch, u32 scratch_len) {
  int w = 0, h = 0;
  ImageResult r = image_probe(d, len, &w, &h);
  if (r != IMG_OK)
    return r;

  if (d[0] == 0x89) {
    if (tw <= w && th <= h)
      return png_stream(d, len, out, tw, th, scratch, scratch_len);
    r = decode_png(d, len, rgb_buf, IMAGE_RGB_MAX, 1, &w, &h);
    if (r == IMG_OK)
      fit_nearest(rgb_buf, w, h, 4, out, tw, th);
    return r;
  }

  /* JPEG: whole if it fits, else from each 8x8 block's average alone. */
  r = jpeg_decode_scaled(d, len, rgb_buf, IMAGE_RGB_MAX, 0, &w, &h);
  if (r == IMG_ERR_TOO_BIG)
    r = jpeg_decode_scaled(d, len, rgb_buf, IMAGE_RGB_MAX, 3, &w, &h);
  if (r != IMG_OK)
    return r;
  if (tw > w || th > h) {
    fit_nearest(rgb_buf, w, h, 3, out, tw, th);
    return IMG_OK;
  }
  {
    Fit f;
    u8 *row = fit_init(&f, w, h, tw, th, out, scratch);
    if ((u32)(row + (u32)w * 4u - scratch) > scratch_len)
      return IMG_ERR_TOO_BIG;
    for (int y = 0; y < h; y++) {
      const u8 *s = rgb_buf + (u32)y * (u32)w * 3u;
      for (int x = 0; x < w; x++) {
        row[x * 4 + 0] = s[x * 3 + 0];
        row[x * 4 + 1] = s[x * 3 + 1];
        row[x * 4 + 2] = s[x * 3 + 2];
        row[x * 4 + 3] = 0xFF;
      }
      fit_row(&f, y, row);
    }
    fit_flush(&f);
  }
  return IMG_OK;
}

/* Area-averages whole source pixels, which at integer ratios avoids the
 * aliasing sampling would give. */
void image_draw_fit(volatile u8 *fb, int bx, int by, int bw, int bh,
                    int screen_height, const Image *img) {
  if (!img || !img->rgb || img->w <= 0 || img->h <= 0)
    return;

  int step = 1;
  while (img->w / step > bw || img->h / step > bh)
    step++;

  int dw = img->w / step, dh = img->h / step;
  if (dw < 1) dw = 1;
  if (dh < 1) dh = 1;
  int ox = bx + (bw - dw) / 2, oy = by + (bh - dh) / 2;
  u32 area = (u32)step * (u32)step;

  for (int dx = 0; dx < dw; dx++) {
    int px = ox + dx;
    if (px < 0)
      continue;
    volatile u8 *p =
        fb + ((u32)px * (u32)screen_height + (u32)(screen_height - 1 - oy)) * 3u;
    for (int dy = 0; dy < dh; dy++) {
      u32 sr = 0, sg = 0, sb = 0;
      for (int j = 0; j < step; j++) {
        const u8 *s = img->rgb +
                      (((u32)(dy * step + j) * (u32)img->w) + (u32)(dx * step)) * 3u;
        for (int i = 0; i < step; i++) {
          sr += s[0];
          sg += s[1];
          sb += s[2];
          s += 3;
        }
      }
      int ty = oy + dy;
      if (ty >= 0 && ty < screen_height) {
        p[0] = (u8)(sb / area);
        p[1] = (u8)(sg / area);
        p[2] = (u8)(sr / area);
      }
      p -= 3;
    }
  }
}
