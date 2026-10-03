#ifndef AURORA_SOFT3D_H
#define AURORA_SOFT3D_H

#include "aurora.h"

/* A software renderer on the ARM9: flat-shaded convex faces, culled and drawn
 * back to front, so a mesh needs no depth buffer as long as its faces do not
 * cut through each other. No FPU on the ARM9, so everything is fixed point:
 * positions in S3D_ONE units, unit vectors and sines in 1/16384, angles in
 * 1/65536 of a turn. */

#define S3D_ONE 4096
#define S3D_TURN 65536u

typedef struct {
  s32 x, y, z;
} S3dVec;

/* Up to four vertices, counter-clockwise seen from outside, in one plane. */
typedef struct {
  u8 count;
  u8 v[4];
  Color color;
} S3dFace;

typedef struct {
  const S3dVec *verts;
  int vert_count;
  const S3dFace *faces;
  int face_count;
} S3dMesh;

/* Orbits the origin `dist` away, looking at it. Yaw 0 looks down -z; a
 * positive pitch raises the camera. `focal` is in pixels. For one eye of a
 * stereo pair, `eye` moves the view sideways (negative for the left eye) and
 * `conv` is the distance at which both eyes' pictures coincide; 0 for 2D. */
typedef struct {
  u32 yaw;
  s32 pitch;
  s32 dist;
  s32 focal;
  s32 eye;
  s32 conv;
} S3dCamera;

#define S3D_MAX_VERTS 64
#define S3D_MAX_FACES 64

s32 s3d_sin(u32 angle);
s32 s3d_cos(u32 angle);

/* Draws into a column-major BGR framebuffer w x h, over what is there; w is
 * at most TOP_SCREEN_WIDTH. `light` points towards the light, in world space,
 * any length. Returns the number of faces drawn. */
int s3d_draw(volatile u8 *fb, int w, int h, const S3dMesh *mesh,
             const S3dCamera *cam, const S3dVec *light);

/* Face `face` lit as s3d_draw() lights it, for drawing the mesh elsewhere. */
Color s3d_face_colour(const S3dMesh *mesh, int face, const S3dVec *light);

#endif
