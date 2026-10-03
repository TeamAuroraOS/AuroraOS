/* PICA200 command lists, built on the ARM9 and run by the ARM11 (P3d11.c).
 *
 * Register numbers and encodings: GBATEK "3DS GPU Internal Registers" and
 * libctru gpu/registers.h, enums.h, gpu.c (zlib). What a frame sets, and in
 * what order, follows citro3d (zlib); the same sequence runs on bare metal in
 * open_agb_firm. The ARM9 has no FPU, so the floats here are libgcc's. */
#include "p3d.h"
#include "gpu.h"

#define R_FINALIZE           0x010u
#define R_CULL               0x040u
#define R_VIEWPORT_WIDTH     0x041u
#define R_DEPTHMAP_SCALE     0x04Du
#define R_SH_OUTMAP_TOTAL    0x04Fu
#define R_EARLYDEPTH_FUNC    0x061u
#define R_EARLYDEPTH_TEST1   0x062u
#define R_EARLYDEPTH_CLEAR   0x063u
#define R_SH_OUTATTR_MODE    0x064u
#define R_SCISSOR_MODE       0x065u
#define R_VIEWPORT_XY        0x068u
#define R_EARLYDEPTH_DATA    0x06Au
#define R_DEPTHMAP_ENABLE    0x06Du
#define R_RENDERBUF_DIM      0x06Eu
#define R_SH_OUTATTR_CLOCK   0x06Fu
#define R_TEXUNIT_CONFIG     0x080u
#define R_TEXUNIT0_BORDER    0x081u
#define R_TEXUNIT0_TYPE      0x08Eu
#define R_TEXUNIT0_SHADOW    0x08Bu
#define R_LIGHTING_ENABLE0   0x08Fu
#define R_TEXENV0            0x0C0u
#define R_TEXENV_UPDATE      0x0E0u
#define R_FOG_COLOR          0x0E1u
#define R_TEXENV_BUF_COLOR   0x0FDu
#define R_COLOR_OPERATION    0x100u
#define R_BLEND_FUNC         0x101u
#define R_LOGIC_OP           0x102u
#define R_BLEND_COLOR        0x103u
#define R_ALPHA_TEST         0x104u
#define R_DEPTH_COLOR_MASK   0x107u
#define R_FB_INVALIDATE      0x110u
#define R_FB_FLUSH           0x111u
#define R_COLORBUFFER_READ   0x112u
#define R_DEPTHBUFFER_FORMAT 0x116u
#define R_COLORBUFFER_FORMAT 0x117u
#define R_EARLYDEPTH_TEST2   0x118u
#define R_FB_BLOCK32         0x11Bu
#define R_DEPTHBUFFER_LOC    0x11Cu
#define R_GAS_DELTAZ_DEPTH   0x126u
#define R_FRAGOP_SHADOW      0x130u
#define R_LIGHTING_ENABLE1   0x1C6u
#define R_ATTRIBBUFFERS_LOC  0x200u
#define R_ATTR_FORMAT_LOW    0x201u
#define R_ATTRIBBUFFER0      0x203u
#define R_INDEXBUFFER_CONFIG 0x227u
#define R_NUMVERTICES        0x228u
#define R_GEOSTAGE_CONFIG    0x229u
#define R_VERTEX_OFFSET      0x22Au
#define R_DRAWELEMENTS       0x22Fu
#define R_VTX_FUNC           0x231u
#define R_FIXEDATTRIB_INDEX  0x232u
#define R_FIXEDATTRIB_DATA0  0x233u
#define R_VSH_NUM_ATTR       0x242u
#define R_VSH_COM_MODE       0x244u
#define R_START_DRAW_FUNC0   0x245u
#define R_VSH_OUTMAP_TOTAL1  0x24Au
#define R_VSH_OUTMAP_TOTAL2  0x251u
#define R_GSH_MISC0          0x252u
#define R_GEOSTAGE_CONFIG2   0x253u
#define R_GSH_MISC1          0x254u
#define R_PRIMITIVE_CONFIG   0x25Eu
#define R_RESTART_PRIMITIVE  0x25Fu
#define R_GSH_INPUT_CONFIG   0x289u
#define R_VSH_BOOLUNIFORM    0x2B0u
#define R_VSH_INPUT_CONFIG   0x2B9u
#define R_VSH_ENTRYPOINT     0x2BAu
#define R_VSH_PERMUTATION    0x2BBu
#define R_VSH_OUTMAP_MASK    0x2BDu
#define R_VSH_CODE_END       0x2BFu
#define R_VSH_FLOATUNIF_CFG  0x2C0u
#define R_VSH_FLOATUNIF_DATA 0x2C1u
#define R_VSH_CODE_CONFIG    0x2CBu
#define R_VSH_CODE_DATA      0x2CCu
#define R_VSH_OPDESC_CONFIG  0x2D5u
#define R_VSH_OPDESC_DATA    0x2D6u

