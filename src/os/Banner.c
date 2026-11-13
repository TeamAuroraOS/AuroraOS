/* Everything that moves is fixed point (the ARM9 has no FPU): positions and
 * matrices in 16.16, unit vectors and sines in 1/16384, angles as s3d's 65,536
 * per turn. Only the matrices sent to the GPU are floats. */
#include "banner.h"
#include "appicon.h"
#include "container.h"
#include "ff.h"
#include "gpu.h"
#include "p3d.h"
#include "soft3d.h"
#include "ui.h"
#include "model_shbin.h" /* generated from model.v.pica */

extern void os_dcache_flush(void);

#define AR_BASE    BANNER_ARENA_ADDR
#define AR_SCRATCH (BANNER_ARENA_ADDR + 0x00600000u) /* 1 MB: decode rows */
#define AR_FILE    (BANNER_ARENA_ADDR + 0x00700000u) /* 1 MB: the file */
#define AR_END     AR_SCRATCH

#define MAX_TEX   48
#define MAX_MESH  64
#define MAX_NODE  255
#define DYN_VERTS 2400
#define LIST_WORDS 0x8000u
#define WALL_W    512u
#define WALL_H    256u
#define HALO_SIZE 32u

#define F_OFFSET    0x0001u
#define F_ROTATION  0x0002u
#define F_SCALE     0x0004u
#define F_SPIN      0x0008u
#define F_ORBIT     0x0010u
#define F_BOB       0x0020u
#define F_INHERIT   0x0040u
#define F_TINT      0x0080u
#define F_GLOW      0x0100u
#define F_BILLBOARD 0x0200u
#define F_UNLIT     0x0400u
#define F_ADDITIVE  0x0800u
#define F_PULSE     0x1000u

#define ONE   65536
#define Q14   16384
#define NEAR  0.05f
#define FAR   200.0f
#define TEX_WRAP_REPEAT 2u
#define TEX_WRAP_CLAMP  0u

typedef struct {
  float pos[4];
  float uv[2];
  float col[4];
} BnVert;

typedef struct {
  u32 addr;
  u16 w, h;
  u8 levels, translucent, r, g, b;
} BnTex;

typedef struct {
  u32 vbase, vcount, index, icount;
  s16 tex;
  u8 tr, tg, tb, quad;
  s32 radius;
  const s16 *nrm;
  s32 qx[4], qy[4];
  float qu[4], qt[4];
  u16 qi[6];
} BnMesh;

typedef struct {
  s16 mesh, parent;
  u16 flags;
  s32 off[3], rot[3], spin[3], scl[3], orb[6], bob[3], pulse[3];
  u8 tint[3], glow;
  u32 lit;
} BnNode;

typedef struct {
  s32 m[3][4];
} Mx;

typedef struct {
  int ok;
  u32 base, top;
  BnTex tex[MAX_TEX + 2];
  u32 ntex;
  int halo_tex, wall_tex;
  BnMesh mesh[MAX_MESH];
  u32 nmesh;
  BnNode node[MAX_NODE];
  u32 nnode;
  Mx world[MAX_NODE];
  s32 eye[3], right[3], up[3], back[3], light[3];
  P3dMtx viewproj, ortho, view;
  u8 opaque, sky_tex, sky_strength;
  Color sky_top, sky_bot;
  s32 sky_u, sky_v;
  int bloom;
  s32 bloom_int, halo_k;
  int img_tex;
  s16 img_x, img_y;
  u16 img_w, img_h, img_scale;
  u32 pcm, pcm_samples, pcm_rate;
  u32 list, dynv, dyni;
  const char *err;
} Banner;

static Banner bn;
static float lut255[256];
static float lut256[257];
static FATFS bn_fs;

static u16 rd16(const u8 *p) { return (u16)(p[0] | p[1] << 8); }
static u32 rd32(const u8 *p) {
  return p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}
static float rdf(const u8 *p) {
  union { u32 u; float f; } v;
  v.u = rd32(p);
  return v.f;
}

static s32 qmul(s32 a, s32 b) { return (s32)(((s64)a * b) >> 16); }

static u32 isqrt64(u64 v) {
  u64 r = 0, bit = (u64)1 << 62;
  while (bit > v)
    bit >>= 2;
  while (bit) {
    if (v >= r + bit) {
      v -= r + bit;
      r = (r >> 1) + bit;
    } else {
      r >>= 1;
    }
    bit >>= 2;
  }
  return (u32)r;
}

/* An IEEE half float as 16.16, saturated to +-32767. */
static s32 half_q16(u16 h) {
  u32 e = (h >> 10) & 31u, m = h & 1023u;
  s64 v;
  if (e == 0)
    v = (s64)m >> 8;
  else if (e == 31)
    v = 0x7FFFFFFF;
  else if (e >= 9)
    v = (s64)(1024u + m) << (e - 9);
  else
    v = (s64)(1024u + m) >> (9 - e);
  if (v > 0x7FFF0000)
    v = 0x7FFF0000;
  return (h & 0x8000u) ? -(s32)v : (s32)v;
}

static float q16f(s32 v) { return (float)v * (1.0f / 65536.0f); }

/* Degrees in 16.16 to s3d's 65,536 per turn. */
static u32 deg_angle(s64 deg) {
  const s64 full = (s64)360 << 16;
  deg %= full;
  if (deg < 0)
    deg += full;
  return (u32)(deg / 360) & 0xFFFFu;
}

static s64 deg_at(s32 phase, s32 speed, u32 ms) {
  return (s64)phase + (s64)speed * (s64)ms / 1000;
}

