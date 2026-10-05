/* QR code encoder: byte mode, level M, versions 1 to 10. Written from ISO/IEC
 * 18004 and Project Nayuki's description of the QR code steps. */
#include "qr.h"

/* Level M: error correction codewords per block, and blocks, per version. */
static const u8 ec_len[10] = {10, 16, 26, 18, 24, 16, 18, 22, 22, 26};
static const u8 ec_blocks[10] = {1, 1, 1, 2, 2, 4, 4, 4, 5, 5};

static u8 fn[QR_MAX * QR_MAX]; /* finder, timing, alignment, format, version */
static int sz;

static void set(Qr *q, int x, int y, int dark) {
  q->dark[y * sz + x] = (u8)dark;
  fn[y * sz + x] = 1;
}

/* Codewords the data and error correction share in a version. */
static int raw_codewords(int v) {
  int bits = (16 * v + 128) * v + 64;
  if (v >= 2) {
    int n = v / 7 + 2;
    bits -= (25 * n - 10) * n - 55;
    if (v >= 7)
      bits -= 36;
  }
  return bits / 8;
}

static u8 gf_mul(u8 a, u8 b) {
  u32 r = 0;
  for (int i = 7; i >= 0; i--) {
    r = (r << 1) ^ ((r >> 7) * 0x11Du);
    r ^= ((b >> i) & 1u) * a;
  }
  return (u8)r;
}

/* The generator polynomial of degree n, without its leading 1, highest
 * power first. */
static void rs_gen(u8 *g, int n) {
  u8 root = 1;
  for (int i = 0; i < n; i++)
    g[i] = i == n - 1;
  for (int i = 0; i < n; i++) {
    for (int j = 0; j < n; j++) {
      g[j] = gf_mul(g[j], root);
      if (j + 1 < n)
        g[j] ^= g[j + 1];
    }
    root = gf_mul(root, 2);
  }
}

static void rs_ec(const u8 *d, int len, const u8 *g, int n, u8 *out) {
  for (int i = 0; i < n; i++)
    out[i] = 0;
  for (int i = 0; i < len; i++) {
    u8 f = d[i] ^ out[0];
    for (int j = 0; j + 1 < n; j++)
      out[j] = out[j + 1];
    out[n - 1] = 0;
    for (int j = 0; j < n; j++)
      out[j] ^= gf_mul(g[j], f);
  }
}

static void finder(Qr *q, int cx, int cy) {
  for (int dy = -4; dy <= 4; dy++)
    for (int dx = -4; dx <= 4; dx++) {
      int x = cx + dx, y = cy + dy;
      int d = dx < 0 ? -dx : dx, e = dy < 0 ? -dy : dy;
      if (e > d)
        d = e;
      if (x >= 0 && x < sz && y >= 0 && y < sz)
        set(q, x, y, d != 2 && d != 4);
    }
}

static void align(Qr *q, int cx, int cy) {
  for (int dy = -2; dy <= 2; dy++)
    for (int dx = -2; dx <= 2; dx++) {
      int d = dx < 0 ? -dx : dx, e = dy < 0 ? -dy : dy;
      set(q, cx + dx, cy + dy, (e > d ? e : d) != 1);
    }
}

/* Level M is 00 in the format bits. */
static void format_bits(Qr *q, int mask) {
  int data = mask, rem = data;
  for (int i = 0; i < 10; i++)
    rem = (rem << 1) ^ ((rem >> 9) * 0x537);
  int bits = ((data << 10) | rem) ^ 0x5412;
#define BIT(i) ((bits >> (i)) & 1)
  for (int i = 0; i <= 5; i++)
    set(q, 8, i, BIT(i));
  set(q, 8, 7, BIT(6));
  set(q, 8, 8, BIT(7));
  set(q, 7, 8, BIT(8));
  for (int i = 9; i < 15; i++)
    set(q, 14 - i, 8, BIT(i));
  for (int i = 0; i < 8; i++)
    set(q, sz - 1 - i, 8, BIT(i));
  for (int i = 8; i < 15; i++)
    set(q, 8, sz - 15 + i, BIT(i));
#undef BIT
  set(q, 8, sz - 8, 1);
}

