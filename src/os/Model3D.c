#include "model3d.h"
#include "anim.h"
#include "ff.h"
#include "glb.h"
#include "gpu.h"
#include "p3d.h"
#include "soft3d.h"
#include "stereo.h"
#include "timer.h"
#include "touch.h"
#include "ui.h"
#include "model_shbin.h" /* generated from model.v.pica */

extern void os_dcache_flush(void);

#define MODEL_PATH "0:/Aurora/model.glb"

/* Composed once per visit and copied in by the GPU every frame, for the
 * software renderer. Clear of the animation frames, which end at 0x26D58400. */
#define CUBE_BG_ADDR 0x26D60000u /* 288,000 B -> 0x26DA6500 */
/* The right eye's frame, while the top screen is in 3D. */
#define RIGHT_ADDR   0x26DB0000u /* 288,000 B -> 0x26DF6500 */
/* The two eyes' command lists: above the GLB's JSON tokens (glb.h). */
#define LIST_ADDR    0x27D00000u
#define LIST_WORDS   0x10000u /* 256 KB each, to 0x27D80000 */

#define DEG(d)      ((d) * 65536 / 360)
#define FINE        8 /* extra angle bits, so slow turns still add up */
#define PITCH_MAX   DEG(85)
#define ORBIT_DEG_S 120 /* at full deflection */
/* Raw ADC units from the rest point. */
#define CPAD_DEAD   150
#define CPAD_FULL   1000
#define STATS_US    500000u

/* 240 pixels of focal length across the 400-pixel width: tan of half the
 * vertical field is 0.5. Models are scaled to GLB_RADIUS (2) units. */
#define CAM_FOCAL   240
#define TAN_HALF    0.5f
#define START_DIST  4.5f
#define DIST_MIN    0.3f
#define DIST_MAX    40.0f
#define ZOOM_RATE   1.2f /* fraction of the distance per second */
#define PAN_RATE    0.6f /* fraction of the distance per second */
#define PAN_MAX     (3.0f * GLB_RADIUS)
#define NEAR        0.05f
#define FAR         200.0f
/* The eyes' separation with the slider at the top, as a fraction of the
 * distance to the screen plane, so the depth stays comfortable at any zoom:
 * nothing beyond the plane can separate more than this x 240 pixels. */
#define EYE_FRAC    0.035f

enum { REND_GPU, REND_CPU };

typedef struct {
  u32 fps10, frame_us, draw_us;
  int measured;
  int renderer, gpu_failed;
  u32 p3d_state;
  int slider_raw, level, stereo, barrier_pos;
  int closest; /* primitive nearest the viewer, or -1 */
  char note[64];
} Stats;

typedef struct {
  u32 yaw;           /* with FINE extra bits */
  s32 pitch;
  float dist;
  float tgt[3];      /* the point orbited, panned with the D-pad */
} View;

static GlbModel model;
static u16 order[GLB_MAX_PRIMS];

static const Color bg_top = {0x2C, 0x2F, 0x3A}, bg_bot = {0x0C, 0x0D, 0x12};
#define GLOW_R 190
#define GRID_X 9
#define GRID_Y 5
static float grid_rgb[GRID_Y][GRID_X][4];

static char *put_str(char *p, const char *s) {
  while (*s)
    *p++ = *s++;
  return p;
}

static char *put_int(char *p, int v) {
  char t[12];
  int n = 0;
  u32 u = v < 0 ? (u32)-v : (u32)v;
  if (v < 0)
    *p++ = '-';
  if (!u)
    *p++ = '0';
  while (u) {
    t[n++] = (char)('0' + u % 10u);
    u /= 10u;
  }
  while (n--)
    *p++ = t[n];
  return p;
}

/* Tenths of a unit, printed with one decimal. */
static char *put_tenths(char *p, u32 t10) {
  p = put_int(p, (int)(t10 / 10u));
  *p++ = '.';
  *p++ = (char)('0' + t10 % 10u);
  return p;
}

/* The backdrop at pixel (x, y): a vertical gradient with a soft glow of the
 * accent colour in the middle. */