static u32 crc_step(u32 crc, const u8 *p, u32 n) {
  crc = ~crc;
  while (n--) {
    crc ^= *p++;
    for (int k = 0; k < 8; k++)
      crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

static void *alloc(u32 size, u32 align) {
  u32 a = (bn.top + align - 1u) & ~(align - 1u);
  if (a + size > AR_END) {
    bn.err = "the banner does not fit in memory";
    return 0;
  }
  bn.top = a + size;
  return (void *)(uintptr_t)a;
}

static void mcopy(void *d, const void *s, u32 n) {
  u8 *dd = (u8 *)d;
  const u8 *ss = (const u8 *)s;
  while (n--)
    *dd++ = *ss++;
}

/* ---- textures ---- */

static void px565(u32 fmt, u16 v, u8 *o) {
  if (fmt == 0) {
    u32 r = (v >> 11) & 31u, g = (v >> 5) & 63u, b = v & 31u;
    o[0] = (u8)(r << 3 | r >> 2);
    o[1] = (u8)(g << 2 | g >> 4);
    o[2] = (u8)(b << 3 | b >> 2);
    o[3] = 255;
  } else if (fmt == 1) {
    o[0] = (u8)(((v >> 12) & 15u) * 17u);
    o[1] = (u8)(((v >> 8) & 15u) * 17u);
    o[2] = (u8)(((v >> 4) & 15u) * 17u);
    o[3] = (u8)((v & 15u) * 17u);
  } else {
    u32 r = (v >> 11) & 31u, g = (v >> 6) & 31u, b = (v >> 1) & 31u;
    o[0] = (u8)(r << 3 | r >> 2);
    o[1] = (u8)(g << 3 | g >> 2);
    o[2] = (u8)(b << 3 | b >> 2);
    o[3] = (v & 1u) ? 255 : 0;
  }
}

static u32 tile_offset(u32 x, u32 y, u32 w) {
  u32 mo = (x & 1u) | ((y & 1u) << 1) | ((x & 2u) << 1) | ((y & 2u) << 2) |
           ((x & 4u) << 2) | ((y & 4u) << 3);
  return (((y >> 3) * (w >> 3) + (x >> 3)) * 64u + mo) * 4u;
}

/* The PICA's texel order: 8x8 tiles in Morton order, each texel A, B, G, R
 * (the same as Glb.c's tile()). */
static void tile(const u8 *src, u32 w, u32 h, u8 *dst) {
  for (u32 y = 0; y < h; y++)
    for (u32 x = 0; x < w; x++) {
      const u8 *s = src + (y * w + x) * 4u;
      u8 *d = dst + tile_offset(x, y, w);
      d[0] = s[3];
      d[1] = s[2];
      d[2] = s[1];
      d[3] = s[0];
    }
}

static void halve(const u8 *s, u32 w, u32 h, u8 *d) {
  u32 w2 = w / 2u, h2 = h / 2u;
  for (u32 y = 0; y < h2; y++)
    for (u32 x = 0; x < w2; x++) {
      const u8 *a = s + ((y * 2u) * w + x * 2u) * 4u, *b = a + w * 4u;
      u8 *o = d + (y * w2 + x) * 4u;
      for (int c = 0; c < 4; c++)
        o[c] = (u8)((a[c] + a[c + 4] + b[c] + b[c + 4] + 2u) >> 2);
    }
}

/* Row-major RGBA8 at AR_SCRATCH, `w` x `h` powers of two, into a mipmapped,
 * tiled texture. */
static int texture_from_rgba(u32 w, u32 h, BnTex *t) {
  u8 *lv = (u8 *)(uintptr_t)AR_SCRATCH;
  u8 *next = lv + w * h * 4u;
  u32 total = 0, levels = 0, w2, h2;
  u64 sr = 0, sg = 0, sb = 0, sw = 0;
  u8 *dst;

  for (w2 = w, h2 = h; w2 >= 8u && h2 >= 8u; w2 >>= 1, h2 >>= 1) {
    total += w2 * h2 * 4u;
    levels++;
  }
  if (!levels || (u32)(next - lv) + w * h * 4u > 0x00100000u) {
    bn.err = "a texture is too large";
    return 0;
  }
  dst = (u8 *)alloc(total, 128);
  if (!dst)
    return 0;
  t->addr = (u32)(uintptr_t)dst;
  t->w = (u16)w;
  t->h = (u16)h;
  t->levels = (u8)(levels - 1u);
  t->translucent = 0;
  for (u32 i = 0; i < w * h; i++) {
    const u8 *p = lv + i * 4u;
    u32 a = p[3] + 1u;
    sr += (u64)p[0] * a;
    sg += (u64)p[1] * a;
    sb += (u64)p[2] * a;
    sw += a;
    if (p[3] < 250u)
      t->translucent = 1;
  }
  t->r = (u8)(sr / sw);
  t->g = (u8)(sg / sw);
  t->b = (u8)(sb / sw);
  for (w2 = w, h2 = h; levels--; w2 >>= 1, h2 >>= 1) {
    tile(lv, w2, h2, dst);
    dst += w2 * h2 * 4u;
    if (levels) {
      u8 *s = lv;
      halve(lv, w2, h2, next);
      lv = next;
      next = s;
    }
  }
  return 1;
}

static u32 pot_up(u32 v) {
  u32 p = 8;
  while (p < v)
    p <<= 1;
  return p;
}

/* A texture blob (u16 w, u16 h, u8 format, palette, pixels); a size that is
 * not a power of two is padded with clear texels (IMAG). */
static int texture_blob(const u8 *d, u32 len, BnTex *t, u32 *iw, u32 *ih) {
  u32 w, h, fmt, pw, ph, n, pos = 5, palfmt = 0, count = 0;
  u8 pal[256][4];
  u8 *rgba = (u8 *)(uintptr_t)AR_SCRATCH;

  if (len < 5)
    return 0;
  w = rd16(d);
  h = rd16(d + 2);
  fmt = d[4];
  if (!w || !h || w > 512u || h > 512u || fmt > 4u)
    return 0;
  pw = pot_up(w);
  ph = pot_up(h);
  n = w * h;
  if (fmt >= 3u) {
    if (pos + 3u > len)
      return 0;
    palfmt = d[pos];
    count = rd16(d + pos + 1);
    pos += 3;
    if (count > 256u || palfmt > 2u || pos + count * 2u > len)
      return 0;
    for (u32 i = 0; i < count; i++)
      px565(palfmt, rd16(d + pos + i * 2u), pal[i]);
    pos += count * 2u;
  }
  if (fmt == 3u ? pos + n > len
                : fmt == 4u ? pos + (n + 1u) / 2u > len : pos + n * 2u > len)
    return 0;
  for (u32 i = 0; i < pw * ph * 4u; i++)
    rgba[i] = 0;
  for (u32 y = 0; y < h; y++)
    for (u32 x = 0; x < w; x++) {
      u32 i = y * w + x;
      u8 *o = rgba + (y * pw + x) * 4u;
      if (fmt == 3u || fmt == 4u) {
        u32 k = fmt == 3u ? d[pos + i] : (d[pos + (i >> 1)] >> (4u * (i & 1u))) & 15u;
        if (k < count) {
          o[0] = pal[k][0];
          o[1] = pal[k][1];
          o[2] = pal[k][2];
          o[3] = pal[k][3];
        }
      } else {
        px565(fmt, rd16(d + pos + i * 2u), o);
      }
    }
  if (iw)
    *iw = w;
  if (ih)
    *ih = h;
  return texture_from_rgba(pw, ph, t);
}

static int halo_texture(BnTex *t) {
  u8 *rgba = (u8 *)(uintptr_t)AR_SCRATCH;
  for (u32 y = 0; y < HALO_SIZE; y++)
    for (u32 x = 0; x < HALO_SIZE; x++) {
      s32 dx = (s32)(2u * x) - (s32)(HALO_SIZE - 1u);
      s32 dy = (s32)(2u * y) - (s32)(HALO_SIZE - 1u);
      u32 r = isqrt64(((u64)(dx * dx + dy * dy)) << 32) / HALO_SIZE;
      s32 v = r >= (u32)ONE ? 0 : ONE - (s32)r;
      u32 g = (u32)(((s64)v * v >> 16) * 255 >> 16);
      u8 *o = rgba + (y * HALO_SIZE + x) * 4u;
      o[0] = o[1] = o[2] = (u8)g;
      o[3] = 255;
    }
  return texture_from_rgba(HALO_SIZE, HALO_SIZE, t);
}

/* The Home Menu's wallpaper (column-major BGR, bottom row first in each
 * column) as a 512x256 texture, the screen at its top left. */
static int wallpaper_texture(BnTex *t) {
  const u8 *bg = ui_wallpaper_top();
  u8 *dst;
  if (!bg)
    return 0;
  dst = (u8 *)alloc(WALL_W * WALL_H * 4u, 128);
  if (!dst)
    return 0;
  for (u32 x = 0; x < TOP_SCREEN_WIDTH; x++) {
    const u8 *col = bg + x * TOP_SCREEN_HEIGHT * 3u;
    for (u32 y = 0; y < TOP_SCREEN_HEIGHT; y++) {
      const u8 *s = col + (TOP_SCREEN_HEIGHT - 1u - y) * 3u;
      u8 *d = dst + tile_offset(x, y, WALL_W);
      d[0] = 255;
      d[1] = s[0];
      d[2] = s[1];
      d[3] = s[2];
    }
  }
  t->addr = (u32)(uintptr_t)dst;
  t->w = WALL_W;
  t->h = WALL_H;
  t->levels = 0;
  t->translucent = 0;
  return 1;
}

/* ---- BBM geometry ---- */

static u32 bits_at(const u8 *d, u32 len, u32 bit, u32 n) {
  u32 v = 0;
  for (u32 i = 0; i < n; i++, bit++) {
    u32 byte = bit >> 3;
    if (byte >= len)
      break;
    v |= (u32)((d[byte] >> (bit & 7u)) & 1u) << i;
  }
  return v;
}

static float half_float(u16 h) { return q16f(half_q16(h)); }

/* The vertex block of a BBM record into `out` positions (x, y, z floats). */
static int bbm_vertices(const u8 *d, u32 len, u32 vc, float *out) {
  if (!len)
    return 0;
  if (d[0] == 6u) {
    u32 bits, lens[3], p = 38;
    float mn[3], mx[3];
    if (len < 38)
      return 0;
    bits = d[1];
    for (int a = 0; a < 3; a++) {
      mn[a] = rdf(d + 2 + a * 8);
      mx[a] = rdf(d + 6 + a * 8);
      lens[a] = rd32(d + 26 + a * 4);
    }
    if (bits < 1u || bits > 24u)
      return 0;
    for (int a = 0; a < 3; a++) {
      float top = (float)((1u << bits) - 1u), step = (mx[a] - mn[a]) / top;
      if (p + lens[a] > len)
        return 0;
      for (u32 i = 0; i < vc; i++)
        out[i * 3u + (u32)a] = mn[a] + (float)bits_at(d + p, lens[a], i * bits, bits) * step;
      p += lens[a];
    }
    return 1;
  }
  if (d[0] == 4u || d[0] == 5u) {
    u32 size = d[0] == 4u ? 2u : 4u, p = 13;
    if (len < 13)
      return 0;
    for (int a = 0; a < 3; a++) {
      u32 ln = rd32(d + 1 + a * 4);
      const u8 *s = d + p;
      if (p + ln > len || ln < vc * size)
        return 0;
      for (u32 i = 0; i < vc; i++) {
        if (size == 2u) {
          out[i * 3u + (u32)a] = half_float((u16)(s[i] | s[vc + i] << 8));
        } else {
          u8 b[4] = {s[i], s[vc + i], s[2u * vc + i], s[3u * vc + i]};
          out[i * 3u + (u32)a] = rdf(b);
        }
      }
      p += ln;
    }
    return 1;
  }
  return 0;
}

static u32 zz(u32 v) { return (v >> 1) ^ (0u - (v & 1u)); }

/* The face block into 16-bit indices; the count of indices, or 0. */
static u32 bbm_faces(const u8 *d, u32 len, u32 vc, u16 *out, u32 cap) {
  u32 n = 0;
  s32 prev = 0;
  if (!len)
    return 0;
  switch (d[0]) {
    case 1:
      n = (len - 1u) / 2u;
      if (n > cap)
        return 0;
      for (u32 i = 0; i < n; i++)
        out[i] = rd16(d + 1 + i * 2u);
      break;
    case 0:
      n = (len - 1u) / 4u;
      if (n > cap)
        return 0;
      for (u32 i = 0; i < n; i++)
        out[i] = (u16)rd32(d + 1 + i * 4u);
      break;
    case 3:
    case 2: {
      u32 sz = d[0] == 3u ? 2u : 4u;
      n = (len - 1u) / sz;
      if (n > cap)
        return 0;
      for (u32 i = 0; i < n; i++) {
        prev += sz == 2u ? (s32)(s16)rd16(d + 1 + i * 2u) : (s32)rd32(d + 1 + i * 4u);
        out[i] = (u16)prev;
      }
      break;
    }
    case 4: {
      u32 bits;
      if (len < 6)
        return 0;
      bits = d[1];
      n = rd32(d + 2);
      if (n > cap || bits < 1u || bits > 32u)
        return 0;
      for (u32 i = 0; i < n; i++)
        out[i] = (u16)bits_at(d + 6, len - 6u, i * bits, bits);
      break;
    }
    case 5: {
      u32 p = 5;
      if (len < 5)
        return 0;
      n = rd32(d + 1);
      if (n > cap)
        return 0;
      for (u32 i = 0; i < n; i++) {
        u32 v = 0, shift = 0;
        for (;;) {
          u8 b;
          if (p >= len)
            return 0;
          b = d[p++];
          v |= (u32)(b & 0x7Fu) << shift;
          shift += 7;
          if (b < 0x80u || shift > 28u)
            break;
        }
        prev += (s32)zz(v);
        out[i] = (u16)prev;
      }
      break;
    }
    default:
      return 0;
  }
  n -= n % 3u;
  for (u32 i = 0; i < n; i++)
    if (out[i] >= vc)
      out[i] = 0;
  return n;
}

static void normals(BnMesh *m, const float *pos, const u16 *idx, u32 ni) {
  s64 *acc = (s64 *)(uintptr_t)AR_SCRATCH;
  s32 *q = (s32 *)(acc + m->vcount * 3u);
  s16 *nrm = (s16 *)alloc(m->vcount * 6u, 4);
  u64 r2max = 0;
  int smooth = m->vcount * 36u <= 0x80000u;
  if (!nrm)
    return;
  m->nrm = nrm;
  m->radius = 0;
  for (u32 i = 0; i < m->vcount; i++) {
    s64 x = (s32)(pos[i * 3] * 4096.0f), y = (s32)(pos[i * 3 + 1] * 4096.0f),
        z = (s32)(pos[i * 3 + 2] * 4096.0f);
    u64 r2 = (u64)(x * x + y * y + z * z);
    if (r2 > r2max)
      r2max = r2;
    nrm[i * 3] = 0;
    nrm[i * 3 + 1] = Q14;
    nrm[i * 3 + 2] = 0;
    if (smooth) {
      q[i * 3] = (s32)x;
      q[i * 3 + 1] = (s32)y;
      q[i * 3 + 2] = (s32)z;
      acc[i * 3] = acc[i * 3 + 1] = acc[i * 3 + 2] = 0;
    }
  }
  m->radius = (s32)(isqrt64(r2max) << 4);
  if (!smooth)
    return;
  for (u32 f = 0; f + 2u < ni; f += 3u) {
    const s32 *a = q + idx[f] * 3u, *b = q + idx[f + 1] * 3u, *c = q + idx[f + 2] * 3u;
    s64 ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2];
    s64 vx = c[0] - a[0], vy = c[1] - a[1], vz = c[2] - a[2];
    s64 nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
    for (int k = 0; k < 3; k++) {
      s64 *t = acc + idx[f + (u32)k] * 3u;
      t[0] += nx >> 6;
      t[1] += ny >> 6;
      t[2] += nz >> 6;
    }
  }
  for (u32 i = 0; i < m->vcount; i++) {
    s64 x = acc[i * 3], y = acc[i * 3 + 1], z = acc[i * 3 + 2];
    u32 l;
    while (x > 0x3FFFFFFF || x < -0x3FFFFFFF || y > 0x3FFFFFFF || y < -0x3FFFFFFF ||
           z > 0x3FFFFFFF || z < -0x3FFFFFFF) {
      x >>= 2;
      y >>= 2;
      z >>= 2;
    }
    l = isqrt64((u64)(x * x + y * y + z * z));
    if (!l)
      continue;
    nrm[i * 3] = (s16)(x * Q14 / (s64)l);
    nrm[i * 3 + 1] = (s16)(y * Q14 / (s64)l);
    nrm[i * 3 + 2] = (s16)(z * Q14 / (s64)l);
  }
}

static int parse_model(const u8 *d, u32 len) {
  BnMesh *m;
  u32 vc, fc, vl, fl, flags;
  const u8 *rec = d + 4;
  float *pos;
  u16 *idx;
  BnVert *vb;
  u32 ni;

  if (bn.nmesh >= MAX_MESH || len < 4u + 48u)
    return 0;
  m = &bn.mesh[bn.nmesh];
  flags = d[3];
  if (rec[0] != 'B' || rec[1] != 'B' || rec[2] != 'M' || rd16(rec + 12))
    return 0;
  vc = rd32(rec + 4);
  fc = rd32(rec + 8);
  vl = rd32(rec + 16);
  fl = rd32(rec + 24);
  if (!vc || vc > 65535u || rd32(rec + 20) || rd32(rec + 28) ||
      52u + vl + fl > len || fc > 65535u)
    return 0;
  pos = (float *)(uintptr_t)(AR_SCRATCH + 0x80000u);
  idx = (u16 *)(pos + vc * 3u);
  if ((u32)((u8 *)(idx + fc * 3u + 3u) - (u8 *)(uintptr_t)AR_SCRATCH) > 0x00100000u)
    return 0;
  if (!bbm_vertices(rec + 48, vl, vc, pos))
    return 0;
  ni = bbm_faces(rec + 48 + vl, fl, vc, idx, fc * 3u + 3u);
  vb = (BnVert *)alloc(vc * sizeof(BnVert), 16);
  if (!vb)
    return 0;
  for (u32 i = 0; i < vc; i++) {
    vb[i].pos[0] = pos[i * 3];
    vb[i].pos[1] = pos[i * 3 + 1];
    vb[i].pos[2] = pos[i * 3 + 2];
    vb[i].pos[3] = 1.0f;
    vb[i].uv[0] = vb[i].uv[1] = 0.0f;
    vb[i].col[0] = vb[i].col[1] = vb[i].col[2] = vb[i].col[3] = 1.0f;
  }
  if (flags & 1u) {
    const u8 *uvb = rec + 48 + vl + fl;
    u32 ub, ulen;
    float u0, u1, v0, v1, top;
    if ((u32)(uvb - d) + 17u > len)
      return 0;
    ub = uvb[0];
    u0 = rdf(uvb + 1);
    u1 = rdf(uvb + 5);
    v0 = rdf(uvb + 9);
    v1 = rdf(uvb + 13);
    ulen = (vc * ub + 7u) / 8u;
    if (ub < 1u || ub > 24u || (u32)(uvb - d) + 17u + 2u * ulen > len)
      return 0;
    top = (float)((1u << ub) - 1u);
    for (u32 i = 0; i < vc; i++) {
      float u = u0 + (float)bits_at(uvb + 17, ulen, i * ub, ub) * ((u1 - u0) / top);
      float v = v0 + (float)bits_at(uvb + 17 + ulen, ulen, i * ub, ub) * ((v1 - v0) / top);
      vb[i].uv[0] = u;
      vb[i].uv[1] = 1.0f - v;
    }
  }
  {
    u16 *ib = (u16 *)alloc(ni * 2u + 2u, 16);
    if (!ib)
      return 0;
    for (u32 i = 0; i < ni; i++)
      ib[i] = idx[i];
    m->index = (u32)(uintptr_t)ib;
  }
  m->vbase = (u32)(uintptr_t)vb;
  m->vcount = vc;
  m->icount = ni;
  m->tex = d[0] == 0xFFu ? -1 : (s16)d[0];
  {
    u16 t = rd16(d + 1);
    u32 r = (t >> 11) & 31u, g = (t >> 5) & 63u, b = t & 31u;
    m->tr = (u8)(r << 3 | r >> 2);
    m->tg = (u8)(g << 2 | g >> 4);
    m->tb = (u8)(b << 3 | b >> 2);
  }
  m->quad = 0;
  if (vc == 4u && ni == 6u && (flags & 1u)) {
    m->quad = 1;
    for (u32 i = 0; i < 4; i++) {
      m->qx[i] = (s32)(vb[i].pos[0] * 65536.0f);
      m->qy[i] = (s32)(vb[i].pos[1] * 65536.0f);
      m->qu[i] = vb[i].uv[0];
      m->qt[i] = vb[i].uv[1];
    }
    for (u32 i = 0; i < 6; i++)
      m->qi[i] = idx[i];
  }
  m->nrm = 0;
  normals(m, pos, idx, ni);
  if (!m->nrm)
    return 0;
  bn.nmesh++;
  return 1;
}

/* ---- nodes ---- */

static const u8 *halves(const u8 *p, const u8 *end, s32 *out, int n) {
  if (p + 2 * n > end)
    return 0;
  for (int i = 0; i < n; i++)
    out[i] = half_q16(rd16(p + 2 * i));
  return p + 2 * n;
}

static int parse_nodes(const u8 *d, u32 len) {
  const u8 *p = d + 1, *end = d + len;
  u32 n;
  if (!len)
    return 0;
  n = d[0];
  for (u32 i = 0; i < n; i++) {
    BnNode *nd = &bn.node[i];
    u16 f;
    if (p + 4 > end)
      return 0;
    nd->mesh = p[0] == 0xFFu ? -1 : p[0];
    nd->parent = p[1] == 0xFFu ? -1 : p[1];
    f = rd16(p + 2);
    nd->flags = f;
    p += 4;
    for (int k = 0; k < 3; k++) {
      nd->off[k] = nd->rot[k] = nd->spin[k] = nd->bob[k] = nd->pulse[k] = 0;
      nd->scl[k] = ONE;
      nd->tint[k] = 255;
    }
    for (int k = 0; k < 6; k++)
      nd->orb[k] = 0;
    nd->glow = 0;
    nd->lit = 0;
    if ((f & F_OFFSET) && !(p = halves(p, end, nd->off, 3)))
      return 0;
    if ((f & F_ROTATION) && !(p = halves(p, end, nd->rot, 3)))
      return 0;
    if ((f & F_SCALE) && !(p = halves(p, end, nd->scl, 3)))
      return 0;
    if ((f & F_SPIN) && !(p = halves(p, end, nd->spin, 3)))
      return 0;
    if ((f & F_ORBIT) && !(p = halves(p, end, nd->orb, 6)))
      return 0;
    if ((f & F_BOB) && !(p = halves(p, end, nd->bob, 3)))
      return 0;
    if (f & F_TINT) {
      u16 t;
      u32 r, g, b;
      if (p + 2 > end)
        return 0;
      t = rd16(p);
      p += 2;
      r = (t >> 11) & 31u;
      g = (t >> 5) & 63u;
      b = t & 31u;
      nd->tint[0] = (u8)(r << 3 | r >> 2);
      nd->tint[1] = (u8)(g << 2 | g >> 4);
      nd->tint[2] = (u8)(b << 3 | b >> 2);
    }
    if (f & F_GLOW) {
      if (p + 1 > end)
        return 0;
      nd->glow = *p++;
    }
    if ((f & F_PULSE) && !(p = halves(p, end, nd->pulse, 3)))
      return 0;
    if (nd->parent >= (s16)i)
      nd->parent = -1;
    if (nd->mesh >= 0 && (u32)nd->mesh >= bn.nmesh)
      nd->mesh = -1;
  }
  bn.nnode = n;
  return 1;
}

static void rot3(u32 ax, u32 ay, u32 az, s32 r[3][3]) {
  s32 cx = s3d_cos(ax), sx = s3d_sin(ax), cy = s3d_cos(ay), sy = s3d_sin(ay),
      cz = s3d_cos(az), sz = s3d_sin(az);
  const s32 X[3][3] = {{Q14, 0, 0}, {0, cx, -sx}, {0, sx, cx}};
  const s32 Y[3][3] = {{cy, 0, sy}, {0, Q14, 0}, {-sy, 0, cy}};
  const s32 Z[3][3] = {{cz, -sz, 0}, {sz, cz, 0}, {0, 0, Q14}};
  s32 t[3][3];
  for (int i = 0; i < 3; i++)
    for (int j = 0; j < 3; j++)
      t[i][j] = (Y[i][0] * X[0][j] + Y[i][1] * X[1][j] + Y[i][2] * X[2][j]) >> 14;
  for (int i = 0; i < 3; i++)
    for (int j = 0; j < 3; j++)
      r[i][j] = (Z[i][0] * t[0][j] + Z[i][1] * t[1][j] + Z[i][2] * t[2][j]) >> 14;
}

static void mx_mul(const Mx *a, const Mx *b, Mx *o) {
  Mx t;
  for (int i = 0; i < 3; i++) {
    for (int j = 0; j < 4; j++) {
      s64 s = (s64)a->m[i][0] * b->m[0][j] + (s64)a->m[i][1] * b->m[1][j] +
              (s64)a->m[i][2] * b->m[2][j];
      t.m[i][j] = (s32)(s >> 16) + (j == 3 ? a->m[i][3] : 0);
    }
  }
  *o = t;
}

static u32 col_len(const Mx *m, int j) {
  return isqrt64((u64)((s64)m->m[0][j] * m->m[0][j] + (s64)m->m[1][j] * m->m[1][j] +
                       (s64)m->m[2][j] * m->m[2][j]));
}

static s32 brightness(const BnNode *n, u32 ms) {
  s32 d = n->pulse[1];
  if (!(n->flags & F_PULSE) || !n->pulse[0] || !d)
    return ONE;
  s64 deg = (s64)n->pulse[0] * 360 * (s64)ms / 1000 + n->pulse[2];
  s32 half = 32768 + s3d_sin(deg_angle(deg)) * 2;
  return ONE - d + qmul(d, half);
}

static void node_local(const BnNode *n, u32 ms, Mx *out) {
  s32 x = n->off[0], y = n->off[1], z = n->off[2];
  s32 r[3][3];
  if (n->orb[0] || n->orb[1]) {
    u32 th = deg_angle(deg_at(n->orb[3], n->orb[2], ms));
    s32 c = s3d_cos(th), s = s3d_sin(th);
    s32 ox = (s32)(((s64)n->orb[0] * c) >> 14), oy = 0,
        oz = -(s32)(((s64)n->orb[1] * s) >> 14);
    u32 tx = deg_angle(n->orb[4]), tz = deg_angle(n->orb[5]);
    s32 cxx = s3d_cos(tx), sxx = s3d_sin(tx), czz = s3d_cos(tz), szz = s3d_sin(tz);
    s32 y1 = (s32)(((s64)oy * cxx - (s64)oz * sxx) >> 14);
    s32 z1 = (s32)(((s64)oy * sxx + (s64)oz * cxx) >> 14);
    s32 x2 = (s32)(((s64)ox * czz - (s64)y1 * szz) >> 14);
    s32 y2 = (s32)(((s64)ox * szz + (s64)y1 * czz) >> 14);
    x += x2;
    y += y2;
    z += z1;
  }
  if (n->bob[0]) {
    s64 deg = (s64)n->bob[1] * 360 * (s64)ms / 1000 + n->bob[2];
    y += (s32)(((s64)n->bob[0] * s3d_sin(deg_angle(deg))) >> 14);
  }
  rot3(deg_angle(deg_at(n->rot[0], n->spin[0], ms)),
       deg_angle(deg_at(n->rot[1], n->spin[1], ms)),
       deg_angle(deg_at(n->rot[2], n->spin[2], ms)), r);
  for (int i = 0; i < 3; i++)
    for (int j = 0; j < 3; j++)
      out->m[i][j] = (s32)(((s64)r[i][j] * n->scl[j]) >> 14);
  out->m[0][3] = x;
  out->m[1][3] = y;
  out->m[2][3] = z;
}

static void eval_nodes(u32 ms) {
  for (u32 i = 0; i < bn.nnode; i++) {
    const BnNode *n = &bn.node[i];
    Mx m;
    node_local(n, ms, &m);
    if (n->parent >= 0) {
      const Mx *p = &bn.world[n->parent];
      if (n->flags & F_INHERIT) {
        mx_mul(p, &m, &m);
      } else {
        m.m[0][3] += p->m[0][3];
        m.m[1][3] += p->m[1][3];
        m.m[2][3] += p->m[2][3];
      }
    }
    if (n->flags & F_BILLBOARD) {
      u32 roll = deg_angle(deg_at(n->rot[2], n->spin[2], ms));
      s32 c = s3d_cos(roll), s = s3d_sin(roll);
      s32 l0 = (s32)col_len(&m, 0), l1 = (s32)col_len(&m, 1), l2 = (s32)col_len(&m, 2);
      for (int k = 0; k < 3; k++) {
        s32 rx = (c * bn.right[k] + s * bn.up[k]) >> 14;
        s32 uy = (-s * bn.right[k] + c * bn.up[k]) >> 14;
        m.m[k][0] = (s32)(((s64)rx * l0) >> 14);
        m.m[k][1] = (s32)(((s64)uy * l1) >> 14);
        m.m[k][2] = (s32)(((s64)bn.back[k] * l2) >> 14);
      }
    }
    bn.world[i] = m;
  }
}

/* ---- camera ---- */

static void normalise14(const s64 v[3], s32 out[3]) {
  u32 l = isqrt64((u64)(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]));
  for (int k = 0; k < 3; k++)
    out[k] = l ? (s32)(v[k] * Q14 / (s64)l) : (k == 1 ? Q14 : 0);
}

