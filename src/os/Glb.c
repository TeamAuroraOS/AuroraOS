/* glTF 2.0 binary (.glb) to PICA200-ready data.
 *
 * Format: the Khronos glTF 2.0 specification (GLB header and chunks,
 * accessors, buffer views, the node hierarchy, materials). Texture layout on
 * the PICA200 (8x8 tiles in Morton order, RGBA8 stored A, B, G, R, image top
 * row first so t = 1 - v): GBATEK "3DS GPU Texture Formats" and devkitPro's
 * tex3ds swizzle. See docs/glb.md. */
#include "glb.h"
#include "ff.h"
#include "image.h"
#include "json.h"
#include "soft3d.h"

#define CT_BYTE   5120
#define CT_UBYTE  5121
#define CT_SHORT  5122
#define CT_USHORT 5123
#define CT_UINT   5125
#define CT_FLOAT  5126

#define WRAP_CLAMP  0u /* GPU wrap modes */
#define WRAP_REPEAT 2u
#define WRAP_MIRROR 3u

#define MAX_INST  512
#define MAX_DEPTH 32

/* The light baked into vertex colours: ambient, a key light from the front,
 * above and to the right, and a dimmer fill from behind so no side is flat. */
#define AMBIENT 0.42f
static const float key_dir[3] = {0.40f, 0.75f, 0.53f};
static const float fill_dir[3] = {-0.50f, 0.25f, -0.83f};
#define KEY  0.58f
#define FILL 0.25f

static GlbPrim prims[GLB_MAX_PRIMS];
static GlbMat mats[GLB_MAX_MATS];
static GlbTex texs[GLB_MAX_TEX];
static int tex_key[GLB_MAX_TEX];

static struct {
  int node, mesh;
  float m[16]; /* column-major, as glTF stores matrices */
} inst[MAX_INST];
static u32 ninst;

typedef struct {
  Json js;
  const u8 *bin;
  u32 binlen;
  int root;
  char *err;
  u32 errmax;
  GlbProgress cb;
  u32 used; /* arena bytes */
} Ctx;

typedef struct {
  const u8 *p;
  u32 count, stride;
  int ctype, n, norm;
} Acc;

static void copy_str(char *d, const char *s, u32 max) {
  u32 i = 0;
  while (s[i] && i + 1 < max) {
    d[i] = s[i];
    i++;
  }
  d[i] = 0;
}

static int fail(Ctx *c, const char *why) {
  copy_str(c->err, why, c->errmax);
  return 0;
}