/* The value the ARM11 sets as GPUREG_IRQ_CMP (P3d11.c). */
#define FINALIZE_MAGIC 0x12345678u

/* Command header: register, byte mask, parameter count - 1, and bit 31 for
 * consecutive registers rather than one register written repeatedly. */
static void put(P3dList *l, u32 reg, u32 mask, int seq, const u32 *v, u32 n) {
  u32 need = n + 1 + ((n + 1) & 1);
  if (l->full || l->n + need > l->cap) {
    l->full = 1;
    return;
  }
  l->buf[l->n++] = v[0];
  l->buf[l->n++] = (reg & 0x3FFu) | ((mask & 0xFu) << 16) |
                   ((n - 1u) << 20) | (seq ? 0x80000000u : 0);
  for (u32 i = 1; i < n; i++)
    l->buf[l->n++] = v[i];
  if (!(n & 1u))
    l->buf[l->n++] = 0; /* each command is a multiple of 8 bytes */
}

void p3d_write(P3dList *l, u32 reg, u32 mask, u32 value) {
  put(l, reg, mask, 0, &value, 1);
}

void p3d_write_seq(P3dList *l, u32 reg, const u32 *v, u32 n) {
  while (n) {
    u32 k = n > 256u ? 256u : n;
    put(l, reg, 0xF, 1, v, k);
    reg += k;
    v += k;
    n -= k;
  }
}

void p3d_write_port(P3dList *l, u32 reg, const u32 *v, u32 n) {
  while (n) {
    u32 k = n > 256u ? 256u : n;
    put(l, reg, 0xF, 0, v, k);
    v += k;
    n -= k;
  }
}

void p3d_begin(P3dList *l, u32 *buf, u32 cap_words) {
  l->buf = buf;
  l->n = 0;
  l->cap = cap_words;
  l->full = 0;
}

u32 p3d_end(P3dList *l) {
  p3d_write(l, R_FINALIZE, 0xF, FINALIZE_MAGIC);
  if (l->n & 3u)
    p3d_write(l, R_FINALIZE, 0xF, FINALIZE_MAGIC);
  return l->full ? 0 : l->n * 4u;
}

static u32 bits(float f) {
  union {
    float f;
    u32 u;
  } x;
  x.f = f;
  return x.u;
}

u32 p3d_f32(float f) { return bits(f); }

/* PICA floats: 1 sign, 7 exponent (bias 63), and a 16- or 23-bit mantissa. */
u32 p3d_f24(float f) {
  u32 i = bits(f), sign = i >> 31;
  s32 e = (s32)((i >> 23) & 0xFFu) - 127 + 63;
  if (e < 0)
    return sign << 23;
  if (e > 0x7F)
    return (sign << 23) | (0x7Fu << 16);
  return (sign << 23) | ((u32)e << 16) | ((i & 0x7FFFFFu) >> 7);
}

u32 p3d_f31(float f) {
  u32 i = bits(f), sign = i >> 31;
  s32 e = (s32)((i >> 23) & 0xFFu) - 127 + 63;
  if (e < 0)
    return sign << 30;
  if (e > 0x7F)
    return (sign << 30) | (0x7Fu << 23);
  return (sign << 30) | ((u32)e << 23) | (i & 0x7FFFFFu);
}