static int parse_view(const u8 *d, u32 len) {
  s32 eye[3], tgt[3], fov[1];
  s64 f[3], s[3], u[3];
  s32 fq[3], sq[3], uq[3];
  P3dMtx proj;
  u32 a;
  float tan_half;
  if (len < 18)
    return 0;
  halves(d, d + len, eye, 3);
  halves(d + 6, d + len, tgt, 3);
  halves(d + 12, d + len, fov, 1);
  {
    u16 t = rd16(d + 14), b = rd16(d + 16);
    u32 r = (t >> 11) & 31u, g = (t >> 5) & 63u, bb = t & 31u;
    bn.sky_top = (Color){(u8)(r << 3 | r >> 2), (u8)(g << 2 | g >> 4), (u8)(bb << 3 | bb >> 2)};
    r = (b >> 11) & 31u;
    g = (b >> 5) & 63u;
    bb = b & 31u;
    bn.sky_bot = (Color){(u8)(r << 3 | r >> 2), (u8)(g << 2 | g >> 4), (u8)(bb << 3 | bb >> 2)};
  }
  bn.opaque = len > 18 ? (d[18] & 1u) : 1;
  for (int k = 0; k < 3; k++) {
    bn.eye[k] = eye[k];
    f[k] = (s64)tgt[k] - eye[k];
  }
  normalise14(f, fq);
  {
    s64 upv[3] = {0, Q14, 0};
    if (fq[1] > 16367 || fq[1] < -16367) {
      upv[1] = 0;
      upv[2] = -Q14;
    }
    s[0] = (s64)fq[1] * upv[2] - (s64)fq[2] * upv[1];
    s[1] = (s64)fq[2] * upv[0] - (s64)fq[0] * upv[2];
    s[2] = (s64)fq[0] * upv[1] - (s64)fq[1] * upv[0];
  }
  normalise14(s, sq);
  u[0] = ((s64)sq[1] * fq[2] - (s64)sq[2] * fq[1]) >> 14;
  u[1] = ((s64)sq[2] * fq[0] - (s64)sq[0] * fq[2]) >> 14;
  u[2] = ((s64)sq[0] * fq[1] - (s64)sq[1] * fq[0]) >> 14;
  normalise14(u, uq);
  for (int k = 0; k < 3; k++) {
    bn.right[k] = sq[k];
    bn.up[k] = uq[k];
    bn.back[k] = -fq[k];
  }
  {
    static const s32 lv[3] = {-5610, 9616, 12020};
    for (int k = 0; k < 3; k++)
      bn.light[k] = (bn.right[k] * lv[0] + bn.up[k] * lv[1] + bn.back[k] * lv[2]) >> 14;
  }
  for (int c = 0; c < 3; c++) {
    bn.view.m[0][c] = (float)bn.right[c] * (1.0f / Q14);
    bn.view.m[1][c] = (float)bn.up[c] * (1.0f / Q14);
    bn.view.m[2][c] = (float)bn.back[c] * (1.0f / Q14);
  }
  for (int r = 0; r < 3; r++) {
    const s32 *ax = r == 0 ? bn.right : r == 1 ? bn.up : bn.back;
    s64 t = ((s64)ax[0] * eye[0] + (s64)ax[1] * eye[1] + (s64)ax[2] * eye[2]) >> 14;
    bn.view.m[r][3] = -q16f((s32)t);
  }
  bn.view.m[3][0] = bn.view.m[3][1] = bn.view.m[3][2] = 0.0f;
  bn.view.m[3][3] = 1.0f;
  if (fov[0] < 5 * ONE)
    fov[0] = 5 * ONE;
  if (fov[0] > 150 * ONE)
    fov[0] = 150 * ONE;
  a = deg_angle((s64)fov[0] / 2);
  tan_half = (float)s3d_sin(a) / (float)s3d_cos(a);
  p3d_persp_stereo_tilt(&proj, tan_half, (float)TOP_SCREEN_WIDTH / TOP_SCREEN_HEIGHT, NEAR, FAR, 0.0f, 1.0f);
  p3d_mtx_mul(&bn.viewproj, &proj, &bn.view);
  p3d_ortho_tilt(&bn.ortho, 0.0f, TOP_SCREEN_WIDTH, TOP_SCREEN_HEIGHT, 0.0f, 1.0f, -1.0f);
  return 1;
}

