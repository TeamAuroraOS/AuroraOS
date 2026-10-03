#ifndef AURORA_GLB_H
#define AURORA_GLB_H

#include "aurora.h"

/* glTF 2.0 binary (.glb) models, converted into data the PICA200 draws
 * directly: one vertex buffer, 16-bit index lists and tiled RGBA8 textures,
 * all in the GPU arena. See docs/glb.md. */

/* FCRAM nothing else uses while the OS's own screens run: the app-launch
 * staging area (0x24000000), and the top of FCRAM above the ARM11 mailbox. */
#define GLB_ARENA_ADDR 0x24000000u
#define GLB_ARENA_SIZE 0x01000000u /* 16 MB, to 0x25000000 */
#define GLB_FILE_ADDR  0x27100000u
#define GLB_FILE_MAX   0x00B00000u /* 11 MB, to 0x27C00000 */
#define GLB_TOK_ADDR   0x27C00000u
#define GLB_TOK_SIZE   0x00100000u /* 1 MB of JSON tokens, to 0x27D00000 */

/* Models are centred on the origin and scaled to this radius. */
#define GLB_RADIUS 2.0f
/* Longest texture side kept; larger ones are scaled down. */
#define GLB_TEX_MAX 256

#define GLB_MAX_PRIMS 512
#define GLB_MAX_MATS  256
#define GLB_MAX_TEX   64
#define GLB_NAME      24

/* What the GPU reads per vertex: position (w = 1), texture coordinate (t
 * already flipped for the PICA), and the colour with the lighting baked in. */
typedef struct {
  float pos[4];
  float uv[2];
  float col[4];
} GlbVertex;

enum { GLB_OPAQUE = 0, GLB_MASK, GLB_BLEND };

typedef struct {
  u32 vfirst, vcount; /* vertices, in the vertex buffer */
  u32 index;          /* address of its 16-bit indices */
  u32 icount;
  int mat;            /* -1 for none */
  float centre[3];
  char name[GLB_NAME];
} GlbPrim;

typedef struct {
  int tex; /* -1 for none */
  u8 alpha, cutoff;
} GlbMat;

typedef struct {
  u32 addr; /* level 0, then each smaller level after it */
  u16 w, h;
  u8 levels; /* mipmap levels after the first */
  u8 wrap_s, wrap_t, nearest;
  u8 translucent; /* some texel is not fully opaque */
} GlbTex;

typedef struct {
  u32 base; /* the vertex buffer, also the GPU's attribute base */
  u32 nverts, ntris;
  GlbPrim *prim;
  u32 nprim;
  GlbMat *mat;
  u32 nmat;
  GlbTex *tex;
  u32 ntex;
  const s32 *fixed; /* x, y, z per vertex in 1/4096 units, for picking */
  u32 skipped;      /* primitives that are not triangles */
  u32 tex_failed;   /* images that could not be decoded */
  char title[48];
} GlbModel;

/* Stage, and progress through it, while a model loads. */
typedef void (*GlbProgress)(const char *stage, u32 done, u32 total);

/* Reads and converts `path` (the card must be mounted). 0 on failure, with a
 * reason in `err`. */
int glb_load(const char *path, GlbModel *m, GlbProgress cb, char *err,
             u32 errmax);

/* A coloured cube in the same form, for when there is no model. */
void glb_cube(GlbModel *m);

/* The primitive vertex `v` belongs to. */
u32 glb_prim_of(const GlbModel *m, u32 v);

#endif