void p3d_target(P3dList *l) {
  const u32 dim = 0x01000000u | ((GPU_P3D_H - 1u) << 12) | GPU_P3D_W;
  const u32 loc[3] = {GPU_P3D_DEPTH >> 3, GPU_P3D_COLOR >> 3, dim};
  const u32 masks[4] = {0xF, 0xF, 0x3, 0x3}; /* colour and depth+stencil */
  const u32 view[4] = {p3d_f24(GPU_P3D_W / 2.0f),
                       p3d_f31(2.0f / GPU_P3D_W) << 1,
                       p3d_f24(GPU_P3D_H / 2.0f),
                       p3d_f31(2.0f / GPU_P3D_H) << 1};
  const u32 scissor[3] = {0, 0, 0};

  p3d_write(l, R_FB_INVALIDATE, 0xF, 1);
  p3d_write_seq(l, R_DEPTHBUFFER_LOC, loc, 3);
  p3d_write(l, R_RENDERBUF_DIM, 0xF, dim);
  p3d_write(l, R_DEPTHBUFFER_FORMAT, 0xF, 3); /* 24-bit depth, 8-bit stencil */
  p3d_write(l, R_COLORBUFFER_FORMAT, 0xF, 2); /* RGBA8 */
  p3d_write(l, R_FB_BLOCK32, 0xF, 0);
  p3d_write_seq(l, R_COLORBUFFER_READ, masks, 4);
  p3d_write_seq(l, R_VIEWPORT_WIDTH, view, 4);
  p3d_write(l, R_VIEWPORT_XY, 0xF, 0);
  p3d_write_seq(l, R_SCISSOR_MODE, scissor, 3);
}

void p3d_depth(P3dList *l, int on) {
  /* Enable, test function (GREATER 6 or ALWAYS 1) and write mask (colour
   * 0xF, plus depth 0x10), then the stencil test and op, both unused. */
  const u32 frag[4] = {0x10, 0x10, 0,
                       on ? (1u | (6u << 4) | (0x1Fu << 8))
                          : ((1u << 4) | (0xFu << 8))};
  const u32 map[2] = {p3d_f24(-1.0f), p3d_f24(0.0f)};

  p3d_write(l, R_DEPTHMAP_ENABLE, 0xF, 1);
  p3d_write(l, R_CULL, 0xF, 0);
  p3d_write_seq(l, R_DEPTHMAP_SCALE, map, 2);
  p3d_write_seq(l, R_ALPHA_TEST, frag, 4);
  p3d_write(l, R_GAS_DELTAZ_DEPTH, 0x8, on ? 0x02000000u : 0);
  p3d_write(l, R_BLEND_COLOR, 0xF, 0);
  p3d_write(l, R_BLEND_FUNC, 0xF, 0x76760000u); /* source alpha over */
  p3d_write(l, R_LOGIC_OP, 0xF, 0);
  p3d_write(l, R_COLOR_OPERATION, 0x7, 0x00E40100u);
  p3d_write(l, R_FRAGOP_SHADOW, 0xF, 0x80003C00u);
  p3d_write(l, R_EARLYDEPTH_TEST1, 0x1, 0);
  p3d_write(l, R_EARLYDEPTH_TEST2, 0xF, 0);
  p3d_write(l, R_EARLYDEPTH_FUNC, 0x1, 0);
  p3d_write(l, R_EARLYDEPTH_DATA, 0x7, 0);
}

void p3d_vertex_colour(P3dList *l) {
  p3d_write(l, R_TEXUNIT_CONFIG, 0xB, 0x00001000u); /* every unit off */
  p3d_write(l, R_TEXUNIT_CONFIG, 0x4, 0x00010000u); /* and the cache cleared */
  p3d_write(l, R_TEXUNIT0_SHADOW, 0xF, 1);
  p3d_write(l, R_LIGHTING_ENABLE0, 0xF, 0);
  p3d_write(l, R_LIGHTING_ENABLE1, 0xF, 1);
  p3d_write(l, R_TEXENV_UPDATE, 0x7, 0);
  p3d_write(l, R_TEXENV_BUF_COLOR, 0xF, 0xFFFFFFFFu);
  p3d_write(l, R_FOG_COLOR, 0xF, 0);
  for (u32 i = 0; i < 6; i++) {
    /* Stage 0 replaces with the vertex colour; the rest pass it on. Sources
     * are nibbles, 0 primary colour, 0xF previous stage. */
    const u32 src = i ? 0x000F000Fu : 0;
    const u32 env[5] = {src, 0, 0, 0xFFFFFFFFu, 0};
    p3d_write_seq(l, R_TEXENV0 + (i < 4 ? i : i + 2) * 8u, env, 5);
  }
}

