#include "soft3d.h"

#define NEAR (S3D_ONE / 4) /* vertices closer than this drop their faces */
#define AMBIENT 112        /* light levels in 1/256 */
#define DIFFUSE 160

/* Columns run [ceil(a - 0.5), ceil(b - 0.5)) for an edge from a to b, which
 * gives two faces sharing an edge every column exactly once between them. The
 * same rule picks rows. Coordinates are 16.16 pixels. */
static inline int px_ceil(s32 v) { return (v - 0x8000 + 0xFFFF) >> 16; }

/* Folded onto a quarter turn, then the Taylor series to x^9 in Q28. */
s32 s3d_sin(u32 angle) {
  s32 a = (s32)(angle & 0xFFFFu), x, x2, t;
  if (a >= 32768)
    a -= 65536;
  if (a > 16384)
    a = 32768 - a;
  else if (a < -16384)
    a = -32768 - a;
  x = a * 25736; /* 2 pi / 65536 in Q28 */
  x2 = (s32)(((s64)x * x) >> 28);
  t = (1 << 28) - x2 / 72;
  t = (1 << 28) - (s32)(((s64)x2 * t) >> 28) / 42;
  t = (1 << 28) - (s32)(((s64)x2 * t) >> 28) / 20;
  t = (1 << 28) - (s32)(((s64)x2 * t) >> 28) / 6;
  return (s32)(((s64)x * t) >> 42);
}

s32 s3d_cos(u32 angle) { return s3d_sin(angle + S3D_TURN / 4u); }