static Color backdrop_at(int x, int y) {
  const s32 inv_r2 = (256 << 16) / (GLOW_R * GLOW_R);
  int t = y * 255 / (TOP_SCREEN_HEIGHT - 1);
  int dx = x - TOP_SCREEN_WIDTH / 2;
  int dy = (y - TOP_SCREEN_HEIGHT / 2) * 3 / 2; /* wider than tall */
  s32 g = 256 - (((dx * dx + dy * dy) * inv_r2) >> 16);
  Color c;

  c.r = (u8)((bg_top.r * (255 - t) + bg_bot.r * t) / 255);
  c.g = (u8)((bg_top.g * (255 - t) + bg_bot.g * t) / 255);
  c.b = (u8)((bg_top.b * (255 - t) + bg_bot.b * t) / 255);
  if (g > 0) {
    g = (g * g) >> 11; /* up to 1/8 of the accent */
    c.r = (u8)(c.r + (((g_accent.r - c.r) * g) >> 8));
    c.g = (u8)(c.g + (((g_accent.g - c.g) * g) >> 8));
    c.b = (u8)(c.b + (((g_accent.b - c.b) * g) >> 8));
  }
  return c;
}

static void backdrop_build(void) {
  enum { W = TOP_SCREEN_WIDTH, H = TOP_SCREEN_HEIGHT };
  static u32 col[H * 3 / 4];
  u8 *c8 = (u8 *)col;
  u32 *dst = (u32 *)CUBE_BG_ADDR;

  for (int x = 0; x < W; x++) {
    for (int y = 0; y < H; y++) {
      Color c = backdrop_at(x, y);
      u8 *p = c8 + (H - 1 - y) * 3;
      p[0] = c.b;
      p[1] = c.g;
      p[2] = c.r;
    }
    for (int i = 0; i < H * 3 / 4; i++)
      *dst++ = col[i];
  }
  for (int j = 0; j < GRID_Y; j++)
    for (int i = 0; i < GRID_X; i++) {
      Color c = backdrop_at(i * (W - 1) / (GRID_X - 1),
                            j * (H - 1) / (GRID_Y - 1));
      grid_rgb[j][i][0] = c.r / 255.0f;
      grid_rgb[j][i][1] = c.g / 255.0f;
      grid_rgb[j][i][2] = c.b / 255.0f;
      grid_rgb[j][i][3] = 1.0f;
    }
}

/* The camera basis in Q14, as Soft3D.c builds it: rows right, up and back
 * towards the viewer. */
static void basis(const View *v, s32 r[3], s32 u[3], s32 b[3]) {
  u32 yaw = v->yaw >> FINE;
  s32 pitch = v->pitch >> FINE;
  s32 cy = s3d_cos(yaw), sy = s3d_sin(yaw);
  s32 cp = s3d_cos((u32)pitch), sp = s3d_sin((u32)pitch);
  r[0] = cy;
  r[1] = 0;
  r[2] = -sy;
  u[0] = -((sp * sy) >> 14);
  u[1] = cp;
  u[2] = -((sp * cy) >> 14);
  b[0] = (cp * sy) >> 14;
  b[1] = sp;
  b[2] = (cp * cy) >> 14;
}

static void view_matrix(P3dMtx *m, const View *v) {
  s32 r[3], u[3], b[3];
  float fr[3], fu[3], fb[3];
  basis(v, r, u, b);
  for (int i = 0; i < 3; i++) {
    fr[i] = r[i] / 16384.0f;
    fu[i] = u[i] / 16384.0f;
    fb[i] = b[i] / 16384.0f;
  }
  /* The eye sits `dist` behind the target along back. */
  m->m[0][0] = fr[0]; m->m[0][1] = fr[1]; m->m[0][2] = fr[2];
  m->m[1][0] = fu[0]; m->m[1][1] = fu[1]; m->m[1][2] = fu[2];
  m->m[2][0] = fb[0]; m->m[2][1] = fb[1]; m->m[2][2] = fb[2];
  m->m[0][3] = -(fr[0] * v->tgt[0] + fr[1] * v->tgt[1] + fr[2] * v->tgt[2]);
  m->m[1][3] = -(fu[0] * v->tgt[0] + fu[1] * v->tgt[1] + fu[2] * v->tgt[2]);
  m->m[2][3] = -(fb[0] * v->tgt[0] + fb[1] * v->tgt[1] + fb[2] * v->tgt[2]) -
               v->dist;
  m->m[3][0] = m->m[3][1] = m->m[3][2] = 0.0f;
  m->m[3][3] = 1.0f;
}