void p3d_shader(P3dList *l, const P3dShader *s) {
  const u32 none = 0x1F1F1F1Fu;
  u32 outmap[8];

  p3d_write(l, R_GEOSTAGE_CONFIG, 0x3, 0);
  p3d_write(l, R_GEOSTAGE_CONFIG2, 0x3, 0);
  p3d_write(l, R_VSH_COM_MODE, 0x1, 0);
  p3d_write(l, R_VSH_CODE_CONFIG, 0xF, 0);
  p3d_write_port(l, R_VSH_CODE_DATA, s->code, s->code_n);
  p3d_write(l, R_VSH_CODE_END, 0xF, 1);
  p3d_write(l, R_VSH_OPDESC_CONFIG, 0xF, 0);
  p3d_write_port(l, R_VSH_OPDESC_DATA, s->opdesc, s->opdesc_n);
  p3d_write(l, R_VSH_ENTRYPOINT, 0xF, 0x7FFF0000u | (s->main & 0xFFFFu));
  p3d_write(l, R_VSH_OUTMAP_MASK, 0xF, s->out_mask);
  p3d_write(l, R_VSH_OUTMAP_TOTAL1, 0xF, s->out_total - 1u);
  p3d_write(l, R_VSH_OUTMAP_TOTAL2, 0xF, s->out_total - 1u);
  p3d_write(l, R_PRIMITIVE_CONFIG, 0x1, s->out_total - 1u);
  outmap[0] = s->out_total;
  for (int i = 0; i < 7; i++)
    outmap[i + 1] = s->outmap[i] ? s->outmap[i] : none;
  p3d_write_seq(l, R_SH_OUTMAP_TOTAL, outmap, 8);
  p3d_write(l, R_SH_OUTATTR_MODE, 0xF, s->out_mode);
  p3d_write(l, R_SH_OUTATTR_CLOCK, 0xF, s->out_clock);
  p3d_write(l, R_GEOSTAGE_CONFIG, 0xA, 0);
  p3d_write(l, R_GSH_MISC0, 0xF, 0);
  p3d_write(l, R_GSH_MISC1, 0xF, 0);
  p3d_write(l, R_GSH_INPUT_CONFIG, 0xF, 0xA0000000u);
  p3d_write(l, R_VSH_BOOLUNIFORM, 0xF, 0x7FFF0000u);
  for (u32 i = 0; i < s->consts_n; i++)
    p3d_write_seq(l, R_VSH_FLOATUNIF_CFG, s->consts + i * 4u, 4);
}

void p3d_inputs2(P3dList *l) {
  /* Two attributes, each four floats (format 0xF); attributes 2..11 fixed. */
  const u32 fmt[2] = {0xFFu, (1u << 28) | (0xFFCu << 16)};
  const u32 perm[2] = {0x10, 0}; /* attribute 0 -> v0, 1 -> v1 */

  p3d_write_seq(l, R_ATTR_FORMAT_LOW, fmt, 2);
  p3d_write(l, R_VSH_INPUT_CONFIG, 0xB, 0xA0000001u);
  p3d_write(l, R_VSH_NUM_ATTR, 0xF, 1);
  p3d_write_seq(l, R_VSH_PERMUTATION, perm, 2);
}

/* FORMAT_HIGH: the input count - 1 in bits 28-31, and bits 16-27 set for the
 * inputs not used, which read as fixed values. */
void p3d_inputs(P3dList *l, const u8 *formats, u32 n) {
  u32 fmt[2] = {0, ((n - 1u) << 28) | ((0xFFFu << 16) & ~(((1u << n) - 1u) << 16))};
  u32 perm[2] = {0, 0};
  for (u32 i = 0; i < n; i++) {
    fmt[i / 8u] |= (u32)formats[i] << ((i % 8u) * 4u);
    perm[i / 8u] |= i << ((i % 8u) * 4u);
  }
  p3d_write_seq(l, R_ATTR_FORMAT_LOW, fmt, 2);
  p3d_write(l, R_VSH_INPUT_CONFIG, 0xB, 0xA0000000u | (n - 1u));
  p3d_write(l, R_VSH_NUM_ATTR, 0xF, n - 1u);
  p3d_write_seq(l, R_VSH_PERMUTATION, perm, 2);
}

void p3d_buffers(P3dList *l, u32 base) {
  static const u32 none[36];
  p3d_write(l, R_ATTRIBBUFFERS_LOC, 0xF, base >> 3);
  p3d_write_seq(l, R_ATTRIBBUFFER0, none, 36);
}