static void function_patterns(Qr *q, int v) {
  for (int i = 0; i < sz; i++) {
    set(q, 6, i, i % 2 == 0);
    set(q, i, 6, i % 2 == 0);
  }
  finder(q, 3, 3);
  finder(q, sz - 4, 3);
  finder(q, 3, sz - 4);
  if (v >= 2) {
    int n = v / 7 + 2, step = (v * 4 + n * 2 + 1) / (n * 2 - 2) * 2;
    int pos[7];
    pos[0] = 6;
    for (int i = n - 1, p = sz - 7; i >= 1; i--, p -= step)
      pos[i] = p;
    for (int i = 0; i < n; i++)
      for (int j = 0; j < n; j++)
        if (!((i == 0 && j == 0) || (i == 0 && j == n - 1) ||
              (i == n - 1 && j == 0)))
          align(q, pos[i], pos[j]);
  }
  format_bits(q, 0);
  if (v >= 7) {
    int rem = v;
    for (int i = 0; i < 12; i++)
      rem = (rem << 1) ^ ((rem >> 11) * 0x1F25);
    int bits = (v << 12) | rem;
    for (int i = 0; i < 18; i++) {
      int b = (bits >> i) & 1, a = sz - 11 + i % 3, c = i / 3;
      set(q, a, c, b);
      set(q, c, a, b);
    }
  }
}

static int masked(int m, int x, int y) {
  switch (m) {
    case 0: return (x + y) % 2 == 0;
    case 1: return y % 2 == 0;
    case 2: return x % 3 == 0;
    case 3: return (x + y) % 3 == 0;
    case 4: return (x / 3 + y / 2) % 2 == 0;
    case 5: return x * y % 2 + x * y % 3 == 0;
    case 6: return (x * y % 2 + x * y % 3) % 2 == 0;
    default: return ((x + y) % 2 + x * y % 3) % 2 == 0;
  }
}

static void apply_mask(Qr *q, int m) {
  for (int y = 0; y < sz; y++)
    for (int x = 0; x < sz; x++)
      if (!fn[y * sz + x] && masked(m, x, y))
        q->dark[y * sz + x] ^= 1;
}

/* Module `i` of row or column `a`; outside the symbol is light. */
static int line_at(const Qr *q, int col, int a, int i) {
  if (i < 0 || i >= sz)
    return 0;
  return col ? q->dark[i * sz + a] : q->dark[a * sz + i];
}

/* The four penalty rules of the standard; the mask with the lowest wins. */
static int penalty(const Qr *q) {
  static const u8 core[7] = {1, 0, 1, 1, 1, 0, 1};
  int p = 0, dark = 0;
  for (int col = 0; col < 2; col++)
    for (int a = 0; a < sz; a++) {
      int run = 0, prev = -1;
      for (int i = 0; i < sz; i++) {
        int c = line_at(q, col, a, i);
        if (c == prev) {
          run++;
          p += run == 5 ? 3 : run > 5 ? 1 : 0;
        } else {
          run = 1;
          prev = c;
        }
      }
      for (int s = 0; s + 7 <= sz; s++) {
        int k = 0;
        while (k < 7 && line_at(q, col, a, s + k) == core[k])
          k++;
        if (k < 7)
          continue;
        int before = 1, after = 1;
        for (int j = 1; j <= 4; j++) {
          before &= !line_at(q, col, a, s - j);
          after &= !line_at(q, col, a, s + 6 + j);
        }
        p += 40 * (before + after);
      }
    }
  for (int y = 0; y + 1 < sz; y++)
    for (int x = 0; x + 1 < sz; x++) {
      int c = q->dark[y * sz + x];
      if (c == q->dark[y * sz + x + 1] && c == q->dark[(y + 1) * sz + x] &&
          c == q->dark[(y + 1) * sz + x + 1])
        p += 3;
    }
  for (int i = 0; i < sz * sz; i++)
    dark += q->dark[i];
  int total = sz * sz, d = dark * 20 - total * 10;
  if (d < 0)
    d = -d;
  return p + ((d + total - 1) / total - 1) * 10;
}