/* The nearest vertex in view, by depth along the view axis, as a fraction
 * of a unit in Q12; -1 when none is in view. Fixed point and at most 32k
 * vertices a frame, so it costs a few milliseconds whatever the model. */
static int closest_vertex(const View *v, s32 *depth) {
  s32 r[3], u[3], b[3], t[3];
  s64 best = 0x7FFFFFFFFFFFLL;
  s64 dist = (s64)(v->dist * 4096.0f), near = (s64)(NEAR * 4096.0f);
  const s32 *p = model.fixed;
  u32 step = model.nverts / 32768u + 1u;
  int found = -1;

  basis(v, r, u, b);
  for (int i = 0; i < 3; i++)
    t[i] = (s32)(v->tgt[i] * 4096.0f);
  for (u32 i = 0; i < model.nverts; i += step) {
    s64 dx = p[i * 3] - t[0], dy = p[i * 3 + 1] - t[1],
        dz = p[i * 3 + 2] - t[2];
    s64 vz = dist - ((dx * b[0] + dy * b[1] + dz * b[2]) >> 14);
    s64 vx, vy;
    if (vz <= near || vz >= best)
      continue;
    vx = (dx * r[0] + dz * r[2]) >> 14;
    vy = (dx * u[0] + dy * u[1] + dz * u[2]) >> 14;
    if (vx < 0)
      vx = -vx;
    if (vy < 0)
      vy = -vy;
    /* Inside the frustum: |x| / z under 200/240 and |y| / z under 120/240. */
    if (vx * CAM_FOCAL > vz * (TOP_SCREEN_WIDTH / 2) ||
        vy * CAM_FOCAL > vz * (TOP_SCREEN_HEIGHT / 2))
      continue;
    best = vz;
    found = (int)i;
  }
  *depth = (s32)best;
  return found;
}

static int is_new3ds(void) {
  return stereo_model() == STEREO_MODEL_N3DS ||
         stereo_model() == STEREO_MODEL_N3DS_XL;
}

/* Opaque parts first, then cut-out ones, then blended ones far to near, each
 * of those over everything already drawn. */
static u32 draw_order(const P3dMtx *view) {
  u32 n = 0;
  for (int pass = GLB_OPAQUE; pass <= GLB_BLEND; pass++) {
    u32 first = n;
    for (u32 i = 0; i < model.nprim; i++) {
      int mi = model.prim[i].mat;
      int a = mi >= 0 ? model.mat[mi].alpha : GLB_OPAQUE;
      if (a == pass && model.prim[i].icount)
        order[n++] = (u16)i;
    }
    if (pass != GLB_BLEND)
      continue;
    for (u32 i = first + 1; i < n; i++) {
      u16 o = order[i];
      const float *c = model.prim[o].centre;
      float z = view->m[2][0] * c[0] + view->m[2][1] * c[1] +
                view->m[2][2] * c[2];
      u32 j = i;
      for (; j > first; j--) {
        const float *d = model.prim[order[j - 1]].centre;
        float zj = view->m[2][0] * d[0] + view->m[2][1] * d[1] +
                   view->m[2][2] * d[2];
        if (zj <= z) /* view z grows towards the viewer */
          break;
        order[j] = order[j - 1];
      }
      order[j] = o;
    }
  }
  return n;
}