void p3d_buffer0(P3dList *l, u32 offset, u32 stride, u32 nattr) {
  /* Attributes 0..nattr-1 in order, interleaved. */
  u32 perm = 0;
  for (u32 i = 0; i < nattr; i++)
    perm |= i << (i * 4u);
  const u32 cfg[3] = {offset, perm, (stride << 16) | (nattr << 28)};
  p3d_write_seq(l, R_ATTRIBBUFFER0, cfg, 3);
}

void p3d_draw_elements(P3dList *l, u32 index_offset, u32 count) {
  p3d_write(l, R_PRIMITIVE_CONFIG, 0x2, 0x300); /* triangles from indices */
  p3d_write(l, R_RESTART_PRIMITIVE, 0xF, 1);
  p3d_write(l, R_INDEXBUFFER_CONFIG, 0xF, index_offset | 0x80000000u);
  p3d_write(l, R_NUMVERTICES, 0xF, count);
  p3d_write(l, R_VERTEX_OFFSET, 0xF, 0);
  p3d_write(l, R_GEOSTAGE_CONFIG, 0x2, 0x100);
  p3d_write(l, R_GEOSTAGE_CONFIG2, 0x2, 0x100);
  p3d_write(l, R_START_DRAW_FUNC0, 0x1, 0);
  p3d_write(l, R_DRAWELEMENTS, 0xF, 1);
  p3d_write(l, R_START_DRAW_FUNC0, 0x1, 1);
  p3d_write(l, R_GEOSTAGE_CONFIG, 0x2, 0);
  p3d_write(l, R_GEOSTAGE_CONFIG2, 0x2, 0);
  p3d_write(l, R_VTX_FUNC, 0xF, 1);
  p3d_write(l, R_PRIMITIVE_CONFIG, 0x8, 0);
  p3d_write(l, R_PRIMITIVE_CONFIG, 0x8, 0);
}

void p3d_texture(P3dList *l, u32 addr, u32 w, u32 h, u32 levels, u32 param) {
  const u32 unit[5] = {0, (w << 16) | h, param, levels << 16, addr >> 3};
  p3d_write_seq(l, R_TEXUNIT0_BORDER, unit, 5);
  p3d_write(l, R_TEXUNIT0_TYPE, 0xF, 0); /* RGBA8 */
  p3d_write(l, R_TEXUNIT_CONFIG, 0xB, 0x00001001u); /* unit 0 on */
  p3d_write(l, R_TEXUNIT_CONFIG, 0x4, 0x00010000u); /* drop cached texels */
}

void p3d_texture_off(P3dList *l) {
  p3d_write(l, R_TEXUNIT_CONFIG, 0xB, 0x00001000u);
  p3d_write(l, R_TEXUNIT_CONFIG, 0x4, 0x00010000u);
}

void p3d_stage0(P3dList *l, int textured, int alpha) {
  /* Sources are nibbles: 0 the vertex colour, 3 texture unit 0, 0xE the
   * constant (white). Combiner 0 replaces with the first source, 1
   * multiplies the first two. */
  static const u32 a_src[3] = {0x00Eu, 0x000u, 0x003u};
  u32 rgb = textured ? 0x003u : 0x000u;
  const u32 env[5] = {rgb | (a_src[alpha] << 16), 0,
                      (textured ? 1u : 0u) |
                          ((alpha == P3D_ALPHA_TEXTURE ? 1u : 0u) << 16),
                      0xFFFFFFFFu, 0};
  p3d_write_seq(l, R_TEXENV0, env, 5);
}

void p3d_fragment(P3dList *l, int depth_test, int depth_write, int alpha_ref) {
  /* Alpha test (GEQUAL against alpha_ref, or off), then depth test GREATER
   * with the colour always written and depth only when asked. */
  const u32 frag[4] = {
      alpha_ref >= 0 ? (1u | (7u << 4) | ((u32)alpha_ref << 8)) : 0x10u, 0x10u,
      0, (depth_test ? (1u | (6u << 4)) : (1u << 4)) | (0xFu << 8) |
             (depth_write ? 0x1000u : 0)};
  p3d_write_seq(l, R_ALPHA_TEST, frag, 4);
  p3d_write(l, R_GAS_DELTAZ_DEPTH, 0x8, depth_test ? 0x02000000u : 0);
}