/* ---- the jingle ---- */

static s32 clamp16(s32 v) { return v < -32768 ? -32768 : v > 32767 ? 32767 : v; }

static const u16 ima_steps[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
    253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
    3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
    11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767};
static const s8 ima_index[8] = {-1, -1, -1, -1, 2, 4, 6, 8};

static void put_sample(s16 *out, u32 i, s32 v, int mix) {
  out[i] = (s16)(mix ? (out[i] + v) >> 1 : v);
}

static void dsp_decode(const u8 *data, const s16 *coef, u32 count, s16 *out, int mix) {
  s32 h1 = 0, h2 = 0;
  u32 n = 0;
  for (u32 f = 0; n < count; f += 8u) {
    u32 ps = data[f], sc = 1u << (ps & 15u), ci = (ps >> 4) & 7u;
    s32 c1 = coef[ci * 2u], c2 = coef[ci * 2u + 1u];
    for (u32 i = 0; i < 14u && n < count; i++) {
      u8 b = data[f + 1u + (i >> 1)];
      s32 q = (i & 1u) ? (b & 15) : (b >> 4);
      s32 v;
      if (q >= 8)
        q -= 16;
      v = clamp16(q * (s32)sc + ((c1 * h1 + c2 * h2 + 1024) >> 11));
      put_sample(out, n++, v, mix);
      h2 = h1;
      h1 = v;
    }
  }
}