static void vertex3(P3dList *l, float x, float y, const float col[4]) {
  const float pos[4] = {x, y, 0.0f, 1.0f}, uv[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  p3d_vertex3(l, pos, uv, col);
}

/* One eye's frame from the PICA200 into `dst`; 0 if the GPU refused it. */
static int gpu_eye(int which, u32 dst, const P3dMtx *view, float eye,
                   float conv) {
  static const u8 inputs[3] = {P3D_FMT_FLOAT4, P3D_FMT_FLOAT2, P3D_FMT_FLOAT4};
  u32 *buf = (u32 *)(LIST_ADDR + (u32)which * LIST_WORDS * 4u);
  P3dList l;
  P3dMtx proj, mvp, ortho;
  u32 n, bytes;
  int last = -2;

  p3d_persp_stereo_tilt(&proj, TAN_HALF,
                        (float)TOP_SCREEN_WIDTH / TOP_SCREEN_HEIGHT, NEAR, FAR,
                        eye, conv);
  p3d_mtx_mul(&mvp, &proj, view);
  p3d_ortho_tilt(&ortho, 0.0f, TOP_SCREEN_WIDTH, TOP_SCREEN_HEIGHT, 0.0f,
                 1.0f, -1.0f);

  p3d_begin(&l, buf, LIST_WORDS);
  p3d_target(&l);
  p3d_shader(&l, &model_shader);
  p3d_inputs(&l, inputs, 3);
  p3d_vertex_colour(&l);

  /* The backdrop, in screen pixels, behind everything. */
  p3d_depth(&l, 0);
  p3d_texture_off(&l);
  p3d_stage0(&l, 0, P3D_ALPHA_ONE);
  p3d_uniform(&l, MODEL_SHADER_UNIF_MVP, &ortho);
  p3d_draw_begin(&l, P3D_TRIANGLES);
  for (int j = 0; j + 1 < GRID_Y; j++)
    for (int i = 0; i + 1 < GRID_X; i++) {
      static const int corner[6][2] = {{0, 0}, {1, 0}, {1, 1},
                                       {0, 0}, {1, 1}, {0, 1}};
      for (int k = 0; k < 6; k++) {
        int gi = i + corner[k][0], gj = j + corner[k][1];
        vertex3(&l, (float)(gi * TOP_SCREEN_WIDTH) / (GRID_X - 1),
                (float)(gj * TOP_SCREEN_HEIGHT) / (GRID_Y - 1),
                grid_rgb[gj][gi]);
      }
    }
  p3d_draw_end(&l);

  p3d_depth(&l, 1);
  p3d_uniform(&l, MODEL_SHADER_UNIF_MVP, &mvp);
  p3d_buffers(&l, model.base);
  n = draw_order(view);
  for (u32 k = 0; k < n; k++) {
    const GlbPrim *gp = &model.prim[order[k]];
    if (gp->mat != last) {
      const GlbMat *m = gp->mat >= 0 ? &model.mat[gp->mat] : 0;
      int alpha = m ? m->alpha : GLB_OPAQUE;
      if (m && m->tex >= 0) {
        const GlbTex *t = &model.tex[m->tex];
        p3d_texture(&l, t->addr, t->w, t->h, t->levels,
                    (t->nearest ? 0 : P3D_TEX_MAG_LINEAR) |
                        P3D_TEX_MIN_LINEAR | P3D_TEX_MIP_LINEAR |
                        P3D_TEX_WRAP_S(t->wrap_s) | P3D_TEX_WRAP_T(t->wrap_t));
        p3d_stage0(&l, 1,
                   alpha == GLB_OPAQUE ? P3D_ALPHA_ONE : P3D_ALPHA_TEXTURE);
      } else {
        p3d_texture_off(&l);
        p3d_stage0(&l, 0,
                   alpha == GLB_OPAQUE ? P3D_ALPHA_ONE : P3D_ALPHA_VERTEX);
      }
      p3d_fragment(&l, 1, alpha != GLB_BLEND,
                   alpha == GLB_MASK ? m->cutoff : -1);
      last = gp->mat;
    }
    p3d_buffer0(&l, gp->vfirst * sizeof(GlbVertex), sizeof(GlbVertex), 3);
    p3d_draw_elements(&l, gp->index - model.base, gp->icount);
  }
  p3d_flush(&l);

  bytes = p3d_end(&l);
  return bytes && gpu_p3d((u32)buf, bytes, dst, 0x000000FFu);
}

/* The software renderer's fallback: the coloured cube, flat-shaded. */
static const S3dVec cube_verts[8] = {
    {-S3D_ONE, -S3D_ONE, -S3D_ONE}, {S3D_ONE, -S3D_ONE, -S3D_ONE},
    {S3D_ONE, S3D_ONE, -S3D_ONE},   {-S3D_ONE, S3D_ONE, -S3D_ONE},
    {-S3D_ONE, -S3D_ONE, S3D_ONE},  {S3D_ONE, -S3D_ONE, S3D_ONE},
    {S3D_ONE, S3D_ONE, S3D_ONE},    {-S3D_ONE, S3D_ONE, S3D_ONE},
};
static const S3dFace cube_faces[6] = {
    {4, {4, 5, 6, 7}, {0x4C, 0x8B, 0xF5}}, {4, {1, 0, 3, 2}, {0x52, 0xC4, 0x7A}},
    {4, {5, 1, 2, 6}, {0xE8, 0x5D, 0x5D}}, {4, {0, 4, 7, 3}, {0xF2, 0x9E, 0x4C}},
    {4, {7, 6, 2, 3}, {0xF0, 0xF0, 0xF0}}, {4, {0, 1, 5, 4}, {0xF5, 0xD0, 0x4C}},
};
static const S3dMesh soft_cube = {cube_verts, 8, cube_faces, 6};
static const S3dVec soft_light = {1800, 3600, 2600};

static void backdrop_to(volatile u8 *fb) {
  screen_touch(fb);
  /* Nothing of the last frame may sit dirty in the cache, to be written back
   * over the GPU's copy. */
  os_dcache_flush();
  if (!gpu_alive() || !gpu_texcopy(CUBE_BG_ADDR, (u32)fb, TOP_FB_SIZE)) {
    const u32 *s = (const u32 *)CUBE_BG_ADDR;
    volatile u32 *d = (volatile u32 *)fb;
    for (u32 i = 0; i < TOP_FB_SIZE / 4u; i++)
      d[i] = s[i];
  }
}

static void cpu_eye(volatile u8 *fb, const S3dCamera *cam) {
  backdrop_to(fb);
  s3d_draw(fb, TOP_SCREEN_WIDTH, TOP_SCREEN_HEIGHT, &soft_cube, cam,
           &soft_light);
}

/* Both eyes, or one, converging on the nearest part in view. Falls back to
 * the software cube for good if the GPU fails. */
static void draw_eyes(Stats *st, const View *v, int level) {
  s32 near_q12;
  int vtx = closest_vertex(v, &near_q12);
  float conv = vtx >= 0 ? near_q12 / 4096.0f : v->dist;
  float eye = EYE_FRAC * conv * 0.5f * (float)level / 256.0f;

  st->closest = vtx >= 0 ? (int)glb_prim_of(&model, (u32)vtx) : -1;
  if (st->renderer == REND_GPU) {
    P3dMtx view;
    int ok;
    view_matrix(&view, v);
    screen_touch(VRAM_TOP_LA);
    os_dcache_flush();
    if (st->stereo)
      ok = gpu_eye(0, (u32)VRAM_TOP_LA, &view, -eye, conv) &&
           gpu_eye(1, RIGHT_ADDR, &view, eye, conv);
    else
      ok = gpu_eye(0, (u32)VRAM_TOP_LA, &view, 0.0f, conv);
    if (ok)
      return;
    {
      u32 stereo, writes, runs, waits, stat;
      gpu_3d_info(&stereo, &writes, &st->p3d_state, &runs, &waits, &stat);
    }
    st->renderer = REND_CPU;
    st->gpu_failed = 1;
  }

  {
    S3dCamera cam = {v->yaw >> FINE, v->pitch >> FINE,
                     (s32)(v->dist * S3D_ONE), CAM_FOCAL, 0, 0};
    if (st->stereo) {
      cam.conv = (s32)(conv * S3D_ONE);
      cam.eye = -(s32)(eye * S3D_ONE);
      cpu_eye(VRAM_TOP_LA, &cam);
      cam.eye = -cam.eye;
      cpu_eye((volatile u8 *)RIGHT_ADDR, &cam);
    } else {
      cpu_eye(VRAM_TOP_LA, &cam);
    }
  }
}

static const char *p3d_why(u32 state) {
  switch (state) {
    case GPU_P3D_CLEAR_TIMEOUT: return "clear timed out";
    case GPU_P3D_LIST_TIMEOUT:  return "command list never finished";
    case GPU_P3D_COPY_TIMEOUT:  return "copy to the screen timed out";
    default:                    return "the GPU did not answer";
  }
}

static void draw_bottom(const Stats *s) {
  const int w = BOT_SCREEN_WIDTH, sh = BOT_SCREEN_HEIGHT;
  volatile u8 *fb = VRAM_BOT_A;
  char buf[96], *p;

  ui_wallpaper(fb, w, sh, sh);
  draw_gradient_round_rect(fb, 16, 10, w - 32, 178, 14, sh, COLOR_PANEL_TOP,
                           COLOR_PANEL_BOT);

  if (s->measured) {
    p = put_tenths(buf, s->fps10);
    p = put_str(p, " fps");
  } else {
    p = put_str(buf, "Measuring");
  }
  *p = 0;
  ui_text_mid(fb, w / 2, 18, sh, buf, COLOR_WHITE, COLOR_PANEL_TOP, &ui_title);

  if (s->measured) {
    p = put_tenths(buf, s->frame_us / 100u);
    p = put_str(p, " ms a frame, drawing ");
    p = put_tenths(p, s->draw_us / 100u);
    p = put_str(p, " ms");
    *p = 0;
    ui_text_mid(fb, w / 2, 48, sh, buf, COLOR_HM_TEXT2, COLOR_PANEL_TOP,
                &ui_font);
  }

  if (s->renderer == REND_GPU) {
    p = put_str(buf, model.title);
    p = put_str(p, ": ");
    p = put_int(p, (int)model.ntris);
    p = put_str(p, " triangles, ");
    p = put_int(p, (int)model.ntex);
    p = put_str(p, model.ntex == 1 ? " texture" : " textures");
  } else if (s->gpu_failed) {
    p = put_str(buf, "Software cube: GPU ");
    p = put_str(p, p3d_why(s->p3d_state));
  } else {
    p = put_str(buf, "Software cube: the GPU is not running");
  }
  *p = 0;
  ui_text_mid_fit(fb, w / 2, 76, sh, buf, w - 48, COLOR_WHITE,
                  COLOR_PANEL_TOP, &ui_small);

  if (s->note[0])
    ui_text_mid_fit(fb, w / 2, 94, sh, s->note, w - 48, COLOR_HM_TEXT2,
                    COLOR_PANEL_TOP, &ui_small);

  p = put_str(buf, "Closest: ");
  p = put_str(p, s->closest >= 0 && s->renderer == REND_GPU
                     ? model.prim[s->closest].name
                     : "nothing in view");
  *p = 0;
  ui_text_mid_fit(fb, w / 2, 112, sh, buf, w - 48, COLOR_HM_TEXT2,
                  COLOR_PANEL_BOT, &ui_small);

  p = put_str(buf, stereo_model_name(stereo_model()));
  if (!stereo_capable()) {
    p = put_str(p, ": no 3D screen");
  } else {
    p = put_str(p, ", 3D slider ");
    p = put_int(p, s->level * 100 / 256);
    p = put_str(p, "% (raw ");
    p = put_int(p, s->slider_raw);
    p = put_str(p, s->stereo ? "), 3D on" : "), 2D");
  }
  *p = 0;
  ui_text_mid(fb, w / 2, 130, sh, buf, COLOR_HM_TEXT2, COLOR_PANEL_BOT,
              &ui_small);

  if (s->stereo && is_new3ds()) {
    if (s->barrier_pos == STEREO_POS_FIXED) {
      p = put_str(buf, "Barrier: fixed pattern   X: next position");
    } else {
      p = put_str(buf, "Barrier: position ");
      p = put_int(p, s->barrier_pos);
      p = put_str(p, "   X: next position");
    }
    *p = 0;
    ui_text_mid(fb, w / 2, 148, sh, buf, COLOR_HM_TEXT2, COLOR_PANEL_BOT,
                &ui_small);
  }

  ui_text_mid(fb, w / 2, sh - 40, sh,
              "Circle Pad: orbit   D-pad: pan", COLOR_HM_TEXT2,
              COLOR_HM_BG_BOT, &ui_small);
  ui_text_mid(fb, w / 2, sh - 22, sh,
              "Y: zoom in   A: zoom out   B: back", COLOR_HM_TEXT2,
              COLOR_HM_BG_BOT, &ui_small);
}

/* Shown on the bottom screen while model.glb loads. */
static void progress(const char *stage, u32 done, u32 total) {
  const int w = BOT_SCREEN_WIDTH, sh = BOT_SCREEN_HEIGHT;
  volatile u8 *fb = VRAM_BOT_A;
  const int bar = w - 96;
  char buf[48], *p;
  int fill = total ? (int)((u64)bar * done / total) : 0;

  ui_wallpaper(fb, w, sh, sh);
  draw_gradient_round_rect(fb, 28, 70, w - 56, 100, 14, sh, COLOR_PANEL_TOP,
                           COLOR_PANEL_BOT);
  ui_text_mid(fb, w / 2, 84, sh, "Loading model.glb", COLOR_WHITE,
              COLOR_PANEL_TOP, &ui_title);
  p = put_str(buf, stage);
  if (total > 1 && stage[0] != 'R') {
    p = put_str(p, " ");
    p = put_int(p, (int)done + 1);
    p = put_str(p, " of ");
    p = put_int(p, (int)total);
  }
  *p = 0;
  ui_text_mid(fb, w / 2, 116, sh, buf, COLOR_HM_TEXT2, COLOR_PANEL_TOP,
              &ui_font);
  draw_filled_round_rect(fb, 48, 144, bar, 10, 5, sh, COLOR_HM_BG);
  if (fill > 10)
    draw_filled_round_rect(fb, 48, 144, fill, 10, 5, sh, g_accent);
  screen_present_bottom();
}

/* -256..256 from a reading centred on the rest point. */
static int axis(int v) {
  int m = v < 0 ? -v : v;
  if (m <= CPAD_DEAD)
    return 0;
  m = m >= CPAD_FULL ? 256 : (m - CPAD_DEAD) * 256 / (CPAD_FULL - CPAD_DEAD);
  return v < 0 ? -m : m;
}

static void present(const Stats *st, int bottom) {
  if (st->stereo) {
    gpu_present_st_async((u32)VRAM_TOP_LA, RIGHT_ADDR,
                         bottom ? (u32)VRAM_BOT_A : 0);
    if (bottom)
      gpu_wait_idle();
  } else if (bottom) {
    anim_present(ANIM_BOTH); /* one operation, so the top keeps its blank */
  } else {
    screen_present_top();
  }
}

static void load(Stats *st) {
  static FATFS fs;
  if (f_mount(&fs, "", 1) != FR_OK) {
    put_str(st->note, "No SD card: showing the built-in cube")[0] = 0;
    glb_cube(&model);
    return;
  }
  if (glb_load(MODEL_PATH, &model, progress, st->note, sizeof(st->note))) {
    if (model.tex_failed) {
      char *p = put_int(st->note, (int)model.tex_failed);
      p = put_str(p, model.tex_failed == 1 ? " texture" : " textures");
      p = put_str(p, " could not be decoded");
      *p = 0;
    } else {
      st->note[0] = 0;
    }
  } else {
    /* The reason stays in the note, followed by what is shown instead. */
    char *p = st->note;
    while (*p)
      p++;
    if (p + 10 < st->note + sizeof(st->note))
      put_str(p, ": a cube")[0] = 0;
    glb_cube(&model);
  }
  f_mount(NULL, "", 0);
}

void model_screen(void) {
  const s64 rate = (s64)DEG(ORBIT_DEG_S) << FINE; /* per second, at 256 */
  View v = {(u32)DEG(35) << FINE, DEG(20) << FINE, START_DIST, {0, 0, 0}};
  u32 t_last, t_win, frames = 0, draw_sum = 0;
  /* Stereo frames go through the GPU's presents and backbuffers. */
  int can_gpu = gpu_alive() && VRAM_TOP_LA == VRAM_TOP_BACK;
  int can_3d = can_gpu && stereo_capable();
  static Stats st;

  st = (Stats){0};
  st.renderer = can_gpu ? REND_GPU : REND_CPU;
  st.barrier_pos = STEREO_POS_FIXED;
  st.closest = -1;

  /* A first frame for both screens, so the slide in runs before loading. */
  backdrop_build();
  backdrop_to(VRAM_TOP_LA);
  ui_text_mid(VRAM_TOP_LA, TOP_SCREEN_WIDTH / 2, 110, TOP_SCREEN_HEIGHT,
              "Loading...", COLOR_WHITE, bg_top, &ui_title);
  screen_present_top();
  progress("Reading", 0, 1);

  if (can_gpu)
    load(&st);
  else
    glb_cube(&model);

  draw_eyes(&st, &v, 0);
  draw_bottom(&st);
  screen_present_top();
  screen_present_bottom();

  t_last = t_win = timer_ticks();
  for (;;) {
    u32 dt, t0, us, k, held;
    int ax, ay, want;
    float fdt;

    k = get_keys_down();
    if (k & BUTTON_B)
      break;

    st.slider_raw = stereo_slider_raw();
    st.level = can_3d ? stereo_slider(st.slider_raw) : 0;
    want = st.level > 0;
    if ((k & BUTTON_X) && st.stereo && is_new3ds()) {
      st.barrier_pos = st.barrier_pos + 1 >= STEREO_POSITIONS
                           ? STEREO_POS_FIXED
                           : st.barrier_pos + 1;
      stereo_enable(1, st.barrier_pos);
    }
    if (want != st.stereo) {
      gpu_wait_idle();
      if (stereo_enable(want, st.barrier_pos))
        st.stereo = want;
    }

    dt = timer_us_since(t_last);
    t_last = timer_ticks();
    if (dt > 100000u)
      dt = 100000u;
    fdt = dt / 1000000.0f;
    {
      int px, py, rx, ry;
      cpad_read(&px, &py, &rx, &ry);
      ax = axis(px);
      ay = axis(py);
    }
    /* Pushing right moves the camera right, up moves it up. */
    v.yaw += (u32)(s32)(rate * ax * (s64)dt / (256 * 1000000LL));
    v.pitch += (s32)(rate * ay * (s64)dt / (256 * 1000000LL));
    if (v.pitch > (PITCH_MAX << FINE))
      v.pitch = PITCH_MAX << FINE;
    if (v.pitch < -(PITCH_MAX << FINE))
      v.pitch = -(PITCH_MAX << FINE);

    held = get_keys();
    if (held & BUTTON_Y)
      v.dist -= v.dist * ZOOM_RATE * fdt;
    if (held & BUTTON_A)
      v.dist += v.dist * ZOOM_RATE * fdt;
    if (v.dist < DIST_MIN)
      v.dist = DIST_MIN;
    if (v.dist > DIST_MAX)
      v.dist = DIST_MAX;
    if (held & (BUTTON_DLEFT | BUTTON_DRIGHT | BUTTON_DUP | BUTTON_DDOWN)) {
      s32 r[3], u[3], b[3];
      float sx = (held & BUTTON_DRIGHT) ? 1.0f : (held & BUTTON_DLEFT) ? -1.0f : 0.0f;
      float sy = (held & BUTTON_DUP) ? 1.0f : (held & BUTTON_DDOWN) ? -1.0f : 0.0f;
      float step = v.dist * PAN_RATE * fdt / 16384.0f;
      basis(&v, r, u, b);
      for (int i = 0; i < 3; i++) {
        v.tgt[i] += (r[i] * sx + u[i] * sy) * step;
        if (v.tgt[i] > PAN_MAX)
          v.tgt[i] = PAN_MAX;
        if (v.tgt[i] < -PAN_MAX)
          v.tgt[i] = -PAN_MAX;
      }
    }

    /* The previous present ends at a vertical blank; only the work after it
     * counts as drawing. */
    gpu_wait_idle();
    t0 = timer_ticks();
    draw_eyes(&st, &v, st.level);
    draw_sum += timer_us_since(t0);
    frames++;

    us = timer_us_since(t_win);
    if (us < STATS_US) {
      present(&st, 0);
      continue;
    }
    st.measured = 1;
    st.fps10 = (u32)(((u64)frames * 10000000u) / us);
    st.frame_us = us / frames;
    st.draw_us = draw_sum / frames;
    draw_bottom(&st);
    present(&st, 1);
    frames = draw_sum = 0;
    t_win = timer_ticks();
  }

  if (st.stereo) {
    gpu_wait_idle();
    stereo_enable(0, STEREO_POS_FIXED);
  }
}