/* Each row goes as four 32-bit floats, w first. */
void p3d_uniform(P3dList *l, u32 reg, const P3dMtx *m) {
  u32 v[16];
  for (int r = 0; r < 4; r++)
    for (int c = 0; c < 4; c++)
      v[r * 4 + c] = p3d_f32(m->m[r][3 - c]);
  p3d_write(l, R_VSH_FLOATUNIF_CFG, 0xF, 0x80000000u | reg);
  p3d_write_port(l, R_VSH_FLOATUNIF_DATA, v, 16);
}

void p3d_draw_begin(P3dList *l, u32 primitive) {
  p3d_write(l, R_PRIMITIVE_CONFIG, 0x2, primitive);
  p3d_write(l, R_RESTART_PRIMITIVE, 0xF, 1);
  p3d_write(l, R_INDEXBUFFER_CONFIG, 0xF, 0x80000000u);
  p3d_write(l, R_GEOSTAGE_CONFIG2, 0x1, 1);
  p3d_write(l, R_START_DRAW_FUNC0, 0x1, 0);
  p3d_write(l, R_FIXEDATTRIB_INDEX, 0xF, 0xF); /* immediate-mode vertices */
}

/* Four float24s in three words, as citro3d's C3D_ImmSendAttrib packs them. */
static void attr(P3dList *l, const float v[4]) {
  u32 x = p3d_f24(v[0]), y = p3d_f24(v[1]), z = p3d_f24(v[2]),
      w = p3d_f24(v[3]);
  const u32 p[3] = {(z >> 16) | (w << 8), (y >> 8) | (z << 16),
                    x | (y << 24)};
  p3d_write_seq(l, R_FIXEDATTRIB_DATA0, p, 3);
}

void p3d_vertex(P3dList *l, const float pos[4], const float col[4]) {
  attr(l, pos);
  attr(l, col);
}

void p3d_vertex3(P3dList *l, const float a[4], const float b[4],
                 const float c[4]) {
  attr(l, a);
  attr(l, b);
  attr(l, c);
}

void p3d_draw_end(P3dList *l) {
  p3d_write(l, R_START_DRAW_FUNC0, 0x1, 1);
  p3d_write(l, R_GEOSTAGE_CONFIG2, 0x1, 0);
  p3d_write(l, R_VTX_FUNC, 0xF, 1);
}

void p3d_flush(P3dList *l) {
  p3d_write(l, R_FB_FLUSH, 0xF, 1);
  p3d_write(l, R_FB_INVALIDATE, 0xF, 1);
  p3d_write(l, R_EARLYDEPTH_CLEAR, 0xF, 1);
}

void p3d_mtx_mul(P3dMtx *out, const P3dMtx *a, const P3dMtx *b) {
  P3dMtx t;
  for (int r = 0; r < 4; r++)
    for (int c = 0; c < 4; c++)
      t.m[r][c] = a->m[r][0] * b->m[0][c] + a->m[r][1] * b->m[1][c] +
                  a->m[r][2] * b->m[2][c] + a->m[r][3] * b->m[3][c];
  *out = t;
}

static void zero(P3dMtx *m) {
  for (int r = 0; r < 4; r++)
    for (int c = 0; c < 4; c++)
      m->m[r][c] = 0.0f;
}

/* Columns 0..3 are x, y, z, w. The eye's offset moves the view sideways
 * (x - eye) and the picture back by eye / screen, so a point `screen` away on
 * the axis lands in the same place for both eyes. */
void p3d_persp_stereo_tilt(P3dMtx *m, float tan_half, float aspect,
                           float near, float far, float eye, float screen) {
  float ta = tan_half * aspect;
  zero(m);
  m->m[0][1] = 1.0f / tan_half;
  m->m[1][0] = -1.0f / ta;
  m->m[1][2] = eye / (ta * screen);
  m->m[1][3] = eye / ta;
  m->m[2][2] = near / (near - far);
  m->m[2][3] = near * far / (near - far);
  m->m[3][2] = -1.0f;
}

void p3d_ortho_tilt(P3dMtx *m, float left, float right, float bottom,
                    float top, float near, float far) {
  zero(m);
  m->m[0][1] = 2.0f / (top - bottom);
  m->m[0][3] = (bottom + top) / (bottom - top);
  m->m[1][0] = 2.0f / (left - right);
  m->m[1][3] = (left + right) / (right - left);
  m->m[2][2] = 1.0f / (far - near);
  m->m[2][3] = 0.5f * (near + far) / (near - far) - 0.5f;
  m->m[3][3] = 1.0f;
}
