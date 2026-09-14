/* BMP and PNG decoding (JPEG is in jpeg.c) to RGB888 at IMAGE_RGB_ADDR. Alpha
 * is composited onto the panel colour, since the framebuffer has none. Every
 * length is checked against the buffer caps, so a malformed file fails instead
 * of overrunning. */

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

/* Back-references read from the output buffer itself, so no sliding window is
 * needed: the whole decompressed stream is resident. */
static u32 inflate(const u8 *src, u32 len, u8 *dst, u32 dst_max, int *err) {
  BitIn b;
  Huff lit, dist;
  u32 out = 0;
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
      for (u32 i = 0; i < n; i++)
        dst[out++] = b.src[b.pos++];
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
          dst[out++] = (u8)s;
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
          while (l--)
            dst[out++] = dst[from++];
        }
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
  *err = b.err;
  return out;
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

static ImageResult decode_png(const u8 *d, u32 len, int *ow, int *oh) {
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
    u8 *out = rgb_buf + (u32)y * (u32)w * 3u;
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
    r = decode_png(file_buf, len, &w, &h);
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