static void ima_decode(const u8 *data, s32 pred, u32 idx, u32 count, s16 *out, int mix) {
  u32 n = 0;
  for (u32 i = 0; n < count; i++) {
    for (u32 half = 0; half < 2u && n < count; half++) {
      u32 code = half ? data[i] >> 4 : data[i] & 15u;
      u32 step = ima_steps[idx];
      s32 diff = (s32)(step >> 3);
      if (code & 4u)
        diff += (s32)step;
      if (code & 2u)
        diff += (s32)(step >> 1);
      if (code & 1u)
        diff += (s32)(step >> 2);
      pred = clamp16((code & 8u) ? pred - diff : pred + diff);
      {
        s32 ni = (s32)idx + ima_index[code & 7u];
        idx = (u32)(ni < 0 ? 0 : ni > 88 ? 88 : ni);
      }
      put_sample(out, n++, pred, mix);
    }
  }
}

static int parse_jingle(const u8 *d, u32 len) {
  u32 codec, nch, rate, count, hsz, dsz, pos;
  s16 *out;
  if (len < 12)
    return 0;
  codec = d[0];
  nch = d[1];
  rate = rd16(d + 2);
  count = rd32(d + 4);
  if ((codec != 1u && codec != 2u) || nch < 1u || nch > 2u || !rate || !count ||
      count > 30u * 48000u)
    return 0;
  hsz = codec == 1u ? 37u : 6u;
  dsz = codec == 1u ? ((count + 13u) / 14u) * 8u : (count + 1u) / 2u;
  if (12u + nch * (hsz + dsz) > len)
    return 0;
  out = (s16 *)alloc(count * 2u + 64u, 32);
  if (!out)
    return 0;
  pos = 12u + nch * hsz;
  for (u32 c = 0; c < nch; c++) {
    const u8 *h = d + 12u + c * hsz;
    if (codec == 1u) {
      s16 coef[16];
      for (int k = 0; k < 16; k++)
        coef[k] = (s16)rd16(h + 2 * k);
      dsp_decode(d + pos + c * dsz, coef, count, out, c == 1u);
    } else {
      ima_decode(d + pos + c * dsz, (s16)rd16(h), h[2] > 88u ? 88u : h[2], count, out, c == 1u);
    }
  }
  bn.pcm = (u32)(uintptr_t)out;
  bn.pcm_samples = count;
  bn.pcm_rate = rate;
  return 1;
}

/* ---- the file ---- */

static int verify(const u8 *d, u32 size, u32 *count, u32 *first) {
  static const u8 credit[12] = {'C', 'r', 'a', 'c', 'k', 'o', '2', '9', '8', 0, 0x02, 0x98};
  static const u8 zero[4] = {0, 0, 0, 0};
  u32 crc;
  if (size < 32 || d[0] != 'B' || d[1] != 'N' || d[2] != 'R') {
    bn.err = "not a banner";
    return 0;
  }
  if (d[3] != 3) {
    bn.err = "banner version is not 3: rebuild it";
    return 0;
  }
  for (int i = 0; i < 12; i++)
    if (d[4 + i] != credit[i]) {
      bn.err = "the Cracko298 seal is missing";
      return 0;
    }
  if (rd32(d + 16) != size) {
    bn.err = "the banner is cut short";
    return 0;
  }
  crc = crc_step(0, (const u8 *)"Cracko298", 9);
  crc = crc_step(crc, d, 28);
  crc = crc_step(crc, zero, 4);
  crc = crc_step(crc, d + 32, size - 32u);
  if (crc != rd32(d + 28)) {
    bn.err = "the banner's checksum is wrong";
    return 0;
  }
  *count = rd16(d + 20);
  *first = rd32(d + 24);
  return 1;
}

static void luts(void) {
  for (int i = 0; i < 256; i++)
    lut255[i] = (float)i * (1.0f / 255.0f);
  for (int i = 0; i <= 256; i++)
    lut256[i] = (float)i * (1.0f / 256.0f);
}