static u32 isqrt64(u64 v) {
  u64 r = 0, bit = 1ull << 62;
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

/* Bytes p..p+bytes of a run of pixels starting on a pixel boundary, one colour.
 * Stores cost per instruction rather than per byte here (docs/gpu.md "The cost
 * of a CPU pixel"), so the middle goes out in words. */
static void fill_run(volatile u8 *p, u32 bytes, const u8 bgr[3]) {
  u32 ph = 0;
  while (bytes && ((u32)p & 3u)) {
    *p++ = bgr[ph];
    ph = ph == 2 ? 0 : ph + 1;
    bytes--;
  }
  if (bytes >= 12) {
    u32 c0 = bgr[ph], c1 = bgr[ph == 2 ? 0 : ph + 1],
        c2 = bgr[ph == 0 ? 2 : ph - 1];
    u32 w0 = c0 | (c1 << 8) | (c2 << 16) | (c0 << 24);
    u32 w1 = c1 | (c2 << 8) | (c0 << 16) | (c1 << 24);
    u32 w2 = c2 | (c0 << 8) | (c1 << 16) | (c2 << 24);
    volatile u32 *w = (volatile u32 *)p;
    for (; bytes >= 12; bytes -= 12, w += 3) {
      w[0] = w0;
      w[1] = w1;
      w[2] = w2;
    }
    p = (volatile u8 *)w;
  }
  while (bytes--) {
    *p++ = bgr[ph];
    ph = ph == 2 ? 0 : ph + 1;
  }
}

/* 16.16 pixels far off screen, kept in range for 32 bits. */
static s32 clamp_px(s64 v) {
  const s64 lim = (s64)16384 << 16;
  return (s32)(v > lim ? lim : (v < -lim ? -lim : v));
}

/* The two boundary chains of the face being drawn, per column, as 16.16 y. */
static s32 chain_y[2][TOP_SCREEN_WIDTH];

/* A convex polygon of n screen points (16.16), any winding. */
static void raster(volatile u8 *fb, int w, int h, const s32 *sx, const s32 *sy,
                   int n, const u8 bgr[3]) {
  s32 xmin = sx[0], xmax = sx[0];
  int c0, c1;

  for (int i = 1; i < n; i++) {
    if (sx[i] < xmin)
      xmin = sx[i];
    if (sx[i] > xmax)
      xmax = sx[i];
  }
  c0 = px_ceil(xmin);
  c1 = px_ceil(xmax);
  if (c0 < 0)
    c0 = 0;
  if (c1 > w)
    c1 = w;
  if (c0 >= c1)
    return;

  /* Edges heading right form one chain and edges heading left the other. Each
   * edge is walked from its left end whichever way it runs, so a face on
   * either side of it computes the same y in every column. */
  for (int i = 0; i < n; i++) {
    int j = i + 1 == n ? 0 : i + 1, side = sx[j] > sx[i];
    s32 xa = sx[i], ya = sy[i], xb = sx[j], yb = sy[j];
    s64 slope, y; /* a nearly vertical edge can cross one column steeply */
    int e0, e1;
    if (xa == xb)
      continue;
    if (xa > xb) {
      s32 t = xa;
      xa = xb;
      xb = t;
      t = ya;
      ya = yb;
      yb = t;
    }
    e0 = px_ceil(xa);
    e1 = px_ceil(xb);
    if (e0 < c0)
      e0 = c0;
    if (e1 > c1)
      e1 = c1;
    if (e0 >= e1)
      continue;
    slope = (((s64)(yb - ya)) << 16) / (xb - xa);
    y = ya + ((((s64)(e0 << 16) + 0x8000 - xa) * slope) >> 16);
    for (int c = e0; c < e1; c++, y += slope)
      chain_y[side][c] = clamp_px(y);
  }

  for (int c = c0; c < c1; c++) {
    s32 a = chain_y[0][c], b = chain_y[1][c];
    int r0 = px_ceil(a < b ? a : b), r1 = px_ceil(a < b ? b : a);
    if (r0 < 0)
      r0 = 0;
    if (r1 > h)
      r1 = h;
    if (r0 < r1)
      fill_run(fb + ((u32)c * (u32)h + (u32)(h - r1)) * 3u,
               (u32)(r1 - r0) * 3u, bgr);
  }
}

/* Anti-aliases one edge of a face just drawn. raster() fills the pixels whose
 * centres are inside, so this blends the face's colour into the row of pixels
 * just outside, by how much of each the face covers: from half at the edge
 * down to none a pixel away. (ix, iy) is any point inside. */
static void edge_aa(volatile u8 *fb, int w, int h, s32 xa, s32 ya, s32 xb,
                    s32 yb, s32 ix, s32 iy, const u8 bgr[3]) {
  s32 dx = xb - xa, dy = yb - ya;
  s64 adx = dx < 0 ? -(s64)dx : dx, ady = dy < 0 ? -(s64)dy : dy;
  int xmajor = adx >= ady, below, e0, e1, lim;
  u32 len = isqrt64((u64)(adx * adx + ady * ady));
  s32 k, slope, m;

  if (!len)
    return;
  /* From the left or top end, so either face gives the same steps. */
  if (xmajor ? xa > xb : ya > yb) {
    s32 t = xa;
    xa = xb;
    xb = t;
    t = ya;
    ya = yb;
    yb = t;
    dx = -dx;
    dy = -dy;
  }
  /* Turns a distance along the minor axis into the distance from the edge. */
  k = (s32)(((xmajor ? adx : ady) << 16) / len);
  /* Which side of the edge the face is on, from the sign of a cross product:
   * below it for an x-major edge, to its right for a y-major one. */
  below = ((s64)dx * (iy - ya) - (s64)dy * (ix - xa)) > 0;
  if (!xmajor)
    below = !below;

  if (xmajor) {
    e0 = px_ceil(xa);
    e1 = px_ceil(xb);
    slope = (s32)((((s64)dy) << 16) / dx);
    m = ya + (s32)(((s64)((e0 << 16) + 0x8000 - xa) * slope) >> 16);
    lim = w;
  } else {
    e0 = px_ceil(ya);
    e1 = px_ceil(yb);
    slope = (s32)((((s64)dx) << 16) / dy);
    m = xa + (s32)(((s64)((e0 << 16) + 0x8000 - ya) * slope) >> 16);
    lim = h;
  }
  for (int i = e0; i < e1; i++, m += slope) {
    int o = below ? px_ceil(m) - 1 : px_ceil(m), cov, c, r;
    s32 d = below ? m - ((o << 16) + 0x8000) : ((o << 16) + 0x8000) - m;
    volatile u8 *p;
    if (i < 0 || i >= lim)
      continue;
    cov = (0x8000 - (s32)(((s64)d * k) >> 16)) >> 8; /* 128 is half */
    if (cov <= 0)
      continue;
    c = xmajor ? i : o;
    r = xmajor ? o : i;
    if (c < 0 || c >= w || r < 0 || r >= h)
      continue;
    p = fb + ((u32)c * (u32)h + (u32)(h - 1 - r)) * 3u;
    p[0] = (u8)(p[0] + (((bgr[0] - p[0]) * cov) >> 8));
    p[1] = (u8)(p[1] + (((bgr[1] - p[1]) * cov) >> 8));
    p[2] = (u8)(p[2] + (((bgr[2] - p[2]) * cov) >> 8));
  }
}

typedef struct {
  s32 depth;
  int face;
} Order;

/* Whether a face drawn after order[from - 1] shares the edge a-b, which it
 * runs the other way round. Only the later of two faces on an edge smooths
 * it: that blends straight into the face beside it, where smoothing both
 * would let the background show through the seam. */
static int edge_shared_later(const S3dMesh *mesh, const Order *order,
                             int count, int from, int a, int b) {
  for (int i = from; i < count; i++) {
    const S3dFace *g = &mesh->faces[order[i].face];
    int n = g->count > 4 ? 4 : g->count;
    for (int k = 0; k < n; k++)
      if (g->v[k] == b && g->v[k + 1 == n ? 0 : k + 1] == a)
        return 1;
  }
  return 0;
}

/* Flat shade: ambient plus diffuse from the face's normal, which comes from
 * its first three vertices. */
static void shade(const S3dMesh *mesh, const S3dFace *fc, s32 lx, s32 ly,
                  s32 lz, u8 bgr[3]) {
  const S3dVec *v0 = &mesh->verts[fc->v[0]], *v1 = &mesh->verts[fc->v[1]],
               *v2 = &mesh->verts[fc->v[2]];
  s64 ax = v1->x - v0->x, ay = v1->y - v0->y, az = v1->z - v0->z;
  s64 bx = v2->x - v0->x, by = v2->y - v0->y, bz = v2->z - v0->z;
  s64 nx = ay * bz - az * by, ny = az * bx - ax * bz, nz = ax * by - ay * bx;
  u32 len = isqrt64((u64)(nx * nx + ny * ny + nz * nz));
  s32 ndl = len ? (s32)((nx * lx + ny * ly + nz * lz) / len) : 0;
  int lvl = AMBIENT + (ndl > 0 ? (DIFFUSE * ndl) >> 14 : 0), c;

  c = (fc->color.b * lvl) >> 8;
  bgr[0] = (u8)(c > 255 ? 255 : c);
  c = (fc->color.g * lvl) >> 8;
  bgr[1] = (u8)(c > 255 ? 255 : c);
  c = (fc->color.r * lvl) >> 8;
  bgr[2] = (u8)(c > 255 ? 255 : c);
}

static void unit_light(const S3dVec *light, s32 *lx, s32 *ly, s32 *lz) {
  u32 len = isqrt64((u64)((s64)light->x * light->x +
                          (s64)light->y * light->y +
                          (s64)light->z * light->z));
  if (!len)
    len = 1;
  *lx = (s32)(((s64)light->x << 14) / len);
  *ly = (s32)(((s64)light->y << 14) / len);
  *lz = (s32)(((s64)light->z << 14) / len);
}

Color s3d_face_colour(const S3dMesh *mesh, int face, const S3dVec *light) {
  s32 lx, ly, lz;
  u8 bgr[3];
  Color c;
  unit_light(light, &lx, &ly, &lz);
  shade(mesh, &mesh->faces[face], lx, ly, lz, bgr);
  c.r = bgr[2];
  c.g = bgr[1];
  c.b = bgr[0];
  return c;
}

int s3d_draw(volatile u8 *fb, int w, int h, const S3dMesh *mesh,
             const S3dCamera *cam, const S3dVec *light) {
  static s32 px[S3D_MAX_VERTS], py[S3D_MAX_VERTS], pz[S3D_MAX_VERTS];
  static Order order[S3D_MAX_FACES];
  s32 cyaw = s3d_cos(cam->yaw), syaw = s3d_sin(cam->yaw);
  s32 cpit = s3d_cos((u32)cam->pitch), spit = s3d_sin((u32)cam->pitch);
  /* Camera basis in Q14: right, up, and back towards the camera. */
  s32 rx = cyaw, rz = -syaw;
  s32 ux = -((spit * syaw) >> 14), uy = cpit, uz = -((spit * cyaw) >> 14);
  s32 bx = (cpit * syaw) >> 14, by = spit, bz = (cpit * cyaw) >> 14;
  s32 lx, ly, lz;
  int nv = mesh->vert_count, nf = mesh->face_count, count = 0;
  /* Where the convergence distance sits for this eye, in 16.16 pixels. */
  s64 shift = cam->conv ? (((s64)cam->eye * cam->focal) << 16) / cam->conv : 0;

  if (nv > S3D_MAX_VERTS)
    nv = S3D_MAX_VERTS;
  if (nf > S3D_MAX_FACES)
    nf = S3D_MAX_FACES;
  screen_touch(fb);

  unit_light(light, &lx, &ly, &lz);

  for (int i = 0; i < nv; i++) {
    const S3dVec *v = &mesh->verts[i];
    s64 vx = (((s64)v->x * rx + (s64)v->z * rz) >> 14) - cam->eye;
    s64 vy = ((s64)v->x * ux + (s64)v->y * uy + (s64)v->z * uz) >> 14;
    s64 vz = cam->dist - (((s64)v->x * bx + (s64)v->y * by +
                           (s64)v->z * bz) >> 14);
    pz[i] = (s32)vz;
    if (vz < NEAR)
      continue;
    px[i] = clamp_px(((s64)w << 15) + (vx * cam->focal << 16) / vz + shift);
    py[i] = (s32)(((s64)h << 15) - clamp_px((vy * cam->focal << 16) / vz));
  }

  for (int f = 0; f < nf; f++) {
    const S3dFace *fc = &mesh->faces[f];
    int n = fc->count > 4 ? 4 : fc->count, near = 0;
    s64 area = 0;
    s32 depth = 0;
    if (n < 3)
      continue;
    for (int k = 0; k < n; k++) {
      int a = fc->v[k], b = fc->v[k + 1 == n ? 0 : k + 1];
      if (pz[a] < NEAR)
        near = 1;
      depth += pz[a];
      area += ((s64)px[a] * py[b] - (s64)px[b] * py[a]) >> 16;
    }
    /* Counter-clockwise from outside comes out clockwise with y pointing
     * down the screen. */
    if (near || area >= 0)
      continue;
    order[count].depth = depth / n;
    order[count].face = f;
    count++;
  }

  for (int i = 1; i < count; i++) { /* far to near */
    Order o = order[i];
    int j = i;
    for (; j > 0 && order[j - 1].depth < o.depth; j--)
      order[j] = order[j - 1];
    order[j] = o;
  }

  for (int i = 0; i < count; i++) {
    const S3dFace *fc = &mesh->faces[order[i].face];
    int n = fc->count > 4 ? 4 : fc->count;
    s32 sx[4] = {0}, sy[4] = {0};
    s64 cx = 0, cy = 0;
    u8 bgr[3];
    shade(mesh, fc, lx, ly, lz, bgr);
    for (int k = 0; k < n; k++) {
      sx[k] = px[fc->v[k]];
      sy[k] = py[fc->v[k]];
      cx += sx[k];
      cy += sy[k];
    }
    raster(fb, w, h, sx, sy, n, bgr);
    for (int k = 0; k < n; k++) {
      int j = k + 1 == n ? 0 : k + 1;
      if (!edge_shared_later(mesh, order, count, i + 1, fc->v[k], fc->v[j]))
        edge_aa(fb, w, h, sx[k], sy[k], sx[j], sy[j], (s32)(cx / n),
                (s32)(cy / n), bgr);
    }
  }
  return count;
}