static u32 rd32(const u8 *p) {
  return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static float rdf(const u8 *p) {
  union {
    u32 u;
    float f;
  } x;
  x.u = rd32(p);
  return x.f;
}

/* A few Newton steps from an exponent-halving first guess. */
static float fsqrt(float x) {
  union {
    float f;
    u32 u;
  } g;
  if (x <= 0.0f)
    return 0.0f;
  g.f = x;
  g.u = (g.u >> 1) + 0x1FC00000u;
  for (int i = 0; i < 4; i++)
    g.f = 0.5f * (g.f + x / g.f);
  return g.f;
}

static void normalise(float v[3]) {
  float l = fsqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
  if (l > 0.0f) {
    v[0] /= l;
    v[1] /= l;
    v[2] /= l;
  }
}

static void *arena_alloc(Ctx *c, u32 size, u32 align) {
  u32 at = (c->used + align - 1u) & ~(align - 1u);
  if (at + size > GLB_ARENA_SIZE || at + size < at)
    return 0;
  c->used = at + size;
  return (void *)(GLB_ARENA_ADDR + at);
}

/* Column-major 4x4. */
static void mat_mul(float *o, const float *a, const float *b) {
  float t[16];
  for (int col = 0; col < 4; col++)
    for (int row = 0; row < 4; row++)
      t[col * 4 + row] = a[row] * b[col * 4] + a[4 + row] * b[col * 4 + 1] +
                         a[8 + row] * b[col * 4 + 2] +
                         a[12 + row] * b[col * 4 + 3];
  for (int i = 0; i < 16; i++)
    o[i] = t[i];
}

static void mat_identity(float *m) {
  for (int i = 0; i < 16; i++)
    m[i] = (i % 5) ? 0.0f : 1.0f;
}

static float num_at(Ctx *c, int arr, u32 i, float def) {
  return (float)json_num(&c->js, json_at(&c->js, arr, i), def);
}

/* A node's own transform: its matrix, or translation * rotation * scale. */
static void node_local(Ctx *c, int node, float *m) {
  Json *j = &c->js;
  int mat = json_get(j, node, "matrix");
  int t = json_get(j, node, "translation"), r = json_get(j, node, "rotation"),
      s = json_get(j, node, "scale");
  float x, y, z, w, sx, sy, sz;

  if (mat >= 0 && json_count(j, mat) == 16) {
    for (u32 i = 0; i < 16; i++)
      m[i] = num_at(c, mat, i, (i % 5) ? 0.0f : 1.0f);
    return;
  }
  x = num_at(c, r, 0, 0.0f);
  y = num_at(c, r, 1, 0.0f);
  z = num_at(c, r, 2, 0.0f);
  w = num_at(c, r, 3, 1.0f);
  sx = num_at(c, s, 0, 1.0f);
  sy = num_at(c, s, 1, 1.0f);
  sz = num_at(c, s, 2, 1.0f);
  m[0] = (1 - 2 * (y * y + z * z)) * sx;
  m[1] = (2 * (x * y + z * w)) * sx;
  m[2] = (2 * (x * z - y * w)) * sx;
  m[3] = 0;
  m[4] = (2 * (x * y - z * w)) * sy;
  m[5] = (1 - 2 * (x * x + z * z)) * sy;
  m[6] = (2 * (y * z + x * w)) * sy;
  m[7] = 0;
  m[8] = (2 * (x * z + y * w)) * sz;
  m[9] = (2 * (y * z - x * w)) * sz;
  m[10] = (1 - 2 * (x * x + y * y)) * sz;
  m[11] = 0;
  m[12] = num_at(c, t, 0, 0.0f);
  m[13] = num_at(c, t, 1, 0.0f);
  m[14] = num_at(c, t, 2, 0.0f);
  m[15] = 1;
}

static int walk(Ctx *c, int idx, const float *parent, int depth) {
  Json *j = &c->js;
  int node = json_at(j, json_get(j, c->root, "nodes"), (u32)idx);
  int kids;
  float local[16], world[16];

  if (node < 0)
    return fail(c, "a scene names a node that does not exist");
  if (depth > MAX_DEPTH)
    return fail(c, "the node hierarchy is too deep");
  node_local(c, node, local);
  mat_mul(world, parent, local);
  if (json_get(j, node, "mesh") >= 0) {
    if (ninst >= MAX_INST)
      return fail(c, "too many mesh instances");
    inst[ninst].node = node;
    inst[ninst].mesh = json_int(j, json_get(j, node, "mesh"), -1);
    for (int i = 0; i < 16; i++)
      inst[ninst].m[i] = world[i];
    ninst++;
  }
  kids = json_get(j, node, "children");
  for (u32 i = 0; i < json_count(j, kids); i++)
    if (!walk(c, json_int(j, json_at(j, kids, i), -1), world, depth + 1))
      return 0;
  return 1;
}

static int comp_size(int ct) {
  switch (ct) {
    case CT_BYTE: case CT_UBYTE:   return 1;
    case CT_SHORT: case CT_USHORT: return 2;
    case CT_UINT: case CT_FLOAT:   return 4;
    default:                       return 0;
  }
}

static int type_n(Ctx *c, int t) {
  static const char *const names[] = {"SCALAR", "VEC2", "VEC3", "VEC4"};
  for (int i = 0; i < 4; i++)
    if (json_is(&c->js, t, names[i]))
      return i + 1;
  return 0;
}

/* The byte range of a buffer view, inside the GLB's binary chunk. */
static const u8 *view_data(Ctx *c, int idx, u32 *len, u32 *stride) {
  Json *j = &c->js;
  int v = json_at(j, json_get(j, c->root, "bufferViews"), (u32)idx);
  int buf;
  u32 off, n;
  if (v < 0)
    return 0;
  buf = json_at(j, json_get(j, c->root, "buffers"),
                (u32)json_int(j, json_get(j, v, "buffer"), 0));
  if (buf < 0 || json_get(j, buf, "uri") >= 0)
    return 0; /* data in another file: not supported */
  off = (u32)json_int(j, json_get(j, v, "byteOffset"), 0);
  n = (u32)json_int(j, json_get(j, v, "byteLength"), 0);
  if (off > c->binlen || n > c->binlen - off)
    return 0;
  if (stride)
    *stride = (u32)json_int(j, json_get(j, v, "byteStride"), 0);
  *len = n;
  return c->bin + off;
}

static int acc_open(Ctx *c, int idx, Acc *a) {
  Json *j = &c->js;
  int t = json_at(j, json_get(j, c->root, "accessors"), (u32)idx);
  u32 vlen, vstride, off, cs, need;
  const u8 *v;

  if (t < 0 || json_get(j, t, "sparse") >= 0)
    return 0;
  v = view_data(c, json_int(j, json_get(j, t, "bufferView"), -1), &vlen,
                &vstride);
  if (!v)
    return 0;
  a->ctype = json_int(j, json_get(j, t, "componentType"), 0);
  a->n = type_n(c, json_get(j, t, "type"));
  a->count = (u32)json_int(j, json_get(j, t, "count"), 0);
  a->norm = json_bool(j, json_get(j, t, "normalized"), 0);
  cs = (u32)comp_size(a->ctype);
  if (!cs || !a->n || !a->count)
    return 0;
  off = (u32)json_int(j, json_get(j, t, "byteOffset"), 0);
  a->stride = vstride ? vstride : cs * (u32)a->n;
  need = (a->count - 1u) * a->stride + cs * (u32)a->n;
  if (off > vlen || need > vlen - off)
    return 0;
  a->p = v + off;
  return 1;
}

static float acc_f(const Acc *a, u32 i, int k) {
  const u8 *p = a->p + i * a->stride;
  switch (a->ctype) {
    case CT_FLOAT:
      return rdf(p + k * 4);
    case CT_UBYTE:
      return a->norm ? p[k] / 255.0f : (float)p[k];
    case CT_BYTE: {
      float f = (float)(s8)p[k];
      if (!a->norm)
        return f;
      f /= 127.0f;
      return f < -1.0f ? -1.0f : f;
    }
    case CT_USHORT: {
      u32 v = (u32)p[k * 2] | ((u32)p[k * 2 + 1] << 8);
      return a->norm ? v / 65535.0f : (float)v;
    }
    case CT_SHORT: {
      float f = (float)(s16)((u32)p[k * 2] | ((u32)p[k * 2 + 1] << 8));
      if (!a->norm)
        return f;
      f /= 32767.0f;
      return f < -1.0f ? -1.0f : f;
    }
    default:
      return (float)rd32(p + k * 4);
  }
}

static u32 acc_index(const Acc *a, u32 i) {
  const u8 *p = a->p + i * a->stride;
  if (a->ctype == CT_UBYTE)
    return p[0];
  if (a->ctype == CT_USHORT)
    return (u32)p[0] | ((u32)p[1] << 8);
  return rd32(p);
}

static int prim_mode(Ctx *c, int p) {
  return json_int(&c->js, json_get(&c->js, p, "mode"), 4);
}

/* Triangles a primitive of `n` indices (or vertices) makes. */
static u32 prim_tris(int mode, u32 n) {
  if (mode == 4)
    return n / 3u;
  return n >= 3u ? n - 2u : 0u;
}

static void prim_name(Ctx *c, int k, GlbPrim *gp) {
  Json *j = &c->js;
  int name = json_get(j, inst[k].node, "name");
  if (name < 0)
    name = json_get(j, json_at(j, json_get(j, c->root, "meshes"),
                               (u32)inst[k].mesh),
                    "name");
  if (name >= 0) {
    json_str(j, name, gp->name, GLB_NAME);
  } else {
    char *p = gp->name;
    int v = inst[k].mesh;
    const char *s = "Mesh ";
    while (*s)
      *p++ = *s++;
    if (v >= 100)
      *p++ = (char)('0' + v / 100 % 10);
    if (v >= 10)
      *p++ = (char)('0' + v / 10 % 10);
    *p++ = (char)('0' + v % 10);
    *p = 0;
  }
}

static float light(const float n[3]) {
  float k = n[0] * key_dir[0] + n[1] * key_dir[1] + n[2] * key_dir[2];
  float f = n[0] * fill_dir[0] + n[1] * fill_dir[1] + n[2] * fill_dir[2];
  float l = AMBIENT + (k > 0.0f ? KEY * k : 0.0f) + (f > 0.0f ? FILL * f : 0.0f);
  return l > 1.0f ? 1.0f : l;
}

static void base_colour(Ctx *c, int mat, float out[4]) {
  int f = json_get(&c->js,
                   json_get(&c->js,
                            json_at(&c->js, json_get(&c->js, c->root,
                                                     "materials"),
                                    (u32)mat),
                            "pbrMetallicRoughness"),
                   "baseColorFactor");
  for (u32 i = 0; i < 4; i++)
    out[i] = num_at(c, f, i, 1.0f);
}

/* Counts what the scene needs, so the vertex buffer can come first in the
 * arena, all in one piece. */
static int count(Ctx *c, u32 *nv, u32 *ni) {
  Json *j = &c->js;
  int meshes = json_get(j, c->root, "meshes");
  *nv = *ni = 0;
  for (u32 k = 0; k < ninst; k++) {
    int ps = json_get(j, json_at(j, meshes, (u32)inst[k].mesh), "primitives");
    for (u32 i = 0; i < json_count(j, ps); i++) {
      int p = json_at(j, ps, i), mode = prim_mode(c, p), ix;
      Acc pos, idx;
      u32 n;
      if (mode != 4 && mode != 5 && mode != 6)
        continue;
      if (!acc_open(c, json_int(j, json_get(j, json_get(j, p, "attributes"),
                                            "POSITION"), -1), &pos))
        return fail(c, "a mesh has no readable positions");
      if (pos.n != 3)
        return fail(c, "a mesh's positions are not 3D");
      if (pos.count > 65535u)
        return fail(c, "a mesh has more than 65535 vertices");
      ix = json_get(j, p, "indices");
      n = pos.count;
      if (ix >= 0) {
        if (!acc_open(c, json_int(j, ix, -1), &idx) || idx.n != 1)
          return fail(c, "a mesh has unreadable indices");
        n = idx.count;
      }
      *nv += pos.count;
      *ni += 3u * prim_tris(mode, n);
    }
  }
  return 1;
}

/* One primitive into the vertex buffer and its own index list. */
static int build_prim(Ctx *c, u32 k, int p, GlbPrim *gp, GlbVertex *vb,
                      u32 *vnext, float bmin[3], float bmax[3]) {
  Json *j = &c->js;
  int attrs = json_get(j, p, "attributes"), mode = prim_mode(c, p);
  int nrm_i = json_get(j, attrs, "NORMAL"), uv_i;
  int col_i = json_get(j, attrs, "COLOR_0"), ix = json_get(j, p, "indices");
  int mat = json_int(j, json_get(j, p, "material"), -1);
  const float *m = inst[k].m;
  Acc pos, nrm, uv, col, idx;
  int has_nrm, has_uv, has_col, unlit = 0;
  float cof[9], det, base[4], uvm[6] = {1, 0, 0, 0, 1, 0};
  float *acc_n = (float *)IMAGE_FILE_ADDR; /* normals built from faces */
  u32 n, tris;
  u16 *out;

  {
    /* The base colour texture's UV set, and its KHR_texture_transform:
     * uv' = translation + rotation * (scale * uv). */
    int bct = json_get(j, json_get(j, json_at(j, json_get(j, c->root,
                                                          "materials"),
                                              (u32)mat),
                                   "pbrMetallicRoughness"),
                       "baseColorTexture");
    int xf = json_get(j, json_get(j, bct, "extensions"),
                      "KHR_texture_transform");
    int set = json_int(j, json_get(j, bct, "texCoord"), 0);
    char name[12] = "TEXCOORD_0";
    if (xf >= 0 && json_get(j, xf, "texCoord") >= 0)
      set = json_int(j, json_get(j, xf, "texCoord"), 0);
    name[9] = (char)('0' + (set & 7));
    uv_i = json_get(j, attrs, name);
    if (xf >= 0) {
      double rot = json_num(j, json_get(j, xf, "rotation"), 0.0);
      int off = json_get(j, xf, "offset"), sc = json_get(j, xf, "scale");
      float sx = num_at(c, sc, 0, 1.0f), sy = num_at(c, sc, 1, 1.0f);
      u32 turn = (u32)(s32)(rot * (65536.0 / 6.283185307179586));
      float sn = s3d_sin(turn) / 16384.0f, cs = s3d_cos(turn) / 16384.0f;
      uvm[0] = cs * sx;
      uvm[1] = sn * sy;
      uvm[2] = num_at(c, off, 0, 0.0f);
      uvm[3] = -sn * sx;
      uvm[4] = cs * sy;
      uvm[5] = num_at(c, off, 1, 0.0f);
    }
  }
  acc_open(c, json_int(j, json_get(j, attrs, "POSITION"), -1), &pos);
  has_nrm = nrm_i >= 0 && acc_open(c, json_int(j, nrm_i, -1), &nrm) &&
            nrm.n == 3 && nrm.count == pos.count;
  has_uv = uv_i >= 0 && acc_open(c, json_int(j, uv_i, -1), &uv) &&
           uv.n == 2 && uv.count == pos.count;
  has_col = col_i >= 0 && acc_open(c, json_int(j, col_i, -1), &col) &&
            col.n >= 3 && col.count == pos.count;
  n = pos.count;
  if (ix >= 0) {
    acc_open(c, json_int(j, ix, -1), &idx);
    n = idx.count;
  }
  tris = prim_tris(mode, n);
  out = arena_alloc(c, tris * 6u + 2u, 16);
  if (!out)
    return fail(c, "the model does not fit in memory");
  gp->vfirst = *vnext;
  gp->vcount = pos.count;
  gp->index = (u32)out;
  gp->icount = tris * 3u;
  gp->mat = (mat >= 0 && (u32)mat < GLB_MAX_MATS) ? mat : -1;
  prim_name(c, (int)k, gp);
  if (mat >= 0) {
    base_colour(c, mat, base);
    unlit = json_get(j, json_get(j, json_at(j, json_get(j, c->root,
                                                        "materials"),
                                            (u32)mat),
                                 "extensions"),
                     "KHR_materials_unlit") >= 0;
  } else
    base[0] = base[1] = base[2] = base[3] = 1.0f;

  /* The normal matrix is the cofactor matrix of the upper 3x3, sign-corrected
   * for mirroring transforms; the normals are renormalised anyway. */
  {
    float a00 = m[0], a01 = m[4], a02 = m[8];
    float a10 = m[1], a11 = m[5], a12 = m[9];
    float a20 = m[2], a21 = m[6], a22 = m[10];
    cof[0] = a11 * a22 - a12 * a21;
    cof[1] = -(a10 * a22 - a12 * a20);
    cof[2] = a10 * a21 - a11 * a20;
    cof[3] = -(a01 * a22 - a02 * a21);
    cof[4] = a00 * a22 - a02 * a20;
    cof[5] = -(a00 * a21 - a01 * a20);
    cof[6] = a01 * a12 - a02 * a11;
    cof[7] = -(a00 * a12 - a02 * a10);
    cof[8] = a00 * a11 - a01 * a10;
    det = a00 * cof[0] + a01 * cof[1] + a02 * cof[2];
    if (det < 0.0f)
      for (int i = 0; i < 9; i++)
        cof[i] = -cof[i];
  }

  for (u32 i = 0; i < pos.count; i++) {
    GlbVertex *v = &vb[*vnext + i];
    float x = acc_f(&pos, i, 0), y = acc_f(&pos, i, 1), z = acc_f(&pos, i, 2);
    v->pos[0] = m[0] * x + m[4] * y + m[8] * z + m[12];
    v->pos[1] = m[1] * x + m[5] * y + m[9] * z + m[13];
    v->pos[2] = m[2] * x + m[6] * y + m[10] * z + m[14];
    v->pos[3] = 1.0f;
    for (int a = 0; a < 3; a++) {
      if (v->pos[a] < bmin[a])
        bmin[a] = v->pos[a];
      if (v->pos[a] > bmax[a])
        bmax[a] = v->pos[a];
    }
    if (has_uv) {
      float u0 = acc_f(&uv, i, 0), v0 = acc_f(&uv, i, 1);
      v->uv[0] = uvm[0] * u0 + uvm[1] * v0 + uvm[2];
      v->uv[1] = 1.0f - (uvm[3] * u0 + uvm[4] * v0 + uvm[5]);
    } else {
      v->uv[0] = v->uv[1] = 0.0f;
    }
    if (has_nrm) {
      float nx = acc_f(&nrm, i, 0), ny = acc_f(&nrm, i, 1),
            nz = acc_f(&nrm, i, 2);
      float w[3] = {cof[0] * nx + cof[1] * ny + cof[2] * nz,
                    cof[3] * nx + cof[4] * ny + cof[5] * nz,
                    cof[6] * nx + cof[7] * ny + cof[8] * nz};
      normalise(w);
      for (int a = 0; a < 3; a++)
        v->col[a] = w[a]; /* the normal, until the colour replaces it */
    } else {
      acc_n[i * 3] = acc_n[i * 3 + 1] = acc_n[i * 3 + 2] = 0.0f;
    }
    v->col[3] = 1.0f;
  }

  for (u32 t = 0; t < tris; t++) {
    u32 a, b, d;
    if (mode == 4) {
      a = t * 3u;
      b = a + 1u;
      d = a + 2u;
    } else if (mode == 5) {
      a = (t & 1u) ? t + 1u : t;
      b = (t & 1u) ? t : t + 1u;
      d = t + 2u;
    } else {
      a = 0;
      b = t + 1u;
      d = t + 2u;
    }
    if (ix >= 0) {
      a = acc_index(&idx, a);
      b = acc_index(&idx, b);
      d = acc_index(&idx, d);
    }
    if (a >= pos.count || b >= pos.count || d >= pos.count)
      a = b = d = 0; /* a broken index makes an empty triangle */
    out[t * 3u] = (u16)a;
    out[t * 3u + 1u] = (u16)b;
    out[t * 3u + 2u] = (u16)d;
    if (!has_nrm) {
      const float *pa = vb[*vnext + a].pos, *pb = vb[*vnext + b].pos,
                  *pd = vb[*vnext + d].pos;
      float e1[3] = {pb[0] - pa[0], pb[1] - pa[1], pb[2] - pa[2]};
      float e2[3] = {pd[0] - pa[0], pd[1] - pa[1], pd[2] - pa[2]};
      float fn[3] = {e1[1] * e2[2] - e1[2] * e2[1],
                     e1[2] * e2[0] - e1[0] * e2[2],
                     e1[0] * e2[1] - e1[1] * e2[0]};
      for (int q = 0; q < 3; q++) {
        acc_n[a * 3u + q] += fn[q];
        acc_n[b * 3u + q] += fn[q];
        acc_n[d * 3u + q] += fn[q];
      }
    }
  }

  for (u32 i = 0; i < pos.count; i++) {
    GlbVertex *v = &vb[*vnext + i];
    float nv[3], l, vc[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    if (has_nrm) {
      nv[0] = v->col[0];
      nv[1] = v->col[1];
      nv[2] = v->col[2];
    } else {
      nv[0] = acc_n[i * 3];
      nv[1] = acc_n[i * 3 + 1];
      nv[2] = acc_n[i * 3 + 2];
      normalise(nv);
    }
    if (has_col)
      for (int a = 0; a < col.n && a < 4; a++)
        vc[a] = acc_f(&col, i, a);
    l = unlit ? 1.0f : light(nv);
    for (int a = 0; a < 3; a++) {
      float cv = base[a] * vc[a] * l;
      v->col[a] = cv > 1.0f ? 1.0f : cv;
    }
    v->col[3] = base[3] * vc[3];
  }

  *vnext += pos.count;
  return 1;
}

/* Centres the model and scales it to GLB_RADIUS, then makes the fixed-point
 * copy the viewer searches every frame. */
static void normalise_model(GlbModel *m, GlbVertex *vb, const float bmin[3],
                            const float bmax[3], s32 *fixed) {
  float ctr[3], r2 = 0.0f, s;
  for (int a = 0; a < 3; a++)
    ctr[a] = 0.5f * (bmin[a] + bmax[a]);
  for (u32 i = 0; i < m->nverts; i++) {
    float dx = vb[i].pos[0] - ctr[0], dy = vb[i].pos[1] - ctr[1],
          dz = vb[i].pos[2] - ctr[2], d = dx * dx + dy * dy + dz * dz;
    if (d > r2)
      r2 = d;
  }
  s = r2 > 0.0f ? GLB_RADIUS / fsqrt(r2) : 1.0f;
  for (u32 i = 0; i < m->nverts; i++)
    for (int a = 0; a < 3; a++) {
      vb[i].pos[a] = (vb[i].pos[a] - ctr[a]) * s;
      fixed[i * 3 + a] = (s32)(vb[i].pos[a] * 4096.0f);
    }
  for (u32 p = 0; p < m->nprim; p++) {
    GlbPrim *gp = &m->prim[p];
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    for (u32 i = 0; i < gp->vcount; i++)
      for (int a = 0; a < 3; a++) {
        float v = vb[gp->vfirst + i].pos[a];
        if (v < lo[a])
          lo[a] = v;
        if (v > hi[a])
          hi[a] = v;
      }
    for (int a = 0; a < 3; a++)
      gp->centre[a] = 0.5f * (lo[a] + hi[a]);
  }
}

static u32 pot_le(u32 v) {
  u32 p = 8;
  while (p * 2u <= v && p < GLB_TEX_MAX)
    p *= 2u;
  return p;
}

/* Area average, with colour weighted by alpha so clear pixels do not darken
 * their neighbours. */
static void resample(const u8 *src, u32 sw, u32 sh, u8 *dst, u32 dw, u32 dh) {
  for (u32 y = 0; y < dh; y++) {
    u32 y0 = y * sh / dh, y1 = (y + 1u) * sh / dh;
    if (y1 <= y0)
      y1 = y0 + 1u;
    for (u32 x = 0; x < dw; x++) {
      u32 x0 = x * sw / dw, x1 = (x + 1u) * sw / dw, r = 0, g = 0, b = 0,
          a = 0, n = 0;
      u8 *o = dst + (y * dw + x) * 4u;
      if (x1 <= x0)
        x1 = x0 + 1u;
      for (u32 yy = y0; yy < y1; yy++)
        for (u32 xx = x0; xx < x1; xx++) {
          const u8 *s = src + (yy * sw + xx) * 4u;
          u32 w = s[3] + 1u; /* as image_decode_fit weights them */
          r += s[0] * w;
          g += s[1] * w;
          b += s[2] * w;
          a += w;
          n++;
        }
      o[0] = (u8)(r / a);
      o[1] = (u8)(g / a);
      o[2] = (u8)(b / a);
      o[3] = (u8)((a - n) / n);
    }
  }
}

/* The PICA's texel order: 8x8 tiles left to right, top to bottom, Morton
 * order inside each, every texel stored A, B, G, R. */
static void tile(const u8 *src, u32 w, u32 h, u8 *dst) {
  for (u32 y = 0; y < h; y++)
    for (u32 x = 0; x < w; x++) {
      u32 mo = (x & 1u) | ((y & 1u) << 1) | ((x & 2u) << 1) | ((y & 2u) << 2) |
               ((x & 4u) << 2) | ((y & 4u) << 3);
      u32 t = ((y >> 3) * (w >> 3) + (x >> 3)) * 64u + mo;
      const u8 *s = src + (y * w + x) * 4u;
      u8 *d = dst + t * 4u;
      d[0] = s[3];
      d[1] = s[2];
      d[2] = s[1];
      d[3] = s[0];
    }
}

static u32 wrap_mode(int gl) {
  if (gl == 33071)
    return WRAP_CLAMP;
  if (gl == 33648)
    return WRAP_MIRROR;
  return WRAP_REPEAT;
}

static int build_tex(Ctx *c, GlbTex *t, int image, int sampler) {
  Json *j = &c->js;
  int img = json_at(j, json_get(j, c->root, "images"), (u32)image);
  int smp = json_at(j, json_get(j, c->root, "samplers"), (u32)sampler);
  /* Level images in IMAGE_FILE_ADDR, the decoder's working rows after them. */
  u8 *lv = (u8 *)IMAGE_FILE_ADDR;
  u8 *next = lv + GLB_TEX_MAX * GLB_TEX_MAX * 4u;
  u8 *scratch = next + GLB_TEX_MAX * GLB_TEX_MAX * 4u;
  u32 len, tw, th, total = 0, w2, h2;
  int w = 0, h = 0, levels = 0;
  const u8 *data;
  u8 *dst;

  if (img < 0)
    return 0;
  data = view_data(c, json_int(j, json_get(j, img, "bufferView"), -1), &len,
                   0);
  if (!data || image_probe(data, len, &w, &h) != IMG_OK)
    return 0;
  tw = pot_le((u32)w);
  th = pot_le((u32)h);
  if (image_decode_fit(data, len, lv, (int)tw, (int)th, scratch,
                       IMAGE_FILE_MAX - (u32)(scratch - lv)) != IMG_OK)
    return 0;

  for (w2 = tw, h2 = th; w2 >= 8u && h2 >= 8u; w2 >>= 1, h2 >>= 1) {
    total += w2 * h2 * 4u;
    levels++;
  }
  dst = arena_alloc(c, total, 128);
  if (!dst)
    return 0;
  t->addr = (u32)dst;
  t->w = (u16)tw;
  t->h = (u16)th;
  t->levels = (u8)(levels - 1);
  t->wrap_s = (u8)wrap_mode(json_int(j, json_get(j, smp, "wrapS"), 10497));
  t->wrap_t = (u8)wrap_mode(json_int(j, json_get(j, smp, "wrapT"), 10497));
  t->nearest = json_int(j, json_get(j, smp, "magFilter"), 9729) == 9728;
  t->translucent = 0;
  for (u32 i = 0; i < tw * th; i++)
    if (lv[i * 4u + 3u] < 250u) {
      t->translucent = 1;
      break;
    }

  for (w2 = tw, h2 = th; levels--; w2 >>= 1, h2 >>= 1) {
    tile(lv, w2, h2, dst);
    dst += w2 * h2 * 4u;
    if (levels) {
      resample(lv, w2, h2, next, w2 / 2u, h2 / 2u);
      u8 *s = lv;
      lv = next;
      next = s;
    }
  }
  return 1;
}

/* Whether every vertex of every part drawn with `mat` is fully opaque; the
 * vertex alpha already holds the base colour factor's. */
static int vertices_opaque(const GlbModel *m, int mat) {
  const GlbVertex *vb = (const GlbVertex *)m->base;
  for (u32 i = 0; i < m->nprim; i++)
    if (m->prim[i].mat == mat)
      for (u32 v = 0; v < m->prim[i].vcount; v++)
        if (vb[m->prim[i].vfirst + v].col[3] < 0.999f)
          return 0;
  return 1;
}

static int build_materials(Ctx *c, GlbModel *m) {
  Json *j = &c->js;
  int list = json_get(j, c->root, "materials");
  u32 n = json_count(j, list), done = 0, want = 0;

  if (n > GLB_MAX_MATS)
    n = GLB_MAX_MATS;
  for (u32 i = 0; i < n; i++)
    if (json_get(j, json_get(j, json_at(j, list, i), "pbrMetallicRoughness"),
                 "baseColorTexture") >= 0)
      want++;
  for (u32 i = 0; i < n; i++) {
    int mt = json_at(j, list, i);
    int bct = json_get(j, json_get(j, mt, "pbrMetallicRoughness"),
                       "baseColorTexture");
    int am = json_get(j, mt, "alphaMode");
    GlbMat *gm = &mats[i];
    gm->tex = -1;
    gm->alpha = json_is(j, am, "BLEND") ? GLB_BLEND
                : json_is(j, am, "MASK") ? GLB_MASK
                                         : GLB_OPAQUE;
    gm->cutoff = (u8)(255.0 * json_num(j, json_get(j, mt, "alphaCutoff"),
                                       0.5));
    if (bct >= 0) {
      int tx = json_at(j, json_get(j, c->root, "textures"),
                       (u32)json_int(j, json_get(j, bct, "index"), -1));
      int image = json_int(j, json_get(j, tx, "source"), -1);
      int sampler = json_int(j, json_get(j, tx, "sampler"), -1);
      int key = image * 1024 + sampler + 1;
      if (c->cb)
        c->cb("Textures", done++, want);
      for (u32 k = 0; k < m->ntex; k++)
        if (tex_key[k] == key)
          gm->tex = (int)k;
      if (gm->tex < 0 && image >= 0 && m->ntex < GLB_MAX_TEX) {
        if (build_tex(c, &texs[m->ntex], image, sampler)) {
          tex_key[m->ntex] = key;
          gm->tex = (int)m->ntex++;
        } else {
          m->tex_failed++;
        }
      }
    }
    /* Exporters often mark every material BLEND. One that is opaque in
     * practice is drawn as opaque, so it writes depth and needs no sorting. */
    if (gm->alpha == GLB_BLEND &&
        (gm->tex < 0 || !texs[gm->tex].translucent) &&
        vertices_opaque(m, (int)i))
      gm->alpha = GLB_OPAQUE;
  }
  m->nmat = n;
  return 1;
}

int glb_load(const char *path, GlbModel *m, GlbProgress cb, char *err,
             u32 errmax) {
  static FIL f;
  u8 *file = (u8 *)GLB_FILE_ADDR;
  Ctx c;
  UINT br;
  u32 len, jlen, nv, ni, vnext = 0;
  int scenes, scene;
  float ident[16], bmin[3] = {1e30f, 1e30f, 1e30f},
                   bmax[3] = {-1e30f, -1e30f, -1e30f};
  GlbVertex *vb;
  s32 *fixed;

  c.err = err;
  c.errmax = errmax;
  c.cb = cb;
  c.used = 0;
  m->prim = prims;
  m->mat = mats;
  m->tex = texs;
  m->nprim = m->nmat = m->ntex = m->nverts = m->ntris = 0;
  m->skipped = m->tex_failed = 0;
  ninst = 0;

  if (f_open(&f, path, FA_READ) != FR_OK)
    return fail(&c, "no model.glb on the card");
  len = (u32)f_size(&f);
  if (len > GLB_FILE_MAX) {
    f_close(&f);
    return fail(&c, "model.glb is over 11 MB");
  }
  for (u32 at = 0; at < len; at += br) {
    u32 part = len - at > 0x40000u ? 0x40000u : len - at;
    if (cb)
      cb("Reading", at, len);
    if (f_read(&f, file + at, part, &br) != FR_OK || !br) {
      f_close(&f);
      return fail(&c, "model.glb could not be read");
    }
  }
  f_close(&f);

  if (len < 28 || rd32(file) != 0x46546C67u || rd32(file + 4) != 2u)
    return fail(&c, "not a glTF 2.0 binary (.glb)");
  jlen = rd32(file + 12);
  if (rd32(file + 16) != 0x4E4F534Au || jlen > len - 20)
    return fail(&c, "the glb has no JSON chunk");
  c.bin = 0;
  c.binlen = 0;
  if (20 + jlen + 8 <= len && rd32(file + 20 + jlen + 4) == 0x004E4942u) {
    c.binlen = rd32(file + 20 + jlen);
    c.bin = file + 20 + jlen + 8;
    if (c.binlen > len - (20 + jlen + 8))
      return fail(&c, "the glb's binary chunk is cut short");
  }

  if (cb)
    cb("Parsing", 0, 1);
  if (!json_parse(&c.js, (const char *)file + 20, jlen,
                  (JsonTok *)GLB_TOK_ADDR, GLB_TOK_SIZE / sizeof(JsonTok)))
    return fail(&c, "the glb's JSON could not be read");
  c.root = 0;
  {
    static const char *const handled[] = {
        "KHR_mesh_quantization", "KHR_texture_transform",
        "KHR_materials_unlit"};
    int req = json_get(&c.js, c.root, "extensionsRequired");
    for (u32 r = 0; r < json_count(&c.js, req); r++) {
      int e = json_at(&c.js, req, r), known = 0;
      char ext[40];
      for (u32 k = 0; k < sizeof(handled) / sizeof(handled[0]); k++)
        known |= json_is(&c.js, e, handled[k]);
      if (known)
        continue;
      json_str(&c.js, e, ext, sizeof(ext));
      copy_str(err, "needs extension ", errmax);
      for (u32 i = 0, o = 16; ext[i] && o + 1 < errmax; i++, o++) {
        err[o] = ext[i];
        err[o + 1] = 0;
      }
      return 0;
    }
  }
  {
    int t = json_get(&c.js, json_get(&c.js, json_get(&c.js, c.root, "asset"),
                                     "extras"),
                     "title");
    if (t >= 0)
      json_str(&c.js, t, m->title, sizeof(m->title));
    else
      copy_str(m->title, "model.glb", sizeof(m->title));
  }

  mat_identity(ident);
  scenes = json_get(&c.js, c.root, "scenes");
  scene = json_at(&c.js, scenes,
                  (u32)json_int(&c.js, json_get(&c.js, c.root, "scene"), 0));
  if (scene >= 0) {
    int roots = json_get(&c.js, scene, "nodes");
    for (u32 i = 0; i < json_count(&c.js, roots); i++)
      if (!walk(&c, json_int(&c.js, json_at(&c.js, roots, i), -1), ident, 0))
        return 0;
  } else {
    /* No scene: every node is drawn in place. */
    for (u32 i = 0; i < json_count(&c.js, json_get(&c.js, c.root, "nodes"));
         i++)
      if (!walk(&c, (int)i, ident, MAX_DEPTH))
        return 0;
  }

  if (!count(&c, &nv, &ni))
    return 0;
  if (!nv)
    return fail(&c, "the model has no triangles");
  vb = arena_alloc(&c, nv * sizeof(GlbVertex), 16);
  if (!vb)
    return fail(&c, "the model does not fit in memory");
  m->base = (u32)vb;

  for (u32 k = 0; k < ninst; k++) {
    int ps = json_get(&c.js, json_at(&c.js, json_get(&c.js, c.root, "meshes"),
                                     (u32)inst[k].mesh),
                      "primitives");
    if (cb)
      cb("Meshes", k, ninst);
    for (u32 i = 0; i < json_count(&c.js, ps); i++) {
      int p = json_at(&c.js, ps, i), mode = prim_mode(&c, p);
      if (mode != 4 && mode != 5 && mode != 6) {
        m->skipped++;
        continue;
      }
      if (m->nprim >= GLB_MAX_PRIMS)
        return fail(&c, "too many meshes");
      if (!build_prim(&c, k, p, &prims[m->nprim], vb, &vnext, bmin, bmax))
        return 0;
      m->ntris += prims[m->nprim].icount / 3u;
      m->nprim++;
    }
  }
  m->nverts = vnext;

  fixed = arena_alloc(&c, nv * 12u, 16);
  if (!fixed)
    return fail(&c, "the model does not fit in memory");
  normalise_model(m, vb, bmin, bmax, fixed);
  m->fixed = fixed;

  if (!build_materials(&c, m))
    return 0;
  if (cb)
    cb("Done", 1, 1);
  return 1;
}

u32 glb_prim_of(const GlbModel *m, u32 v) {
  u32 lo = 0, hi = m->nprim;
  while (hi - lo > 1u) {
    u32 mid = (lo + hi) / 2u;
    if (m->prim[mid].vfirst <= v)
      lo = mid;
    else
      hi = mid;
  }
  return lo;
}

void glb_cube(GlbModel *m) {
  static const float corner[8][3] = {
      {-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1},
      {-1, -1, 1},  {1, -1, 1},  {1, 1, 1},  {-1, 1, 1},
  };
  static const struct {
    u8 v[4];
    float n[3], c[3];
    const char *name;
  } face[6] = {
      {{4, 5, 6, 7}, {0, 0, 1}, {0.30f, 0.55f, 0.96f}, "Front"},
      {{1, 0, 3, 2}, {0, 0, -1}, {0.32f, 0.77f, 0.48f}, "Back"},
      {{5, 1, 2, 6}, {1, 0, 0}, {0.91f, 0.36f, 0.36f}, "Right"},
      {{0, 4, 7, 3}, {-1, 0, 0}, {0.95f, 0.62f, 0.30f}, "Left"},
      {{7, 6, 2, 3}, {0, 1, 0}, {0.94f, 0.94f, 0.94f}, "Top"},
      {{0, 1, 5, 4}, {0, -1, 0}, {0.96f, 0.82f, 0.30f}, "Bottom"},
  };
  static const u8 tri[6] = {0, 1, 2, 0, 2, 3};
  const float s = GLB_RADIUS / 1.7320508f; /* corners on the radius */
  GlbVertex *vb = (GlbVertex *)GLB_ARENA_ADDR;
  /* Each face's six indices on a 16-byte boundary, the picking copy after. */
  u16 *ib = (u16 *)(GLB_ARENA_ADDR + 24u * sizeof(GlbVertex));
  s32 *fixed = (s32 *)(ib + 6 * 8);

  m->base = (u32)vb;
  m->prim = prims;
  m->mat = mats;
  m->tex = texs;
  m->nprim = 6;
  m->nmat = 0;
  m->ntex = 0;
  m->nverts = 24;
  m->ntris = 12;
  m->skipped = m->tex_failed = 0;
  m->fixed = fixed;
  copy_str(m->title, "Built-in cube", sizeof(m->title));
  for (u32 f = 0; f < 6; f++) {
    float l = light(face[f].n);
    for (u32 k = 0; k < 4; k++) {
      GlbVertex *v = &vb[f * 4u + k];
      for (int a = 0; a < 3; a++) {
        v->pos[a] = corner[face[f].v[k]][a] * s;
        v->col[a] = face[f].c[a] * l;
        fixed[(f * 4u + k) * 3u + a] = (s32)(v->pos[a] * 4096.0f);
      }
      v->pos[3] = 1.0f;
      v->col[3] = 1.0f;
      v->uv[0] = v->uv[1] = 0.0f;
    }
    for (u32 k = 0; k < 6; k++)
      ib[f * 8u + k] = tri[k];
    prims[f].vfirst = f * 4u;
    prims[f].vcount = 4;
    prims[f].index = (u32)&ib[f * 8u];
    prims[f].icount = 6;
    prims[f].mat = -1;
    for (int a = 0; a < 3; a++)
      prims[f].centre[a] = face[f].n[a] * s * 0.577f;
    copy_str(prims[f].name, face[f].name, GLB_NAME);
  }
}