int banner_parse(const u8 *data, u32 size) {
  u32 count, pos;
  int have_nodes = 0, have_view = 0;
  bn.ok = 0;
  bn.err = 0;
  bn.base = bn.top = AR_BASE;
  bn.ntex = bn.nmesh = bn.nnode = 0;
  bn.halo_tex = bn.wall_tex = bn.img_tex = -1;
  bn.sky_tex = 0xFF;
  bn.bloom = 0;
  bn.pcm = bn.pcm_samples = 0;
  luts();
  if (!verify(data, size, &count, &pos))
    return 0;
  for (u32 c = 0; c < count; c++) {
    const u8 *tag, *d;
    u32 len;
    if (pos + 8u > size)
      break;
    tag = data + pos;
    len = rd32(data + pos + 4);
    d = data + pos + 8;
    if (pos + 8u + len > size)
      break;
    pos += 8u + len;
    if (tag[0] == 'V' && tag[1] == 'I' && tag[2] == 'E' && tag[3] == 'W') {
      have_view = parse_view(d, len);
    } else if (tag[0] == 'T' && tag[1] == 'E' && tag[2] == 'X' && tag[3] == 'R') {
      if (bn.ntex >= MAX_TEX) {
        bn.err = "too many textures";
        return 0;
      }
      if (!texture_blob(d, len, &bn.tex[bn.ntex], 0, 0)) {
        if (bn.err)
          return 0;
        bn.tex[bn.ntex].addr = 0;
      }
      bn.ntex++;
    } else if (tag[0] == 'M' && tag[1] == 'O' && tag[2] == 'D' && tag[3] == 'L') {
      if (!parse_model(d, len)) {
        if (!bn.err)
          bn.err = "a model could not be read";
        return 0;
      }
    } else if (tag[0] == 'N' && tag[1] == 'O' && tag[2] == 'D' && tag[3] == 'E') {
      have_nodes = parse_nodes(d, len);
    } else if (tag[0] == 'S' && tag[1] == 'K' && tag[2] == 'Y' && tag[3] == ' ' && len >= 6) {
      bn.sky_tex = d[0];
      bn.sky_u = half_q16(rd16(d + 1));
      bn.sky_v = half_q16(rd16(d + 3));
      bn.sky_strength = d[5];
    } else if (tag[0] == 'B' && tag[1] == 'L' && tag[2] == 'O' && tag[3] == 'M' && len >= 4) {
      bn.bloom = 1;
      bn.bloom_int = half_q16(rd16(d));
      bn.halo_k = ONE + half_q16(rd16(d + 2)) / 8;
    } else if (tag[0] == 'I' && tag[1] == 'M' && tag[2] == 'A' && tag[3] == 'G' && len > 7) {
      u32 w, h;
      if (d[0] == 0 && bn.ntex < MAX_TEX + 2 &&
          texture_blob(d + 7, len - 7u, &bn.tex[bn.ntex], &w, &h)) {
        bn.img_tex = (int)bn.ntex++;
        bn.img_x = (s16)rd16(d + 1);
        bn.img_y = (s16)rd16(d + 3);
        bn.img_scale = rd16(d + 5);
        bn.img_w = (u16)w;
        bn.img_h = (u16)h;
      }
    } else if (tag[0] == 'J' && tag[1] == 'N' && tag[2] == 'G' && tag[3] == 'L') {
      if (!parse_jingle(d, len))
        bn.pcm = 0;
    }
  }
  if (!have_view || !have_nodes) {
    bn.err = "the banner has no camera or no scene";
    return 0;
  }
  if (bn.ntex < MAX_TEX + 2 && halo_texture(&bn.tex[bn.ntex]))
    bn.halo_tex = (int)bn.ntex++;
  if (!bn.opaque && bn.ntex < MAX_TEX + 2 && wallpaper_texture(&bn.tex[bn.ntex]))
    bn.wall_tex = (int)bn.ntex++;
  for (u32 i = 0; i < bn.nnode; i++) {
    BnNode *n = &bn.node[i];
    const BnMesh *m;
    if (n->mesh < 0)
      continue;
    m = &bn.mesh[n->mesh];
    if (!(n->flags & (F_UNLIT | F_ADDITIVE)) && !m->quad) {
      BnVert *copy = (BnVert *)alloc(m->vcount * sizeof(BnVert), 16);
      if (!copy)
        return 0;
      mcopy(copy, (const void *)(uintptr_t)m->vbase, m->vcount * sizeof(BnVert));
      n->lit = (u32)(uintptr_t)copy;
    }
  }
  bn.dynv = (u32)(uintptr_t)alloc(DYN_VERTS * sizeof(BnVert), 16);
  bn.dyni = (u32)(uintptr_t)alloc(DYN_VERTS / 4u * 6u * 2u, 16);
  bn.list = (u32)(uintptr_t)alloc(LIST_WORDS * 4u, 16);
  if (!bn.dynv || !bn.dyni || !bn.list)
    return 0;
  os_dcache_flush();
  bn.ok = 1;
  return 1;
}

static int path_beside(const char *app, char *out, unsigned max) {
  unsigned n = 0, dot = 0;
  while (app[n]) {
    if (n + 5u >= max)
      return 0;
    out[n] = app[n];
    if (app[n] == '.')
      dot = n;
    if (app[n] == '/')
      dot = 0;
    n++;
  }
  if (!dot)
    dot = n;
  out[dot] = '.';
  out[dot + 1] = 'b';
  out[dot + 2] = 'n';
  out[dot + 3] = 'r';
  out[dot + 4] = 0;
  return 1;
}

static int path_data(const char *app, char *out, unsigned max) {
  static const char tail[] = "/banner.bnr";
  unsigned n;
  if (!aurora_c_data_dir(app, out, max - sizeof(tail)))
    return 0;
  for (n = 0; out[n]; n++)
    ;
  for (unsigned i = 0; i < sizeof(tail); i++)
    out[n + i] = tail[i];
  return 1;
}

static int open_banner(FIL *f, const char *app, u32 where, u32 *base) {
  char path[300];
  *base = 0;
  if (where & BANNER_EMBEDDED) {
    *base = where & ~BANNER_EMBEDDED;
    return f_open(f, app, FA_READ) == FR_OK && f_lseek(f, *base) == FR_OK;
  }
  if (where == BANNER_BESIDE && path_beside(app, path, sizeof path))
    return f_open(f, path, FA_READ) == FR_OK;
  if (where == BANNER_DATA && path_data(app, path, sizeof path))
    return f_open(f, path, FA_READ) == FR_OK;
  return 0;
}

static int read_at(FIL *f, u32 off, void *buf, u32 n) {
  UINT got;
  return f_lseek(f, off) == FR_OK && f_read(f, buf, n, &got) == FR_OK && got == n;
}

/* The ICON chunk of the banner `f` holds at `base`, if it has an Aurora one. */
static void find_icon(FIL *f, u32 base, u16 *icon, u8 *anim, int *has_icon) {
  u8 h[32], ch[9];
  u32 count, pos, size;
  if (!read_at(f, base, h, 32) || h[0] != 'B' || h[1] != 'N' || h[2] != 'R' || h[3] != 3)
    return;
  size = rd32(h + 16);
  count = rd16(h + 20);
  pos = rd32(h + 24);
  for (u32 c = 0; c < count && pos + 8u <= size; c++) {
    u32 len;
    if (!read_at(f, base + pos, ch, 9))
      return;
    len = rd32(ch + 4);
    if (ch[0] == 'I' && ch[1] == 'C' && ch[2] == 'O' && ch[3] == 'N' && ch[8] == 2 &&
        len >= 1u + APPICON_BYTES) {
      u8 hdr[8];
      UINT got;
      if (f_read(f, hdr, 8, &got) != FR_OK || got != 8 || !appicon_header(hdr))
        return;
      if (f_read(f, icon, 2048, &got) == FR_OK && got == 2048) {
        *anim = hdr[4];
        *has_icon = 1;
      }
      return;
    }
    pos += 8u + len;
  }
}

u32 banner_find(const char *app_path, u16 *icon, u8 *anim, int *has_icon) {
  static const u32 order[2] = {BANNER_BESIDE, BANNER_DATA};
  FIL f;
  u8 h[36], m[4];
  u32 off;
  *has_icon = 0;
  if (f_open(&f, app_path, FA_READ) == FR_OK) {
    UINT got;
    int ok = f_read(&f, h, sizeof h, &got) == FR_OK && got == sizeof h &&
             appicon_offset(h, f_size(&f), &off) &&
             read_at(&f, off + APPICON_BYTES, m, 4) && m[0] == 'B' && m[1] == 'N' && m[2] == 'R';
    if (ok) {
      find_icon(&f, off + APPICON_BYTES, icon, anim, has_icon);
      f_close(&f);
      return BANNER_EMBEDDED | (off + APPICON_BYTES);
    }
    f_close(&f);
  }
  for (int i = 0; i < 2; i++) {
    u32 base;
    if (open_banner(&f, app_path, order[i], &base)) {
      find_icon(&f, base, icon, anim, has_icon);
      f_close(&f);
      return order[i];
    }
  }
  return BANNER_NONE;
}

int banner_load(const char *app_path, u32 where) {
  FIL f;
  u32 base, size = 0;
  int ok = 0;
  u8 *buf = (u8 *)(uintptr_t)AR_FILE;
  bn.ok = 0;
  bn.err = "the banner could not be read";
  if (f_mount(&bn_fs, "", 1) == FR_OK) {
    if (open_banner(&f, app_path, where, &base)) {
      u8 h[20];
      if (read_at(&f, base, h, 20)) {
        size = rd32(h + 16);
        ok = size >= 32u && size <= BANNER_FILE_MAX && read_at(&f, base, buf, size);
      }
      f_close(&f);
    }
  }
  f_mount(NULL, "", 0);
  if (!ok)
    return 0;
  return banner_parse(buf, size);
}

void banner_unload(void) { bn.ok = 0; }
int banner_ready(void) { return bn.ok; }
const char *banner_error(void) { return bn.err ? bn.err : ""; }
u32 banner_node_count(void) { return bn.ok ? bn.nnode : 0; }

int banner_jingle(u32 *addr, u32 *samples, u32 *rate) {
  if (!bn.ok || !bn.pcm)
    return 0;
  *addr = bn.pcm;
  *samples = bn.pcm_samples;
  *rate = bn.pcm_rate;
  return 1;
}

int banner_node_matrix(int i, u32 ms, s32 out[12]) {
  if (!bn.ok || i < 0 || (u32)i >= bn.nnode)
    return 0;
  eval_nodes(ms);
  for (int r = 0; r < 3; r++)
    for (int c = 0; c < 4; c++)
      out[r * 4 + c] = bn.world[i].m[r][c];
  return 1;
}

/* ---- drawing ---- */

