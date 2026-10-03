#ifndef AURORA_P3D_H
#define AURORA_P3D_H

#include "aurora.h"

/* Builds PICA200 command lists on the ARM9 for GPU_OP_P3D (gpu.h): every
 * draw call is a series of GPU register writes the GPU reads from memory.
 * Register numbers and encodings from GBATEK and libctru's registers.h; the
 * order of the setup follows citro3d. See docs/stereo3d.md. */

typedef struct {
  u32 *buf;
  u32 n, cap;
  int full; /* a write did not fit; the list must not be run */
} P3dList;

/* A vertex shader from tools/shbin2c.py. */
typedef struct {
  const u32 *code;
  u32 code_n;
  const u32 *opdesc;
  u32 opdesc_n;
  const u32 *consts; /* groups of four words: register, three float24 words */
  u32 consts_n;
  u32 main;
  u32 out_mask, out_total, out_mode, out_clock;
  u32 outmap[7];
} P3dShader;

/* Row-major, as the shader's dp4 rows read it. */
typedef struct {
  float m[4][4];
} P3dMtx;

/* `buf` must be 16-byte aligned and readable by the GPU (FCRAM or VRAM). */
void p3d_begin(P3dList *l, u32 *buf, u32 cap_words);

/* Ends the list with the finalize interrupt GPU_OP_P3D waits for, padded to
 * 16 bytes; returns its size in bytes, or 0 if it overflowed. */
u32 p3d_end(P3dList *l);

/* One register, under a byte mask (0xF for the whole word). */
void p3d_write(P3dList *l, u32 reg, u32 mask, u32 value);
/* Consecutive registers from `reg`. */
void p3d_write_seq(P3dList *l, u32 reg, const u32 *v, u32 n);
/* `n` values into the same register (a data port). */
void p3d_write_port(P3dList *l, u32 reg, const u32 *v, u32 n);

u32 p3d_f24(float f);
u32 p3d_f31(float f);
u32 p3d_f32(float f);

/* Setup for drawing into the GPU_P3D_COLOR / GPU_P3D_DEPTH render target. */
void p3d_target(P3dList *l);
/* Depth buffering on (nearer wins) or off; colour written either way. */
void p3d_depth(P3dList *l, int on);
/* Fixed-function stages: no lighting or textures, the vertex colour out. */
void p3d_vertex_colour(P3dList *l);
void p3d_shader(P3dList *l, const P3dShader *s);
/* Two float vec4 inputs per vertex, sent with p3d_vertex(). */
void p3d_inputs2(P3dList *l);
void p3d_uniform(P3dList *l, u32 reg, const P3dMtx *m);

/* Vertex inputs v0..v(n-1), each one of P3D_FMT_* per vertex. They come from
 * buffer 0 (p3d_buffers, p3d_buffer0) or from p3d_vertex_n(). */
enum {
  P3D_FMT_FLOAT2 = 0x7, /* (components - 1) << 2 | 3 for float */
  P3D_FMT_FLOAT3 = 0xB,
  P3D_FMT_FLOAT4 = 0xF,
};
void p3d_inputs(P3dList *l, const u8 *formats, u32 n);
/* The base every buffer and index offset is counted from, with every buffer
 * cleared; buffer 0 then holds `nattr` interleaved inputs `stride` bytes per
 * vertex, from `offset`. */
void p3d_buffers(P3dList *l, u32 base);
void p3d_buffer0(P3dList *l, u32 offset, u32 stride, u32 nattr);
/* Triangles from `count` 16-bit indices `index_offset` bytes past the base. */
void p3d_draw_elements(P3dList *l, u32 index_offset, u32 count);

/* Texture unit 0: tiled RGBA8 at `addr`, `levels` mipmaps after the first,
 * `param` P3D_TEX_* flags; or off. */
#define P3D_TEX_MAG_LINEAR (1u << 1)
#define P3D_TEX_MIN_LINEAR (1u << 2)
#define P3D_TEX_WRAP_T(m)  ((u32)(m) << 8)
#define P3D_TEX_WRAP_S(m)  ((u32)(m) << 12)
#define P3D_TEX_MIP_LINEAR (1u << 24)
void p3d_texture(P3dList *l, u32 addr, u32 w, u32 h, u32 levels, u32 param);
void p3d_texture_off(P3dList *l);
/* Combiner stage 0: the vertex colour, or texture x vertex colour; alpha one
 * of these. */
enum { P3D_ALPHA_ONE, P3D_ALPHA_VERTEX, P3D_ALPHA_TEXTURE };
void p3d_stage0(P3dList *l, int textured, int alpha);
/* Per-material state: depth test and write, and an alpha test (-1 for none). */
void p3d_fragment(P3dList *l, int depth_test, int depth_write, int alpha_ref);

enum { P3D_TRIANGLES = 0x000, P3D_TRIANGLE_STRIP = 0x100 };
void p3d_draw_begin(P3dList *l, u32 primitive);
void p3d_vertex(P3dList *l, const float pos[4], const float col[4]);
void p3d_vertex3(P3dList *l, const float a[4], const float b[4],
                 const float c[4]);
void p3d_draw_end(P3dList *l);

/* Writes the frame out of the GPU's tile cache; last before p3d_end(). */
void p3d_flush(P3dList *l);

void p3d_mtx_mul(P3dMtx *out, const P3dMtx *a, const P3dMtx *b);
/* A right-handed perspective for one eye, laid out as citro3d's
 * Mtx_PerspStereoTilt (the quarter turn the top screen needs, the PICA's
 * [-1, 0] depth range). `tan_half` is tan(vertical fov / 2), `aspect` width
 * over height, `eye` the eye's sideways offset in world units (negative for
 * the left), `screen` the distance at which both eyes' pictures coincide. */
void p3d_persp_stereo_tilt(P3dMtx *m, float tan_half, float aspect,
                           float near, float far, float eye, float screen);
/* citro3d's Mtx_OrthoTilt, left-handed as citro2d uses it: screen pixels. */
void p3d_ortho_tilt(P3dMtx *m, float left, float right, float bottom,
                    float top, float near, float far);

#endif