int qr_make(Qr *q, const char *text, u32 len) {
  static u8 data[400], all[400], g[32], ec[32];
  int v = 1, cap = 0;
  for (; v <= 10; v++) {
    cap = raw_codewords(v) - ec_len[v - 1] * ec_blocks[v - 1];
    if (4 + (v < 10 ? 8 : 16) + 8 * (int)len <= cap * 8)
      break;
  }
  q->size = 0;
  if (v > 10)
    return 0;
  sz = 17 + 4 * v;

  /* Mode 0100, the count, the bytes, up to four 0 bits, then pad bytes. */
  int bit = 0;
  for (int i = 0; i < cap; i++)
    data[i] = 0;
#define PUT(val, n)                                                   \
  for (int b_ = (n) - 1; b_ >= 0; b_--, bit++)                        \
    if (((val) >> b_) & 1)                                            \
      data[bit >> 3] |= (u8)(0x80u >> (bit & 7));
  PUT(4u, 4);
  PUT(len, v < 10 ? 8 : 16);
  for (u32 i = 0; i < len; i++)
    PUT((u8)text[i], 8);
#undef PUT
  bit += cap * 8 - bit < 4 ? cap * 8 - bit : 4;
  bit = (bit + 7) & ~7;
  for (int i = bit / 8, pad = 0xEC; i < cap; i++, pad ^= 0xEC ^ 0x11)
    data[i] = (u8)pad;

  /* Blocks: the short ones first, the long ones one data codeword longer.
   * Interleaved data codeword by codeword, then error correction. */
  int raw = raw_codewords(v), nb = ec_blocks[v - 1], el = ec_len[v - 1];
  int short_n = nb - raw % nb, short_len = raw / nb - el, n = 0, off = 0;
  rs_gen(g, el);
  for (int i = 0; i <= short_len; i++)
    for (int b = 0, o = 0; b < nb; b++) {
      int bl = short_len + (b >= short_n);
      if (i < bl)
        all[n++] = data[o + i];
      o += bl;
    }
  for (int b = 0; b < nb; b++) {
    int bl = short_len + (b >= short_n);
    rs_ec(data + off, bl, g, el, ec);
    for (int i = 0; i < el; i++)
      all[n + i * nb + b] = ec[i];
    off += bl;
  }

  for (int i = 0; i < sz * sz; i++) {
    q->dark[i] = 0;
    fn[i] = 0;
  }
  function_patterns(q, v);

  int i = 0;
  for (int right = sz - 1; right >= 1; right -= 2) {
    if (right == 6)
      right = 5;
    for (int vert = 0; vert < sz; vert++)
      for (int j = 0; j < 2; j++) {
        int x = right - j, up = ((right + 1) & 2) == 0;
        int y = up ? sz - 1 - vert : vert;
        if (!fn[y * sz + x] && i < raw * 8) {
          q->dark[y * sz + x] = (all[i >> 3] >> (7 - (i & 7))) & 1u;
          i++;
        }
      }
  }

  int best = 0, best_p = 0x7FFFFFFF;
  for (int m = 0; m < 8; m++) {
    apply_mask(q, m);
    format_bits(q, m);
    int p = penalty(q);
    if (p < best_p) {
      best_p = p;
      best = m;
    }
    apply_mask(q, m);
  }
  apply_mask(q, best);
  format_bits(q, best);
  q->size = sz;
  return sz;
}