typedef struct {
  P3dList l;
  BnVert *v;
  u16 *ix;
  u32 nv, ni;
  int tex;   /* bound texture, -2 none yet */
  int alpha; /* bound stage-0 alpha source */
  u32 fade;
} Draw;

static void set_vert(BnVert *o, float x, float y, float z, float u, float t, const float c[4]) {
  o->pos[0] = x;
  o->pos[1] = y;
  o->pos[2] = z;
  o->pos[3] = 1.0f;
  o->uv[0] = u;
  o->uv[1] = t;
  o->col[0] = c[0];
  o->col[1] = c[1];
  o->col[2] = c[2];
  o->col[3] = c[3];
}

static void bind(Draw *dr, int tex, int alpha, u32 wrap) {
  if (tex == dr->tex && alpha == dr->alpha)
    return;
  if (tex >= 0 && bn.tex[tex].addr) {
    const BnTex *t = &bn.tex[tex];
    p3d_texture(&dr->l, t->addr, t->w, t->h, t->levels,
                P3D_TEX_MAG_LINEAR | P3D_TEX_MIN_LINEAR | P3D_TEX_MIP_LINEAR |
                    P3D_TEX_WRAP_S(wrap) | P3D_TEX_WRAP_T(wrap));
    p3d_stage0(&dr->l, 1, alpha);
  } else {
    p3d_texture_off(&dr->l);
    p3d_stage0(&dr->l, 0, alpha == P3D_ALPHA_TEXTURE ? P3D_ALPHA_ONE : alpha);
  }
  dr->tex = tex;
  dr->alpha = alpha;
}

static u32 rgba(u32 r, u32 g, u32 b, u32 a) {
  return (r > 255u ? 255u : r) | (g > 255u ? 255u : g) << 8 | (b > 255u ? 255u : b) << 16 |
         (a > 255u ? 255u : a) << 24;
}

/* A quad from the dynamic buffer: corners 0 top-left, 1 top-right, 2 bottom-
 * left, 3 bottom-right. */
static int quad_start(Draw *dr, u32 *first) {
  if (dr->nv + 4u > DYN_VERTS)
    return 0;
  *first = dr->nv;
  return 1;
}

static void draw_dyn(Draw *dr, u32 first_vert, u32 first_index, u32 nidx) {
  if (!nidx)
    return;
  p3d_buffer0(&dr->l, bn.dynv + first_vert * sizeof(BnVert) - bn.base, sizeof(BnVert), 3);
  p3d_draw_elements(&dr->l, bn.dyni + first_index * 2u - bn.base, nidx);
}

static void screen_quad(Draw *dr, float x0, float y0, float x1, float y1, float u0, float t0,
                        float u1, float t1, const float ct[4], const float cb[4]) {
  u32 f;
  static const u16 pat[6] = {0, 2, 1, 1, 2, 3};
  if (!quad_start(dr, &f))
    return;
  set_vert(&dr->v[f], x0, y0, 0.0f, u0, t0, ct);
  set_vert(&dr->v[f + 1], x1, y0, 0.0f, u1, t0, ct);
  set_vert(&dr->v[f + 2], x0, y1, 0.0f, u0, t1, cb);
  set_vert(&dr->v[f + 3], x1, y1, 0.0f, u1, t1, cb);
  for (int i = 0; i < 6; i++)
    dr->ix[dr->ni + (u32)i] = pat[i];
  draw_dyn(dr, f, dr->ni, 6);
  dr->nv += 4;
  dr->ni += 6;
}

static void colour_of(Color c, float out[4]) {
  out[0] = lut255[c.r];
  out[1] = lut255[c.g];
  out[2] = lut255[c.b];
  out[3] = 1.0f;
}

static void background(Draw *dr, u32 ms) {
  float white[4] = {1.0f, 1.0f, 1.0f, 1.0f}, top[4], bot[4];
  p3d_uniform(&dr->l, MODEL_SHADER_UNIF_MVP, &bn.ortho);
  p3d_fragment(&dr->l, 0, 0, -1);
  p3d_stage1_tint(&dr->l, rgba(255, 255, 255, 255));
  if (!bn.opaque && bn.wall_tex >= 0) {
    bind(dr, bn.wall_tex, P3D_ALPHA_ONE, TEX_WRAP_CLAMP);
    screen_quad(dr, 0, 0, TOP_SCREEN_WIDTH, TOP_SCREEN_HEIGHT, 0.0f, 1.0f,
                (float)TOP_SCREEN_WIDTH / WALL_W, 1.0f - (float)TOP_SCREEN_HEIGHT / WALL_H,
                white, white);
  } else {
    Color t = bn.opaque ? bn.sky_top : (Color){0x2C, 0x2F, 0x3A};
    Color b = bn.opaque ? bn.sky_bot : (Color){0x0C, 0x0D, 0x12};
    colour_of(t, top);
    colour_of(b, bot);
    bind(dr, -1, P3D_ALPHA_ONE, 0);
    screen_quad(dr, 0, 0, TOP_SCREEN_WIDTH, TOP_SCREEN_HEIGHT, 0, 0, 0, 0, top, bot);
  }
  if (bn.sky_tex < bn.ntex && bn.tex[bn.sky_tex].addr) {
    float u = q16f((s32)(((s64)bn.sky_u * ms / 1000) % ONE));
    float v = q16f((s32)(((s64)bn.sky_v * ms / 1000) % ONE));
    float c = lut255[bn.sky_strength], col[4] = {c, c, c, 1.0f};
    p3d_blend(&dr->l, 1);
    bind(dr, bn.sky_tex, P3D_ALPHA_ONE, TEX_WRAP_REPEAT);
    screen_quad(dr, 0, 0, TOP_SCREEN_WIDTH, TOP_SCREEN_HEIGHT, -u, 1.0f + v, 1.0f - u, v, col, col);
    p3d_blend(&dr->l, 0);
  }
}

static void node_mvp(int i, P3dMtx *out) {
  const Mx *w = &bn.world[i];
  P3dMtx m;
  for (int r = 0; r < 3; r++)
    for (int c = 0; c < 4; c++)
      m.m[r][c] = q16f(w->m[r][c]);
  m.m[3][0] = m.m[3][1] = m.m[3][2] = 0.0f;
  m.m[3][3] = 1.0f;
  p3d_mtx_mul(out, &bn.viewproj, &m);
}

static u32 node_colour(const BnNode *n, const BnMesh *m, u32 ms, u32 fade) {
  s32 br = brightness(n, ms);
  u32 r = (u32)(((s64)m->tr * n->tint[0] / 255 * br) >> 16);
  u32 g = (u32)(((s64)m->tg * n->tint[1] / 255 * br) >> 16);
  u32 b = (u32)(((s64)m->tb * n->tint[2] / 255 * br) >> 16);
  return rgba(r, g, b, fade >= 256u ? 255u : fade);
}

/* Lambert lighting into the node's own vertex copy: 0.32 ambient plus 0.68 of
 * a light from the top left, fixed to the camera. */
static void light_node(int i) {
  const BnNode *n = &bn.node[i];
  const BnMesh *m = &bn.mesh[n->mesh];
  const Mx *w = &bn.world[i];
  BnVert *v = (BnVert *)(uintptr_t)n->lit;
  s32 r[3][3];
  for (int c = 0; c < 3; c++) {
    u32 l = col_len(w, c);
    for (int k = 0; k < 3; k++)
      r[k][c] = l ? (s32)((s64)w->m[k][c] * Q14 / (s64)l) : (k == c ? Q14 : 0);
  }
  {
    s32 lx = (r[0][0] * bn.light[0] + r[1][0] * bn.light[1] + r[2][0] * bn.light[2]) >> 14;
    s32 ly = (r[0][1] * bn.light[0] + r[1][1] * bn.light[1] + r[2][1] * bn.light[2]) >> 14;
    s32 lz = (r[0][2] * bn.light[0] + r[1][2] * bn.light[1] + r[2][2] * bn.light[2]) >> 14;
    const s16 *nr = m->nrm;
    for (u32 k = 0; k < m->vcount; k++, nr += 3) {
      s32 d = (nr[0] * lx + nr[1] * ly + nr[2] * lz) >> 14;
      u32 s = 82u + (d > 0 ? (u32)((174 * d) >> 14) : 0u);
      float f = lut256[s > 256u ? 256u : s];
      v[k].col[0] = v[k].col[1] = v[k].col[2] = f;
    }
  }
}

static void draw_mesh(Draw *dr, int i, u32 ms, int pass) {
  const BnNode *n = &bn.node[i];
  const BnMesh *m = &bn.mesh[n->mesh];
  int tex = m->tex >= 0 && (u32)m->tex < bn.ntex ? m->tex : -1;
  int alpha = tex >= 0 ? P3D_ALPHA_TEXTURE : P3D_ALPHA_ONE;
  P3dMtx mvp;
  if (!m->icount)
    return;
  if (n->lit)
    light_node(i);
  node_mvp(i, &mvp);
  bind(dr, tex, alpha, TEX_WRAP_REPEAT);
  p3d_stage1_tint(&dr->l, node_colour(n, m, ms, dr->fade));
  if (pass == 0)
    p3d_fragment(&dr->l, 1, 1, tex >= 0 ? 250 : -1);
  else
    p3d_fragment(&dr->l, 1, 0, pass == 1 ? 4 : -1);
  p3d_uniform(&dr->l, MODEL_SHADER_UNIF_MVP, &mvp);
  p3d_buffer0(&dr->l, (n->lit ? n->lit : m->vbase) - bn.base, sizeof(BnVert), 3);
  p3d_draw_elements(&dr->l, m->index - bn.base, m->icount);
}

/* Sprites and glow halos, all additive: world-space quads in the dynamic
 * buffer, drawn a texture at a time. */
typedef struct {
  u8 tex;
  u16 node;
  u8 halo;
} Sprite;

static float halo_chan(u32 tex, u32 mesh, u32 node, s64 gain) {
  s64 v = ((s64)(tex * mesh / 255u * node / 255u) * gain) >> 16;
  return lut255[v > 255 ? 255 : v < 0 ? 0 : v];
}

static void corner(float *o, const Mx *w, s32 x, s32 y) {
  for (int k = 0; k < 3; k++)
    o[k] = q16f(w->m[k][3] + qmul(w->m[k][0], x) + qmul(w->m[k][1], y));
}

static void emit_sprite(Draw *dr, const Sprite *s, u32 ms) {
  const BnNode *n = &bn.node[s->node];
  const BnMesh *m = &bn.mesh[n->mesh];
  const Mx *w = &bn.world[s->node];
  float col[4], p[3];
  u32 f;
  s32 br = brightness(n, ms);
  if (!quad_start(dr, &f))
    return;
  if (s->halo) {
    const BnTex *t = m->tex >= 0 && (u32)m->tex < bn.ntex ? &bn.tex[m->tex] : 0;
    u32 l = col_len(w, 0), l1 = col_len(w, 1), l2 = col_len(w, 2);
    s32 hs;
    s64 g = (s64)n->glow * bn.bloom_int / 255;
    u32 tr = t ? t->r : 255u, tg = t ? t->g : 255u, tb = t ? t->b : 255u;
    static const s32 cx[4] = {-1, 1, -1, 1}, cy[4] = {1, 1, -1, -1};
    if (l1 > l)
      l = l1;
    if (l2 > l)
      l = l2;
    hs = qmul(qmul(m->radius, (s32)l), bn.halo_k);
    g = (g * br) >> 16;
    col[0] = halo_chan(tr, m->tr, n->tint[0], g);
    col[1] = halo_chan(tg, m->tg, n->tint[1], g);
    col[2] = halo_chan(tb, m->tb, n->tint[2], g);
    col[3] = 1.0f;
    for (int k = 0; k < 4; k++) {
      for (int a = 0; a < 3; a++)
        p[a] = q16f(w->m[a][3] + (s32)(((s64)bn.right[a] * hs * cx[k] + (s64)bn.up[a] * hs * cy[k]) >> 14));
      set_vert(&dr->v[f + (u32)k], p[0], p[1], p[2], k & 1 ? 1.0f : 0.0f, k < 2 ? 1.0f : 0.0f, col);
    }
    {
      static const u16 pat[6] = {0, 2, 1, 1, 2, 3};
      for (int i = 0; i < 6; i++)
        dr->ix[dr->ni + (u32)i] = (u16)(f + pat[i]);
    }
  } else {
    u32 c = node_colour(n, m, ms, 256);
    col[0] = lut255[c & 255u];
    col[1] = lut255[(c >> 8) & 255u];
    col[2] = lut255[(c >> 16) & 255u];
    col[3] = 1.0f;
    (void)br;
    for (int k = 0; k < 4; k++) {
      corner(p, w, m->qx[k], m->qy[k]);
      set_vert(&dr->v[f + (u32)k], p[0], p[1], p[2], m->qu[k], m->qt[k], col);
    }
    for (int i = 0; i < 6; i++)
      dr->ix[dr->ni + (u32)i] = (u16)(f + m->qi[i]);
  }
  dr->nv += 4;
  dr->ni += 6;
}

static int is_sprite(const BnNode *n) {
  return n->mesh >= 0 && (n->flags & F_ADDITIVE) && (n->flags & F_BILLBOARD) &&
         bn.mesh[n->mesh].quad;
}

static s32 view_depth(int i) {
  const Mx *w = &bn.world[i];
  s64 d = 0;
  for (int k = 0; k < 3; k++)
    d += (s64)bn.back[k] * (w->m[k][3] - bn.eye[k]);
  return (s32)(d >> 14);
}

int banner_draw(u32 ms, u32 fade, u32 dst) {
  static const u8 inputs[3] = {P3D_FMT_FLOAT4, P3D_FMT_FLOAT2, P3D_FMT_FLOAT4};
  static u16 order[MAX_NODE];
  static Sprite spr[MAX_NODE * 2];
  Draw dr;
  u32 n = 0, ns = 0, bytes;
  if (!bn.ok)
    return 0;
  dr.v = (BnVert *)(uintptr_t)bn.dynv;
  dr.ix = (u16 *)(uintptr_t)bn.dyni;
  dr.nv = dr.ni = 0;
  dr.tex = -2;
  dr.alpha = -1;
  dr.fade = fade > 256u ? 256u : fade;
  eval_nodes(ms);

  p3d_begin(&dr.l, (u32 *)(uintptr_t)bn.list, LIST_WORDS);
  p3d_target(&dr.l);
  p3d_shader(&dr.l, &model_shader);
  p3d_inputs(&dr.l, inputs, 3);
  p3d_vertex_colour(&dr.l);
  p3d_depth(&dr.l, 1);
  p3d_buffers(&dr.l, bn.base);
  background(&dr, ms);

  for (int pass = 0; pass < 3; pass++) {
    n = 0;
    for (u32 i = 0; i < bn.nnode; i++) {
      const BnNode *nd = &bn.node[i];
      int selected;
      if (nd->mesh < 0 || is_sprite(nd))
        continue;
      {
        const BnMesh *m = &bn.mesh[nd->mesh];
        int tr = m->tex >= 0 && (u32)m->tex < bn.ntex && bn.tex[m->tex].translucent;
        selected = (nd->flags & F_ADDITIVE) ? pass == 2 :
                   pass == 0 ? dr.fade >= 256u :
                   pass == 1 && (tr || dr.fade < 256u);
      }
      if (selected)
        order[n++] = (u16)i;
    }
    if (pass == 1) {
      for (u32 a = 1; a < n; a++) {
        u16 o = order[a];
        s32 z = view_depth(o);
        u32 b = a;
        for (; b > 0 && view_depth(order[b - 1]) > z; b--)
          order[b] = order[b - 1];
        order[b] = o;
      }
    }
    if (pass == 2)
      p3d_blend(&dr.l, 1);
    for (u32 k = 0; k < n; k++)
      draw_mesh(&dr, order[k], ms, pass);
  }

  for (u32 i = 0; i < bn.nnode; i++) {
    const BnNode *nd = &bn.node[i];
    if (nd->mesh < 0)
      continue;
    if (is_sprite(nd)) {
      const BnMesh *m = &bn.mesh[nd->mesh];
      spr[ns++] = (Sprite){(u8)(m->tex >= 0 ? m->tex : 0xFF), (u16)i, 0};
    }
    if (bn.bloom && nd->glow && bn.halo_tex >= 0)
      spr[ns++] = (Sprite){(u8)bn.halo_tex, (u16)i, 1};
  }
  if (ns) {
    p3d_uniform(&dr.l, MODEL_SHADER_UNIF_MVP, &bn.viewproj);
    p3d_fragment(&dr.l, 1, 0, -1);
    p3d_stage1_tint(&dr.l, rgba(255, 255, 255, dr.fade >= 256u ? 255u : dr.fade));
    for (u32 t = 0; t <= bn.ntex; t++) {
      u32 fv = dr.nv, fi = dr.ni;
      for (u32 k = 0; k < ns; k++)
        if (spr[k].tex == t)
          emit_sprite(&dr, &spr[k], ms);
      if (dr.ni == fi)
        continue;
      for (u32 k = fi; k < dr.ni; k++)
        dr.ix[k] = (u16)(dr.ix[k] - fv);
      bind(&dr, (int)t, P3D_ALPHA_TEXTURE, TEX_WRAP_CLAMP);
      draw_dyn(&dr, fv, fi, dr.ni - fi);
    }
  }
  p3d_blend(&dr.l, 0);

  if (bn.img_tex >= 0) {
    float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    const BnTex *t = &bn.tex[bn.img_tex];
    float x0 = bn.img_x, y0 = bn.img_y;
    float x1 = x0 + (float)(bn.img_w * bn.img_scale) * (1.0f / 256.0f);
    float y1 = y0 + (float)(bn.img_h * bn.img_scale) * (1.0f / 256.0f);
    p3d_uniform(&dr.l, MODEL_SHADER_UNIF_MVP, &bn.ortho);
    p3d_fragment(&dr.l, 0, 0, 4);
    p3d_stage1_tint(&dr.l, rgba(255, 255, 255, dr.fade >= 256u ? 255u : dr.fade));
    bind(&dr, bn.img_tex, P3D_ALPHA_TEXTURE, TEX_WRAP_CLAMP);
    screen_quad(&dr, x0, y0, x1, y1, 0.0f, 1.0f, (float)bn.img_w / t->w,
                1.0f - (float)bn.img_h / t->h, white, white);
  }

  p3d_flush(&dr.l);
  bytes = p3d_end(&dr.l);
  os_dcache_flush();
  return bytes && gpu_p3d(bn.list, bytes, dst, 0x000000FFu);
}
