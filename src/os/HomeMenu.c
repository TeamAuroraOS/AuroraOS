#include "amenu.h"
#include "appicon.h"
#include "audio.h"
#include "font.h"
#include "homemenu.h"
#include "anim.h"
#include "banner.h"
#include "ff.h"
#include "fileview.h"
#include "gpu.h"
#include "homelayout.h"
#include "i2c.h"
#include "icons.h"
#include "keyboard.h"
#include "lang.h"
#include "power.h"
#include "statusbar.h"
#include "system_menu.h"
#include "theme.h"
#include "timer.h"
#include "touch.h"
#include "ui.h"
#include "uisound.h"

#define BW  BOT_SCREEN_WIDTH
#define BSH BOT_SCREEN_HEIGHT
#define TSH TOP_SCREEN_HEIGHT

#define SLOT_SIZE 46
#define SLOT_GAP  12
#define SLOT_STEP (SLOT_SIZE + SLOT_GAP)
#define SLOT_RING 3
#define SLOT_LIFT 3 /* how far the selected tile grows on each side */
#define HOLD_LIFT 6 /* a carried tile, and the one it would go into */
#define GRID_W    (HOME_COLS * SLOT_SIZE + (HOME_COLS - 1) * SLOT_GAP)
#define GRID_X    ((BW - GRID_W) / 2)
#define GRID_Y    52
#define DOTS_Y    225

#define BAR_X 8
#define BAR_Y 8
#define BAR_W (BW - 16)
#define BAR_H 30
enum { BTN_POWER, BTN_BACK, BTN_SHOP, BTN_FILES, BTN_MUSIC, BTN_CAMERA, BTN_SETTINGS };

/* Touch, in ms and pixels. */
#define HOLD_MS   220 /* a press this long picks the tile up */
#define SPRING_MS 450 /* held over a folder this long, it opens */
#define EDGE_MS   450 /* held at a side of the screen this long, the page turns */
#define BACK_MS   350 /* held over the top bar this long, it goes up */
#define SLOP      8   /* how far a press may wander and still be a tap */
#define SWIPE     40  /* how far a swipe goes to turn the page */

/* What a touch landed on: a slot of the page, or one of these. */
#define HIT_NONE (-1)
#define HIT_BTN  100 /* + BTN_* */
#define HIT_DOT  200 /* + page */

#define GAP_ENTRY HL_NONE
#define ANIM_N    (HOME_ITEMS + HOME_FOLDERS)

static const HomeApp *apps;
static int napps;
static const HomeHooks *hk;
static const char *keys[HOME_ITEMS];

static int view = HOME_ROOT; /* the container shown */
static int page, sel;        /* sel is the slot on the page */
static int bar = -1;         /* the bar button with the focus, or -1 */

/* What the view shows: its entries and, while one is carried, a gap. */
static s16 disp[HOME_ITEMS + 1];
static int ndisp;

/* The carried entry is out of the layout until it is put down. */
static int held = HL_NONE, held_c, held_i;
static int gap = -1;  /* where it would go, or -1 when the view has no room */
static int into = -1; /* the tile it would go into, or -1 */
static int by_touch;  /* the stylus has it, rather than the D-pad */

static AnimVal scroll; /* the content x at the left of the screen */
static AnimVal ring_x, ring_y, ring_w, ring_h, held_x, held_y;
static AnimVal pos_x[ANIM_N], pos_y[ANIM_N], lift[ANIM_N];
static u8 shown[ANIM_N];
static int dirty;

enum { T_IDLE, T_PRESS, T_SWIPE, T_DRAG, T_WAIT };
static int tstate, t_x0, t_y0, t_x, t_y, t_hit, grab_dx, grab_dy, moved;
static u32 t_t0;
static int hov_kind, hov_arg;
static u32 hov_t0;

static const Color folder_bot = {0x26, 0x27, 0x2E},
                   folder_tint = {0x3E, 0x4A, 0x60},
                   settings_tint = {0x4A, 0x4C, 0x58};

static char *put_str(char *p, const char *s) {
  while (*s)
    *p++ = *s++;
  *p = 0;
  return p;
}

static char *put_int(char *p, int v) {
  char t[12];
  int n = 0;
  do {
    t[n++] = (char)('0' + v % 10);
    v /= 10;
  } while (v);
  while (n)
    *p++ = t[--n];
  *p = 0;
  return p;
}

static int iabs(int v) { return v < 0 ? -v : v; }

static int anim_idx(int e) {
  return HL_IS_FOLDER(e) ? HOME_ITEMS + HL_FOLDER_ID(e) : e;
}

static int pages(void) {
  int p = (ndisp + HOME_PER_PAGE - 1) / HOME_PER_PAGE;
  return p < 1 ? 1 : p;
}

static int cur(void) { return page * HOME_PER_PAGE + sel; }

static int entry_at(int k) {
  return (k >= 0 && k < ndisp) ? disp[k] : HL_NONE;
}

/* The view's list index of display index `k`, the gap aside. */
static int list_index(int k) { return (gap >= 0 && k > gap) ? k - 1 : k; }

/* Where display index `k` sits: x across all the pages, y on the screen. */
static int slot_cx(int k) {
  return (k / HOME_PER_PAGE) * BW + GRID_X + (k % HOME_COLS) * SLOT_STEP;
}
static int slot_cy(int k) {
  return GRID_Y + ((k % HOME_PER_PAGE) / HOME_COLS) * SLOT_STEP;
}

static void rebuild(void) {
  int n = hl_count(view);
  ndisp = 0;
  for (int i = 0; i <= n; i++) {
    if (held != HL_NONE && i == gap)
      disp[ndisp++] = GAP_ENTRY;
    if (i < n)
      disp[ndisp++] = (s16)hl_at(view, i);
  }
  if (page >= pages())
    page = pages() - 1;
}

static const char *entry_name(int e) {
  if (e == HL_NONE)
    return L(STR_EMPTY_SLOT);
  return HL_IS_FOLDER(e) ? hl_name(HL_FOLDER_ID(e)) : apps[e].name;
}

/* "Folder, 3 items", or the app's own line. */
static const char *entry_dev(int e, char *buf) {
  char *p;
  int n;
  if (e == HL_NONE)
    return "";
  if (!HL_IS_FOLDER(e))
    return apps[e].dev ? apps[e].dev : "";
  n = hl_count(HL_FOLDER_ID(e));
  p = put_str(buf, L(STR_FOLDER));
  p = put_str(p, ", ");
  p = put_int(p, n);
  p = put_str(p, " ");
  put_str(p, L(n == 1 ? STR_ITEM : STR_ITEMS));
  return buf;
}

/* 32x32 bits at half size, coverage as alpha. */
static void bits_half(volatile u8 *fb, int x, int y, int sh,
                      const unsigned char *bits, Color c) {
  for (int j = 0; j < 16; j++)
    for (int i = 0; i < 16; i++) {
      int n = 0;
      for (int d = 0; d < 4; d++) {
        int bx = i * 2 + (d & 1), by = j * 2 + (d >> 1);
        n += (bits[by * ICON_ROW_BYTES + bx / 8] >> (7 - bx % 8)) & 1;
      }
      if (n)
        draw_pixel_alpha(fb, x + i, y + j, sh, c, n * 64);
    }
}

static void icon16(volatile u8 *fb, int x, int y, int sh, u32 asset,
                   const unsigned char *bits, Color c) {
  if (asset != UI_NO_ASSET && draw_asset_boxed(fb, x, y, 16, 16, sh, asset, c))
    return;
  if (bits)
    bits_half(fb, x, y, sh, bits, c);
}

static unsigned galaxy_phase;
static int galaxy_dirty;
#define ICON_CACHE_N 32
#define ICON_SHAPE_N 24
#define ICON_MAX 96
static struct {
  const u16 *source;
  unsigned phase, age;
  Color pixels[1024];
} icon_cache[ICON_CACHE_N];
static struct {
  int size, radius;
  unsigned age;
  u8 scale[ICON_MAX], alpha[ICON_MAX * ICON_MAX];
} icon_shape[ICON_SHAPE_N];
static unsigned icon_age;

static void icon_cache_reset(void) {
  for (int i=0;i<ICON_CACHE_N;i++) icon_cache[i].source=0;
}

static const Color *icon_pixels(const HomeApp *app) {
  unsigned phase=app->icon_animation ? (galaxy_phase & 63u)+1u : 0;
  int slot=0,reuse=-1;
  for (int i=0;i<ICON_CACHE_N;i++) {
    if(icon_cache[i].source==app->color_icon)reuse=i;
    if (icon_cache[i].source==app->color_icon && icon_cache[i].phase==phase) {
      icon_cache[i].age=++icon_age;
      return icon_cache[i].pixels;
    }
    if (!icon_cache[i].source || icon_cache[i].age<icon_cache[slot].age) slot=i;
  }
  if(reuse>=0)slot=reuse;
  for (unsigned sy=0;sy<32;sy++) for (unsigned sx=0;sx<32;sx++) {
    u16 c=app->color_icon[sy*32+sx];
    Color col={(u8)((c>>11)*255/31),(u8)(((c>>5)&63)*255/63),(u8)((c&31)*255/31)};
    if (app->icon_animation) {
      unsigned wave=(sx+sy+galaxy_phase)&63;
      if(wave>31) wave=63-wave;
      col.r=(u8)(col.r+(255-col.r)*wave/180);
      col.b=(u8)(col.b+(255-col.b)*wave/100);
      if(((sx*73+sy*127)%193)==0 && ((galaxy_phase+sx)&15)<5)
        col.r=col.g=col.b=245;
    }
    icon_cache[slot].pixels[sy*32+sx]=col;
  }
  icon_cache[slot].source=app->color_icon;
  icon_cache[slot].phase=phase;
  icon_cache[slot].age=++icon_age;
  return icon_cache[slot].pixels;
}

static int icon_mask(int size,int radius) {
  int slot=0;
  for(int i=0;i<ICON_SHAPE_N;i++) {
    if(icon_shape[i].size==size && icon_shape[i].radius==radius) {
      icon_shape[i].age=++icon_age;return i;
    }
    if(!icon_shape[i].size || icon_shape[i].age<icon_shape[slot].age)slot=i;
  }
  for(int x=0;x<size;x++) {
    icon_shape[slot].scale[x]=(u8)(x*32/size);
    for(int y=0;y<size;y++)icon_shape[slot].alpha[x*size+y]=(u8)appicon_alpha(x,y,size,radius);
  }
  icon_shape[slot].size=size;icon_shape[slot].radius=radius;
  icon_shape[slot].age=++icon_age;return slot;
}

static int color_icon(volatile u8 *fb,int x,int y,int size,int radius,int sh,int e) {
  if(e<0 || HL_IS_FOLDER(e) || !apps[e].color_icon || size<1 || size>ICON_MAX)return 0;
  int width=screen_fb_width(fb),x0=x<0?-x:0,y0=y<0?-y:0;
  int x1=x+size>width?width-x:size,y1=y+size>sh?sh-y:size;
  if(x0>=x1 || y0>=y1)return 1;
  const Color *pixels=icon_pixels(&apps[e]);
  int m=icon_mask(size,radius);
  const u8 *scale=icon_shape[m].scale,*mask=icon_shape[m].alpha;
  screen_touch(fb);
  for(int xx=x0;xx<x1;xx++) {
    unsigned off=((x+xx)*sh+sh-1-y-y0)*3;
    unsigned sx=scale[xx];
    for(int yy=y0;yy<y1;yy++,off-=3) {
      volatile u8 *dst=fb+off;
      unsigned alpha=mask[xx*size+yy];
      if(!alpha)continue;
      Color col=pixels[scale[yy]*32+sx];
      if(alpha==255) {dst[0]=col.b;dst[1]=col.g;dst[2]=col.r;}
      else {
        unsigned inv=256-alpha;
        dst[0]=(u8)((dst[0]*inv+col.b*alpha)>>8);
        dst[1]=(u8)((dst[1]*inv+col.g*alpha)>>8);
        dst[2]=(u8)((dst[2]*inv+col.r*alpha)>>8);
      }
    }
  }
  return 1;
}

static void entry_icon16(volatile u8 *fb, int x, int y, int sh, int e) {
  if (color_icon(fb, x, y, 16, 3, sh, e)) return;
  if (HL_IS_FOLDER(e))
    icon16(fb, x, y, sh, ASSET_ICON_FOLDER_16, icon_folder_bits,
           COLOR_HM_TEXT2);
  else
    icon16(fb, x, y, sh, apps[e].asset_xs, apps[e].icon, COLOR_WHITE);
}

static void entry_icon32(volatile u8 *fb, int x, int y, int sh, int e) {
  if (color_icon(fb, x, y, 32, 6, sh, e)) return;
  if (HL_IS_FOLDER(e))
    ui_icon(fb, x, y, 32, sh, ASSET_ICON_FOLDER_32, icon_folder_bits,
            COLOR_HM_TEXT2);
  else
    ui_icon(fb, x, y, 32, sh, apps[e].asset_sm, apps[e].icon, COLOR_WHITE);
}

static void tile(volatile u8 *fb, int e, int x, int y, int size) {
  int r = 10 + (size - SLOT_SIZE) / 4;
  if (!HL_IS_FOLDER(e)) {
    draw_gradient_round_rect(fb, x, y, size, size, r, BSH, g_theme.slot_top,
                             g_theme.slot_bot);
    if (!color_icon(fb, x, y, size, r, BSH, e))
      ui_icon(fb, x, y, size, BSH, apps[e].asset_sm, apps[e].icon, COLOR_WHITE);
    return;
  }
  draw_gradient_round_rect(fb, x, y, size, size, r, BSH, g_theme.folder_top,
                           g_theme.folder_bot);
  {
    int f = HL_FOLDER_ID(e), n = hl_count(f);
    int ox = x + (size - 36) / 2, oy = y + (size - 36) / 2;
    if (!n)
      ui_icon(fb, x, y, size, BSH, ASSET_ICON_FOLDER_32, icon_folder_bits,
              COLOR_HM_TEXT2);
    for (int i = 0; i < 4 && i < n; i++)
      entry_icon16(fb, ox + (i % 2) * 20, oy + (i / 2) * 20, BSH, hl_at(f, i));
  }
}

static int has_btn(int b) { return b != BTN_BACK || view != HOME_ROOT; }

static int back_w(void) {
  int w = 22 + ui_tw(&ui_bold, hl_name(view));
  return w > BW - 216 ? BW - 216 : w;
}

static void btn_rect(int b, int *x, int *y, int *w, int *h) {
  *y = BAR_Y + 3;
  *h = BAR_H - 6;
  if (b == BTN_POWER) {
    *x = BAR_X + 5;
    *w = 26;
  } else if (b == BTN_BACK) {
    *x = BAR_X + 37;
    *w = back_w();
  } else if (b == BTN_SHOP) {
    *x = BW - 160;
    *w = 26;
  } else if (b == BTN_FILES) {
    *x = BW - 130;
    *w = 26;
  } else if (b == BTN_MUSIC) {
    *x = BW - 100;
    *w = 26;
  } else if (b == BTN_CAMERA) {
    *x = BW - 70;
    *w = 26;
  } else {
    *x = BW - 40;
    *w = 28;
  }
}

static void chevron(volatile u8 *fb, int x, int cy, Color c) {
  for (int i = 0; i < 6; i++) {
    draw_filled_rect(fb, x + i, cy - i - 1, 2, 2, BSH, c);
    draw_filled_rect(fb, x + i, cy + i - 1, 2, 2, BSH, c);
  }
}

static void draw_bar(void) {
  volatile u8 *fb = VRAM_BOT_A;
  int x, y, w, h;
  if (held != HL_NONE && view != HOME_ROOT) {
    Color bg = hov_kind == 1 ? g_accent : COLOR_HM_BAR;
    draw_filled_round_rect(fb, BAR_X, BAR_Y, BAR_W, BAR_H, 10, BSH, bg);
    ui_text_mid(fb, BW / 2, BAR_Y + (BAR_H - ui_th(&ui_bold)) / 2, BSH,
                hov_kind == 1 ? "Release to move out of folder"
                              : "Drag here to move out of folder",
                hov_kind == 1 ? COLOR_WHITE : g_theme.text, bg, &ui_bold);
    return;
  }
  draw_filled_round_rect(fb, BAR_X, BAR_Y, BAR_W, BAR_H, 10, BSH,
                         COLOR_HM_BAR);

  btn_rect(BTN_POWER, &x, &y, &w, &h);
  icon16(fb, x + (w - 16) / 2, y + (h - 16) / 2, BSH, ASSET_ICON_POWER_16,
         icon_power_bits, COLOR_HM_TEXT2);

  if (view != HOME_ROOT) {
    btn_rect(BTN_BACK, &x, &y, &w, &h);
    chevron(fb, x + 5, y + h / 2, COLOR_HM_TEXT2);
    ui_text_fit(fb, x + 18, y + (h - ui_th(&ui_bold)) / 2, BSH,
                hl_name(view), w - 20, COLOR_WHITE, COLOR_HM_BAR, &ui_bold);
  }

  /* Shop (shop.png), Files (Folder.png), Music, Camera (camera.png), Settings */
  btn_rect(BTN_SHOP, &x, &y, &w, &h);
  icon16(fb, x + (w - 16) / 2, y + (h - 16) / 2, BSH, ASSET_ICON_SHOP_BAR_16,
         icon_boot_bits, COLOR_HM_TEXT2);

  btn_rect(BTN_FILES, &x, &y, &w, &h);
  icon16(fb, x + (w - 16) / 2, y + (h - 16) / 2, BSH, ASSET_ICON_FOLDER_BAR_16,
         icon_folder_bits, COLOR_HM_TEXT2);

  btn_rect(BTN_MUSIC, &x, &y, &w, &h);
  icon16(fb, x + (w - 16) / 2, y + (h - 16) / 2, BSH, ASSET_ICON_MUSIC_16,
         icon_music_bits, COLOR_HM_TEXT2);

  btn_rect(BTN_CAMERA, &x, &y, &w, &h);
  icon16(fb, x + (w - 16) / 2, y + (h - 16) / 2, BSH, ASSET_APP_CAMERA_16,
         icon_camera_bits, COLOR_HM_TEXT2);

  {
    int gx = BW - 34, gy = 18;
    draw_filled_round_rect(fb, gx, gy, 16, 4, 2, BSH, COLOR_HM_TEXT2);
    draw_filled_round_rect(fb, gx + 9, gy - 2, 5, 8, 2, BSH, COLOR_WHITE);
    draw_filled_round_rect(fb, gx, gy + 8, 16, 4, 2, BSH, COLOR_HM_TEXT2);
    draw_filled_round_rect(fb, gx + 2, gy + 6, 5, 8, 2, BSH, COLOR_WHITE);
  }
}

static void draw_dots(int sc) {
  volatile u8 *fb = VRAM_BOT_A;
  int n = pages(), now = (sc + BW / 2) / BW, x;
  if (n < 2)
    return;
  if (now < 0)
    now = 0;
  if (now >= n)
    now = n - 1;
  x = (BW - (n * 6 + (n - 1) * 8 + 8)) / 2;
  for (int p = 0; p < n; p++) {
    int w = p == now ? 14 : 6;
    draw_filled_round_rect(fb, x, DOTS_Y, w, 6, 3, BSH,
                           p == now ? COLOR_WHITE : COLOR_HM_TEXT2);
    x += w + 8;
  }
}

typedef struct { int x,y,w,h; } HomeRect;
typedef struct { int e,x,y,size; unsigned phase; } HomeTileStamp;
static u8 bottom_base[BW*BSH*3] __attribute__((aligned(4)));
static HomeTileStamp bottom_tiles[HOME_PER_PAGE];
static HomeRect bottom_ring;
static int bottom_valid,bottom_view,bottom_page,bottom_ndisp,bottom_count;

static HomeRect ring_rect(void) {
  int dx=bar<0?page*BW-anim_px(&scroll):0;
  return (HomeRect){anim_px(&ring_x)+dx,anim_px(&ring_y),anim_px(&ring_w),anim_px(&ring_h)};
}
static void paint_ring(HomeRect q) {
  int r=q.h/2;
  if(r>10+SLOT_LIFT/2+SLOT_RING)r=10+SLOT_LIFT/2+SLOT_RING;
  draw_round_ring(VRAM_BOT_A,q.x,q.y,q.w,q.h,r,SLOT_RING+1,BSH,g_accent);
}
static int rect_equal(HomeRect a,HomeRect b) {
  return a.x==b.x && a.y==b.y && a.w==b.w && a.h==b.h;
}
static int rect_hits(HomeRect a,HomeRect b) {
  return a.w>0 && a.h>0 && b.w>0 && b.h>0 && a.x<b.x+b.w && b.x<a.x+a.w && a.y<b.y+b.h && b.y<a.y+a.h;
}
static HomeRect rect_union(HomeRect a,HomeRect b) {
  if(a.w<=0 || a.h<=0)return b;
  if(b.w<=0 || b.h<=0)return a;
  int x=a.x<b.x?a.x:b.x,y=a.y<b.y?a.y:b.y;
  int r=a.x+a.w>b.x+b.w?a.x+a.w:b.x+b.w;
  int d=a.y+a.h>b.y+b.h?a.y+a.h:b.y+b.h;
  return (HomeRect){x,y,r-x,d-y};
}
static HomeRect stamp_rect(HomeTileStamp t) {return (HomeRect){t.x,t.y,t.size,t.size};}
static int bottom_stamps(HomeTileStamp *out) {
  int n=0,sc=anim_px(&scroll);
  for(int k=0;k<ndisp;k++) {
    int e=disp[k];if(e==GAP_ENTRY)continue;
    int a=anim_idx(e),l=anim_px(&lift[a]),size=SLOT_SIZE+2*l;
    int x=anim_px(&pos_x[a])-sc-l,y=anim_px(&pos_y[a])-l;
    if(x+size<=0 || x>=BW)continue;
    if(n==HOME_PER_PAGE)return -1;
    unsigned phase=(HL_IS_FOLDER(e)||apps[e].icon_animation)?galaxy_phase:0;
    out[n++]=(HomeTileStamp){e,x,y,size,phase};
  }
  return n;
}

static void draw_bottom_full(void) {
  volatile u8 *fb = VRAM_BOT_A;
  int sc = anim_px(&scroll), p0 = sc >= 0 ? sc / BW : -1;

  ui_wallpaper(fb, BW, BSH, BSH);
  draw_bar();

  for (int p = p0; p <= p0 + 1; p++) {
    if (p < 0 || p >= pages())
      continue;
    for (int s = 0; s < HOME_PER_PAGE; s++) {
      int k = p * HOME_PER_PAGE + s, x = slot_cx(k) - sc;
      if (x > -SLOT_SIZE && x < BW)
        draw_gradient_round_rect(fb, x, slot_cy(k), SLOT_SIZE, SLOT_SIZE, 10,
                                 BSH, g_theme.slot_empty_top,
                                 g_theme.slot_empty_bot);
    }
  }

  screen_touch(fb);
  if(held==HL_NONE && into<0 && sc==page*BW)
    for(unsigned i=0;i<sizeof bottom_base;i++)bottom_base[i]=fb[i];

  for (int k = 0; k < ndisp; k++) {
    int e = disp[k], a, l, x, y, size;
    if (e == GAP_ENTRY)
      continue;
    a = anim_idx(e);
    l = anim_px(&lift[a]);
    size = SLOT_SIZE + 2 * l;
    x = anim_px(&pos_x[a]) - sc - l;
    y = anim_px(&pos_y[a]) - l;
    if (x + size > 0 && x < BW)
      tile(fb, e, x, y, size);
    if (k == into)
      draw_round_ring(fb, x - SLOT_RING, y - SLOT_RING, size + 2 * SLOT_RING,
                      size + 2 * SLOT_RING, 10 + l / 2 + SLOT_RING,
                      SLOT_RING + 1, BSH, g_accent);
  }

  if (held == HL_NONE) {
    paint_ring(ring_rect());
  } else {
    /* over a tile it would go into, it shrinks so that tile shows round it */
    int size = SLOT_SIZE + 2 * HOLD_LIFT, x = anim_px(&held_x),
        y = anim_px(&held_y);
    if (into >= 0) {
      x += 2 * HOLD_LIFT;
      y += 2 * HOLD_LIFT;
      size -= 4 * HOLD_LIFT;
    }
    tile(fb, held, x, y, size);
    draw_round_ring(fb, x - SLOT_RING, y - SLOT_RING, size + 2 * SLOT_RING,
                    size + 2 * SLOT_RING, 13 + SLOT_RING, SLOT_RING + 1, BSH,
                    g_accent);
  }
  draw_dots(sc);
}

static void draw_bottom(void) {
  HomeTileStamp now[HOME_PER_PAGE];
  int count=bottom_stamps(now);
  int eligible=held==HL_NONE && into<0 && anim_px(&scroll)==page*BW && count>=0;
  HomeRect ring=ring_rect(),damage={0,0,0,0};
  int full=!bottom_valid || !eligible || bottom_view!=view || bottom_page!=page ||
    bottom_ndisp!=ndisp || bottom_count!=count;
  if(!full)for(int i=0;i<count;i++) {
    if(now[i].e!=bottom_tiles[i].e){full=1;break;}
    HomeRect a=stamp_rect(now[i]),b=stamp_rect(bottom_tiles[i]);
    if(!rect_equal(a,b) || now[i].phase!=bottom_tiles[i].phase)
      damage=rect_union(damage,rect_union(a,b));
  }
  if(!full && !rect_equal(ring,bottom_ring))damage=rect_union(damage,rect_union(ring,bottom_ring));
  if(!full && damage.w) {
    HomeRect prev;
    do {
      prev=damage;
      for(int i=0;i<count;i++)if(rect_hits(damage,stamp_rect(now[i])))damage=rect_union(damage,stamp_rect(now[i]));
      if(rect_hits(damage,ring))damage=rect_union(damage,ring);
    }while(!rect_equal(prev,damage));
    if(damage.y+damage.h>DOTS_Y)full=1;
  }
  if(full)draw_bottom_full();
  else if(damage.w) {
    int x0=damage.x<0?0:damage.x,y0=damage.y<0?0:damage.y;
    int x1=damage.x+damage.w>BW?BW:damage.x+damage.w;
    int y1=damage.y+damage.h>BSH?BSH:damage.y+damage.h;
    volatile u8 *fb=VRAM_BOT_A;screen_touch(fb);
    for(int x=x0;x<x1;x++) {
      unsigned off=(x*BSH+BSH-y1)*3;
      for(int i=0;i<(y1-y0)*3;i++)fb[off+i]=bottom_base[off+i];
    }
    for(int i=0;i<count;i++)if(rect_hits(damage,stamp_rect(now[i])))tile(fb,now[i].e,now[i].x,now[i].y,now[i].size);
    if(rect_hits(damage,ring))paint_ring(ring);
  }
  bottom_valid=eligible;bottom_view=view;bottom_page=page;bottom_ndisp=ndisp;
  bottom_count=count;bottom_ring=ring;
  if(count>=0)for(int i=0;i<count;i++)bottom_tiles[i]=now[i];
}

/* Points every animation at where things belong now; `jump` goes there at
 * once. */
static void place(int jump) {
  static u8 now[ANIM_N];
  void (*set)(AnimVal *, int) = jump ? anim_jump : anim_to;
  const int out = SLOT_RING + SLOT_LIFT;

  for (int i = 0; i < ANIM_N; i++)
    now[i] = 0;
  for (int k = 0; k < ndisp; k++) {
    int e = disp[k], a, want = 0;
    if (e == GAP_ENTRY)
      continue;
    a = anim_idx(e);
    now[a] = 1;
    if (jump || !shown[a]) {
      anim_jump(&pos_x[a], slot_cx(k));
      anim_jump(&pos_y[a], slot_cy(k));
      anim_jump(&lift[a], 0);
    } else {
      anim_to(&pos_x[a], slot_cx(k));
      anim_to(&pos_y[a], slot_cy(k));
    }
    if (held == HL_NONE && bar < 0 && k == cur())
      want = SLOT_LIFT;
    if (k == into)
      want = HOLD_LIFT;
    lift[a].bounce = 1;
    set(&lift[a], want);
  }
  for (int i = 0; i < ANIM_N; i++)
    shown[i] = now[i];

  ring_x.bounce = ring_y.bounce = 1;
  if (bar >= 0) {
    int x, y, w, h;
    btn_rect(bar, &x, &y, &w, &h);
    set(&ring_x, x - 2);
    set(&ring_y, y - 2);
    set(&ring_w, w + 4);
    set(&ring_h, h + 4);
  } else {
    set(&ring_x, GRID_X + (sel % HOME_COLS) * SLOT_STEP - out);
    set(&ring_y, GRID_Y + (sel / HOME_COLS) * SLOT_STEP - out);
    set(&ring_w, SLOT_SIZE + 2 * out);
    set(&ring_h, SLOT_SIZE + 2 * out);
  }
  if (held != HL_NONE && !by_touch && gap >= 0) {
    set(&held_x, GRID_X + (gap % HOME_COLS) * SLOT_STEP - HOLD_LIFT);
    set(&held_y, slot_cy(gap) - HOLD_LIFT);
  }
  set(&scroll, page * BW);
  dirty = 1;
}

#define PV_BOX    96
#define PV_BOX_X  ((TOP_SCREEN_WIDTH - PV_BOX) / 2)
#define PV_BOX_Y  34
#define PV_CARD_X 40
#define PV_CARD_Y 148
#define PV_CARD_W (TOP_SCREEN_WIDTH - 2 * PV_CARD_X)
#define PV_CARD_H 62
#define PV_HINT_Y (PV_CARD_Y + PV_CARD_H + 8)
#define PV_ALL_H  (PV_CARD_Y + PV_CARD_H - PV_BOX_Y)

/* Set while the top shows an app's own art, which spans the box, the card and
 * the gap between them. */
static int art_shown;

/* What the top screen describes: the carried entry, the bar button, or the
 * selected slot. */
static void top_item(void) {
  volatile u8 *fb = VRAM_TOP_LA;
  char devbuf[48];
  const char *name, *dev = "";
  Color tint, dark;
  int e = HL_NONE, folder = 0;
  u32 asset = UI_NO_ASSET, art = UI_NO_ASSET;
  const unsigned char *bits = 0;

  if (held != HL_NONE)
    e = held;
  else if (bar < 0)
    e = entry_at(cur());

  if (e >= 0 && asset_get(apps[e].asset_art))
    art = apps[e].asset_art;
  if (art_shown || art != UI_NO_ASSET)
    ui_wallpaper_rect(fb, PV_CARD_X, PV_BOX_Y, PV_CARD_W, PV_ALL_H, TSH);
  art_shown = art != UI_NO_ASSET;
  if (art_shown) {
    const Asset *a = asset_get(art);
    ui_wallpaper_rect(fb, 0, PV_HINT_Y, TOP_SCREEN_WIDTH, TSH - PV_HINT_Y,
                      TSH);
    draw_asset(fb, (TOP_SCREEN_WIDTH - a->w) / 2,
               PV_BOX_Y + (PV_ALL_H - a->h) / 2, TSH, a, COLOR_WHITE);
    if (held != HL_NONE)
      ui_text_mid(fb, TOP_SCREEN_WIDTH / 2, PV_HINT_Y, TSH,
                  L(by_touch ? STR_MOVE_TOUCH : STR_MOVE_KEYS),
                  COLOR_HM_TEXT2, COLOR_HM_BG_BOT, &ui_small);
    return;
  }

  if (held == HL_NONE && bar == BTN_POWER) {
    name = L(STR_POWER_OFF);
    dev = L(STR_SYSTEM);
    asset = ASSET_ICON_POWER_64;
    bits = icon_power_bits;
    tint = COLOR_DARK_RED;
  } else if (held == HL_NONE && bar == BTN_SHOP) {
    name = "aShop";
    dev = "Aurora";
    asset = ASSET_ICON_SHOP_BAR_64;
    bits = icon_boot_bits;
    tint = (Color){0x1E, 0x8C, 0x60};
  } else if (held == HL_NONE && bar == BTN_FILES) {
    name = "Files";
    dev = L(STR_SYSTEM);
    asset = ASSET_ICON_FOLDER_BAR_64;
    bits = icon_folder_bits;
    tint = (Color){0x2D, 0x5E, 0x7A};
  } else if (held == HL_NONE && bar == BTN_CAMERA) {
    name = "Camera";
    dev = "Aurora";
    asset = ASSET_APP_CAMERA_64;
    bits = icon_camera_bits;
    tint = (Color){0x2A, 0x62, 0x7A};
  } else if (held == HL_NONE && bar == BTN_MUSIC) {
    name = L(STR_MUSIC);
    dev = "Aurora";
    asset = ASSET_ICON_MUSIC_64;
    bits = icon_music_bits;
    tint = (Color){0x4E, 0x3A, 0x78};
  } else if (held == HL_NONE && bar == BTN_SETTINGS) {
    name = L(STR_SETTINGS);
    dev = L(STR_SYSTEM);
    asset = ASSET_ICON_SETTINGS_64;
    bits = icon_settings_bits;
    tint = settings_tint;
  } else if (held == HL_NONE && bar == BTN_BACK) {
    int p = hl_parent(view);
    name = p == HOME_ROOT ? L(STR_HOME) : hl_name(p);
    dev = L(STR_BACK);
    asset = p == HOME_ROOT ? ASSET_ICON_APPS_64 : ASSET_ICON_FOLDER_64;
    bits = icon_folder_bits;
    tint = folder_tint;
  } else if (e == HL_NONE) {
    name = L(STR_EMPTY_SLOT);
    tint = COLOR_HM_EMPTY_TOP;
  } else if (HL_IS_FOLDER(e)) {
    name = entry_name(e);
    dev = entry_dev(e, devbuf);
    folder = 1;
    tint = folder_tint;
  } else {
    name = apps[e].name;
    dev = apps[e].dev ? apps[e].dev : "";
    asset = apps[e].asset_lg;
    bits = apps[e].icon;
    tint = apps[e].tint;
  }
  dark.r = (u8)(tint.r * 5 / 9);
  dark.g = (u8)(tint.g * 5 / 9);
  dark.b = (u8)(tint.b * 5 / 9);

  ui_wallpaper_rect(fb, PV_BOX_X, PV_BOX_Y, PV_BOX, PV_BOX, TSH);
  ui_wallpaper_rect(fb, PV_CARD_X, PV_CARD_Y, PV_CARD_W, PV_CARD_H, TSH);
  ui_wallpaper_rect(fb, 0, PV_HINT_Y, TOP_SCREEN_WIDTH, TSH - PV_HINT_Y, TSH);
  draw_gradient_round_rect(fb, PV_BOX_X, PV_BOX_Y, PV_BOX, PV_BOX, 16, TSH,
                           tint, dark);
  if (folder) {
    int f = HL_FOLDER_ID(e), n = hl_count(f);
    if (!n)
      ui_icon(fb, PV_BOX_X, PV_BOX_Y, PV_BOX, TSH, ASSET_ICON_FOLDER_64,
              icon_folder_bits, COLOR_WHITE);
    for (int i = 0; i < 4 && i < n; i++)
      entry_icon32(fb, PV_BOX_X + 10 + (i % 2) * 44,
                   PV_BOX_Y + 10 + (i / 2) * 44, TSH, hl_at(f, i));
  } else if (bits || asset != UI_NO_ASSET) {
    if (!color_icon(fb, PV_BOX_X, PV_BOX_Y, PV_BOX, 16, TSH, e))
      ui_icon(fb, PV_BOX_X, PV_BOX_Y, PV_BOX, TSH, asset, bits, COLOR_WHITE);
  }

  draw_gradient_round_rect(fb, PV_CARD_X, PV_CARD_Y, PV_CARD_W, PV_CARD_H, 11,
                           TSH, g_theme.panel_top, g_theme.panel_bot);
  ui_text_mid_fit(fb, TOP_SCREEN_WIDTH / 2, PV_CARD_Y + 7, TSH, name,
                  PV_CARD_W - 24, g_theme.text, g_theme.panel_top, &ui_title);
  ui_text_mid_fit(fb, TOP_SCREEN_WIDTH / 2, PV_CARD_Y + 31, TSH, dev,
                  PV_CARD_W - 24, g_theme.text_dim, g_theme.panel_bot, &ui_font);
  if (held != HL_NONE)
    ui_text_mid(fb, TOP_SCREEN_WIDTH / 2, PV_HINT_Y, TSH,
                L(by_touch ? STR_MOVE_TOUCH : STR_MOVE_KEYS), COLOR_HM_TEXT2,
                COLOR_HM_BG_BOT, &ui_small);
}

static int top_icon_frame(void) {
  int e=held!=HL_NONE?held:bar<0?entry_at(cur()):HL_NONE;
  if(e<0 || HL_IS_FOLDER(e) || !apps[e].icon_animation || !apps[e].color_icon || art_shown)return 0;
  Color tint=apps[e].tint,dark={(u8)(tint.r*5/9),(u8)(tint.g*5/9),(u8)(tint.b*5/9)};
  ui_wallpaper_rect(VRAM_TOP_LA,PV_BOX_X,PV_BOX_Y,PV_BOX,PV_BOX,TSH);
  draw_gradient_round_rect(VRAM_TOP_LA,PV_BOX_X,PV_BOX_Y,PV_BOX,PV_BOX,16,TSH,tint,dark);
  color_icon(VRAM_TOP_LA,PV_BOX_X,PV_BOX_Y,PV_BOX,16,TSH,e);
  return 1;
}

enum { BN_IDLE, BN_WAIT, BN_SHOWN, BN_FAILED };
static int bn_state, bn_entry = HL_NONE, bn_loaded = HL_NONE, bn_bar_valid;
static int bn_on_screen;
static u32 bn_t0, bn_wait_t0;
static u8 bn_bar[TOP_SCREEN_WIDTH * STATUS_BAR_HEIGHT * 3];
#define BN_DEBOUNCE_MS 160u

static void dock_capture_thumb(const char *app_path);

static int focus_entry(void) {
  return held != HL_NONE ? held : bar < 0 ? entry_at(cur()) : HL_NONE;
}

static void banner_focus(void) {
  int e = focus_entry();
  if (bn_state == BN_SHOWN && e == bn_entry)
    return;
  if (bn_state == BN_WAIT && e == bn_entry)
    return;
  bn_entry = e;
  bn_wait_t0 = anim_now();
  bn_state = (e >= 0 && !HL_IS_FOLDER(e) && apps[e].banner && held == HL_NONE)
                 ? BN_WAIT : BN_IDLE;
}

static void banner_hide(void) {
  if (bn_state == BN_SHOWN)
    ui_sound_banner_stop();
  bn_state = BN_IDLE;
}

static void banner_leave_home(void) {
  banner_hide();
  banner_unload();
  bn_loaded = bn_entry = HL_NONE;
}

static void bar_strip(int save_it) {
  volatile u8 *fb = VRAM_TOP_LA;
  const u32 run = STATUS_BAR_HEIGHT * 3u;
  for (u32 x = 0; x < TOP_SCREEN_WIDTH; x++) {
    volatile u8 *col = fb + x * TSH * 3u + (TSH - STATUS_BAR_HEIGHT) * 3u;
    u8 *c = bn_bar + x * run;
    for (u32 i = 0; i < run; i++) {
      if (save_it)
        c[i] = col[i];
      else
        col[i] = c[i];
    }
  }
}

static int banner_frame(void) {
  u32 ms;
  if (bn_state == BN_WAIT) {
    int e = focus_entry();
    if (e != bn_entry || held != HL_NONE) {
      bn_state = BN_IDLE;
      return 0;
    }
    if (bn_loaded != e) {
      /* Short debounce so scrolling quickly across icons never hitches. */
      if ((u32)anim_ms(bn_wait_t0) < BN_DEBOUNCE_MS)
        return 0;
      banner_unload();
      bn_loaded = HL_NONE;
      if (!banner_load(apps[e].path, apps[e].banner)) {
        bn_state = BN_FAILED;
        ui_wallpaper(VRAM_TOP_LA, TOP_SCREEN_WIDTH, TSH, TSH);
        status_bar_draw();
        bn_on_screen = 0;
        top_item();
        return 1;
      }
      bn_loaded = e;
    }
    bn_state = BN_SHOWN;
    bn_t0 = anim_now();
    bn_bar_valid = 0;
    {
      u32 a, n, r;
      if (banner_jingle(&a, &n, &r))
        ui_sound_banner_play(a, n, r);
    }
  }
  if (bn_state != BN_SHOWN)
    return 0;
  ms = (u32)anim_ms(bn_t0);
  screen_touch(VRAM_TOP_LA);
  if (!banner_draw(ms, 256u, (u32)VRAM_TOP_LA)) {
    banner_hide();
    bn_state = BN_FAILED;
    ui_wallpaper(VRAM_TOP_LA, TOP_SCREEN_WIDTH, TSH, TSH);
    status_bar_draw();
    bn_on_screen = 0;
    top_item();
    return 1;
  }
  bn_on_screen = 1;
  if (!bn_bar_valid) {
    if (bn_entry >= 0 && !HL_IS_FOLDER(bn_entry) && apps[bn_entry].path)
      dock_capture_thumb(apps[bn_entry].path);
    status_bar_draw();
    bar_strip(1);
    bn_bar_valid = 1;
  } else {
    bar_strip(0);
  }
  return 1;
}

/* What the top screen showed last, so it changes only when that does. */
static int top_view, top_held, top_bar, top_e, top_k, top_touch;

static void top_note(void) {
  top_view = view;
  top_held = held;
  top_bar = bar;
  top_e = held != HL_NONE ? held : entry_at(cur());
  top_k = cur();
  top_touch = by_touch;
}

/* Cross-fades the top screen if what it describes changed. */
static void top_update(void) {
  int e = held != HL_NONE ? held : entry_at(cur()), dir, art_was = art_shown;
  int was_banner = (bn_state == BN_SHOWN) || bn_on_screen;
  int has_bn = (e >= 0 && !HL_IS_FOLDER(e) && apps[e].banner &&
                held == HL_NONE && bar < 0);
  if (top_view == view && top_held == held && top_bar == bar && top_e == e &&
      top_k == cur() && top_touch == by_touch)
    return;
  dir = cur() > top_k ? 1 : cur() < top_k ? -1 : 0;
  top_note();
  banner_hide();
  if (was_banner || has_bn) {
    /* Flush any in-flight top crossfade and wipe the full top screen immediately
     * so no still banner frame ever lingers behind the next slot, and nothing
     * is shown on a banner icon until its banner finishes loading. */
    anim_flush();
    ui_wallpaper(VRAM_TOP_LA, TOP_SCREEN_WIDTH, TSH, TSH);
    status_bar_draw();
    bn_on_screen = 0;
    art_shown = 0;
    if (!has_bn)
      top_item();
    screen_present_top();
    banner_focus();
    return;
  }
  anim_xfade_begin(ANIM_TOP);
  banner_focus();
  if (banner_frame()) {
    anim_xfade_area(ANIM_TOP, 0, 0, TOP_SCREEN_WIDTH, TSH);
    anim_xfade_start(ANIM_TOP, 0);
    screen_present_top();
    return;
  }
  top_item();
  if (art_was || art_shown) {
    anim_xfade_area(ANIM_TOP, PV_CARD_X, PV_BOX_Y, PV_CARD_W, PV_ALL_H);
  } else {
    anim_xfade_area(ANIM_TOP, PV_BOX_X, PV_BOX_Y, PV_BOX, PV_BOX);
    anim_xfade_area(ANIM_TOP, PV_CARD_X, PV_CARD_Y, PV_CARD_W, PV_CARD_H);
  }
  anim_xfade_area(ANIM_TOP, 0, PV_HINT_Y, TOP_SCREEN_WIDTH, TSH - PV_HINT_Y);
  anim_xfade_start(ANIM_TOP, dir);
  screen_present_top();
}

void home_draw(void) {
  bottom_valid=0;
  rebuild();
  ui_wallpaper(VRAM_TOP_LA, TOP_SCREEN_WIDTH, TSH, TSH);
  status_bar_draw();
  bn_on_screen = 0;
  banner_hide();
  anim_xfade_begin(ANIM_TOP);
  top_note();
  banner_focus();
  if (banner_frame()) {
    anim_xfade_area(ANIM_TOP, 0, 0, TOP_SCREEN_WIDTH, TSH);
    anim_xfade_start(ANIM_TOP, 0);
  } else if (bn_state != BN_WAIT) {
    top_item();
  }
  screen_present_top();
  place(1);
  draw_bottom();
  dirty = 0;
  screen_present_bottom();
}

static void animate(int ms) {
  int moved_any = dirty, show = 0;
  dirty = 0;
  moved_any |= anim_step(&scroll, ms);
  moved_any |= anim_step(&ring_x, ms);
  moved_any |= anim_step(&ring_y, ms);
  moved_any |= anim_step(&ring_w, ms);
  moved_any |= anim_step(&ring_h, ms);
  moved_any |= anim_step(&held_x, ms);
  moved_any |= anim_step(&held_y, ms);
  for (int k = 0; k < ndisp; k++) {
    int a;
    if (disp[k] == GAP_ENTRY)
      continue;
    a = anim_idx(disp[k]);
    moved_any |= anim_step(&pos_x[a], ms);
    moved_any |= anim_step(&pos_y[a], ms);
    moved_any |= anim_step(&lift[a], ms);
  }
  if (moved_any || galaxy_dirty) {
    draw_bottom();
    show |= ANIM_BOT;
  }
  if (anim_xfade_frame(ANIM_TOP))
    show |= ANIM_TOP;
  else if (banner_frame())
    show |= ANIM_TOP;
  else if (galaxy_dirty && top_icon_frame())
    show |= ANIM_TOP;
  galaxy_dirty=0;
  anim_present(show);
}

/* Runs the animations for `ms` without taking input. */
static void settle(int ms) {
  u32 t0 = anim_now(), frame_at = 0;
  while (anim_ms(t0) < ms) {
    int step = anim_frame(&frame_at);
    if (step)
      animate(step);
    ui_idle();
  }
}

static void refresh(void) {
  rebuild();
  place(0);
  top_update();
}

/* After a screen or dialog, which may return with the stylus still down. */
static void back_home(void) {
  int x, y;
  home_draw();
  tstate = touch_read(&x, &y, 0, 0) ? T_WAIT : T_IDLE;
}

static void save(void) { hl_save(keys, napps); }

static void message(int err) {
  StringId why = err == HL_TOO_DEEP    ? STR_ERR_DEEP
                 : err == HL_NO_FOLDERS ? STR_ERR_FOLDERS
                 : err == HL_INSIDE     ? STR_ERR_INSIDE
                                        : STR_ERR_FULL;
  ui_dialog(VRAM_BOT_A, BW, BSH, BSH, L(STR_CANT_MOVE), L(why), g_accent, 1);
  screen_present_bottom();
  for (;;) {
    u32 k = get_keys_down();
    int tx, ty;
    if ((k & (BUTTON_A | BUTTON_B)) || touch_tap(&tx, &ty)) {
      ui_sound_close(k);
      break;
    }
    ui_idle();
  }
  anim_transition(ANIM_FADE, ANIM_BOT);
}

static void select_k(int k) {
  if (k < 0)
    k = 0;
  page = k / HOME_PER_PAGE;
  sel = k % HOME_PER_PAGE;
}

/* While carrying: whether `held` may go into the entry at display `k`. */
static int can_go_into(int k) {
  int e = entry_at(k);
  if (e == HL_NONE)
    return 0;
  if (HL_IS_FOLDER(e))
    return hl_fits(HL_FOLDER_ID(e), held) == HL_OK;
  return hl_depth(view) + 1 + hl_height(held) <= HOME_DEPTH &&
         hl_folders_free() > 0;
}

/* Shows container `c`; while carrying, the gap goes to `at` if it fits. */
static void show_view(int c, int at, int push) {
  view = c;
  bar = -1;
  into = -1;
  if (held != HL_NONE) {
    gap = hl_fits(c, held) == HL_OK ? at : -1;
    if (gap > hl_count(c))
      gap = hl_count(c);
    if (gap >= 0)
      select_k(gap);
  }
  hov_kind = 0;
  anim_transition(push ? ANIM_PUSH : ANIM_POP, ANIM_BOTH);
  home_draw();
}

static void open_folder(int f) {
  page = 0;
  sel = 0;
  show_view(f, hl_count(f), 1);
}

static void go_up(void) {
  int f = view, p = hl_parent(f), at = hl_index(p, HL_FOLDER(f));
  ui_sound_back();
  select_k(at);
  show_view(p, at + 1, 0);
}

static void start_carry(int k, int touch) {
  int e = entry_at(k), a, l;
  if (e == HL_NONE)
    return;
  a = anim_idx(e);
  l = anim_px(&lift[a]);
  held_c = view;
  held_i = list_index(k);
  held = hl_take(view, held_i);
  gap = held_i;
  into = -1;
  by_touch = touch;
  anim_jump(&held_x, anim_px(&pos_x[a]) - anim_px(&scroll) - l);
  anim_jump(&held_y, anim_px(&pos_y[a]) - l);
  held_x.bounce = held_y.bounce = 1;
  anim_to(&held_x, anim_px(&held_x) - (HOLD_LIFT - l));
  anim_to(&held_y, anim_px(&held_y) - (HOLD_LIFT - l));
  hov_kind = 0;
  refresh();
}

static void end_carry(int e) {
  int k;
  held = HL_NONE;
  gap = -1;
  into = -1;
  rebuild();
  k = hl_index(view, e);
  if (k >= 0) {
    int a = anim_idx(e);
    select_k(k);
    /* from where it was let go */
    anim_jump(&pos_x[a], anim_px(&held_x) + anim_px(&scroll) + HOLD_LIFT);
    anim_jump(&pos_y[a], anim_px(&held_y) + HOLD_LIFT);
    anim_jump(&lift[a], HOLD_LIFT);
    shown[a] = 1;
  }
  place(0);
  top_update();
}

static void cancel_carry(void) {
  int e = held;
  hl_put(held_c, held_i, e);
  end_carry(e);
}

static void drop(void) {
  int e = held, r, shown_e = e;
  if (hov_kind == 1 && view != HOME_ROOT) {
    /* Dropped directly on the top bar inside a folder: move out to parent! */
    int p = hl_parent(view);
    int at = hl_index(p, HL_FOLDER(view));
    r = hl_put(p, at >= 0 ? at + 1 : hl_count(p), e);
    if (r >= 0) {
      save();
      held = HL_NONE;
      gap = into = -1;
      hov_kind = 0;
      ui_sound_back();
      view = p;
      bar = -1;
      rebuild();
      int k = hl_index(view, e);
      if (k >= 0)
        select_k(k);
      anim_transition(ANIM_POP, ANIM_BOTH);
      home_draw();
      return;
    }
  }
  if (into >= 0) {
    int t = disp[into];
    if (HL_IS_FOLDER(t)) {
      r = hl_put(HL_FOLDER_ID(t), hl_count(HL_FOLDER_ID(t)), e);
    } else {
      r = hl_merge(view, list_index(into), e, L(STR_FOLDER));
      if (r >= 0)
        t = HL_FOLDER(r);
    }
    shown_e = t;
  } else if (gap >= 0) {
    r = hl_put(view, gap, e);
  } else {
    r = hl_fits(view, e);
    if (r == HL_OK)
      r = HL_FULL;
  }
  if (r < 0) {
    hl_put(held_c, held_i, e);
    held = HL_NONE;
    gap = into = -1;
    rebuild();
    message(r);
    back_home();
    return;
  }
  save();
  end_carry(shown_e);
}

/* The gap, or the tile to go into, under the stylus at (x, y). */
static void target_at(int x, int y) {
  int col = (x - GRID_X + SLOT_GAP / 2) / SLOT_STEP;
  int row = (y - GRID_Y + SLOT_GAP / 2) / SLOT_STEP;
  int k, lx, n = hl_count(view), ngap = gap, ninto = -1;

  if (x < GRID_X - SLOT_GAP / 2)
    col = 0;
  if (y < GRID_Y - SLOT_GAP / 2)
    row = 0;
  if (col > HOME_COLS - 1)
    col = HOME_COLS - 1;
  if (row > HOME_ROWS - 1)
    row = HOME_ROWS - 1;
  k = page * HOME_PER_PAGE + row * HOME_COLS + col;
  lx = x - (GRID_X + col * SLOT_STEP);

  if (k >= ndisp) {
    ngap = n;
  } else if (disp[k] != GAP_ENTRY) {
    const int margin = SLOT_SIZE / 5;
    int li = list_index(k);
    if (lx > margin && lx < SLOT_SIZE - margin && can_go_into(k))
      ninto = k;
    else
      ngap = lx < SLOT_SIZE / 2 ? li : li + 1;
  }
  if (gap < 0)
    ngap = -1;
  if (ninto == into && ngap == gap)
    return;
  into = ninto;
  if (into < 0)
    gap = ngap;
  rebuild();
  place(0);
}

static void drag_to(int x, int y) {
  int kind = 0, arg = 0;

  anim_jump(&held_x, x - grab_dx);
  anim_jump(&held_y, y - grab_dy);
  dirty = 1;
  if (!moved && (iabs(x - t_x0) > SLOP || iabs(y - t_y0) > SLOP))
    moved = 1;
  if (!moved)
    return;

  if (y < BAR_Y + BAR_H + 8) {
    if (view != HOME_ROOT)
      kind = 1;
  } else if (x < 14) {
    kind = 2;
  } else if (x > BW - 14) {
    kind = 3;
  } else {
    target_at(x, y);
    if (into >= 0 && HL_IS_FOLDER(disp[into])) {
      kind = 4;
      arg = disp[into];
    }
  }

  if (kind != hov_kind || arg != hov_arg) {
    hov_kind = kind;
    hov_arg = arg;
    hov_t0 = anim_now();
    return;
  }
  if (kind == 1 && anim_ms(hov_t0) >= BACK_MS) {
    go_up();
  } else if (kind == 2 && anim_ms(hov_t0) >= EDGE_MS && page > 0) {
    page--;
    hov_t0 = anim_now();
    ui_sound_move();
    place(0);
  } else if (kind == 3 && anim_ms(hov_t0) >= EDGE_MS && page + 1 < pages()) {
    page++;
    hov_t0 = anim_now();
    ui_sound_move();
    place(0);
  } else if (kind == 4 && anim_ms(hov_t0) >= SPRING_MS) {
    ui_sound_select();
    open_folder(HL_FOLDER_ID(arg));
  }
}

static void carry_keys(u32 k) {
  if (gap >= 0) {
    int g = gap, n = hl_count(view);
    if (k & BUTTON_DLEFT)
      g--;
    if (k & BUTTON_DRIGHT)
      g++;
    if (k & BUTTON_DUP)
      g -= HOME_COLS;
    if (k & BUTTON_DDOWN)
      g += HOME_COLS;
    if (k & BUTTON_L)
      g -= HOME_PER_PAGE;
    if (k & BUTTON_R)
      g += HOME_PER_PAGE;
    if (g < 0)
      g = 0;
    if (g > n)
      g = n;
    if (g != gap) {
      ui_sound_move();
      gap = g;
      select_k(gap);
      refresh();
    }
  }
  if (k & BUTTON_A) {
    ui_sound_select();
    drop();
  } else if (k & BUTTON_B) {
    ui_sound_back();
    cancel_carry();
  }
}

/* ---- Pinned Apps Dock (up to 3 apps, saved in SD:/Aurora/pinned.txt) ---- */
#define DOCK_FILE "Aurora/pinned.txt"

static int dock_streq(const char *a, const char *b) {
  while (*a && *a == *b) { a++; b++; }
  return *a == *b;
}

static void dock_cpy(char *dst, const char *src, int max) {
  int i = 0;
  while (src[i] && i + 1 < max) { dst[i] = src[i]; i++; }
  dst[i] = '\0';
}

static int dock_find(const char *path) {
  if (!path || !path[0])
    return -1;
  volatile AuroraSystemMenuShared *s = aurora_system_menu_shared();
  u32 n = s->dock_count;
  if (n > AURORA_DOCK_MAX) n = AURORA_DOCK_MAX;
  for (u32 i = 0; i < n; i++)
    if (dock_streq((const char *)s->dock[i].path, path))
      return (int)i;
  return -1;
}

extern void os_cache_sync(void);

static void dock_save(void) {
  static FATFS fs;
  static FIL f;
  static char buf[AURORA_DOCK_MAX * 168 + 16];
  volatile AuroraSystemMenuShared *s = aurora_system_menu_shared();
  u32 n = s->dock_count;
  if (n > AURORA_DOCK_MAX) n = AURORA_DOCK_MAX;
  char *p = buf;
  for (u32 i = 0; i < n; i++) {
    const char *nm = (const char *)s->dock[i].name;
    const char *pt = (const char *)s->dock[i].path;
    while (*nm) *p++ = *nm++;
    *p++ = '|';
    while (*pt) *p++ = *pt++;
    *p++ = '\r';
    *p++ = '\n';
  }
  UINT len = (UINT)(p - buf), bw = 0;
  if (f_mount(&fs, "", 1) == FR_OK) {
    f_mkdir("Aurora");
    if (f_open(&f, DOCK_FILE, FA_CREATE_ALWAYS | FA_WRITE) == FR_OK) {
      if (len) f_write(&f, buf, len, &bw);
      f_close(&f);
    }
    f_mount(NULL, "", 0);
  }
  os_cache_sync();
}

static void dock_fill_slot(volatile AuroraDockApp *d, const HomeApp *app) {
  d->tint_rgb = ((u32)app->tint.r << 16) | ((u32)app->tint.g << 8) | app->tint.b;
  d->has_banner = app->banner ? 1u : 0u;
  if (app->color_icon) {
    for (u32 i = 0; i < AURORA_DOCK_ICON_W * AURORA_DOCK_ICON_H; i++)
      d->icon[i] = app->color_icon[i];
    d->has_icon = 1u;
  } else if (app->icon) {
    /* Rasterize legacy 1-bit 32x32 .icon bitmap into RGB565 for the dock. */
    Color bg_top = g_theme.slot_top;
    Color bg_bot = g_theme.slot_bot;
    for (u32 y = 0; y < AURORA_DOCK_ICON_H; y++) {
      u32 r = bg_top.r + ((int)(bg_bot.r - bg_top.r) * (int)y) / 31;
      u32 g = bg_top.g + ((int)(bg_bot.g - bg_top.g) * (int)y) / 31;
      u32 b = bg_top.b + ((int)(bg_bot.b - bg_top.b) * (int)y) / 31;
      uint16_t bg565 = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
      const u8 *row = app->icon + y * 4u;
      for (u32 x = 0; x < AURORA_DOCK_ICON_W; x++) {
        int bit = (row[x >> 3] & (u8)(0x80u >> (x & 7u))) != 0;
        d->icon[y * AURORA_DOCK_ICON_W + x] = bit ? 0xFFFFu : bg565;
      }
    }
    d->has_icon = 1u;
  } else {
    d->has_icon = 0u;
  }
}

static void dock_sync_meta(void) {
  volatile AuroraSystemMenuShared *s = aurora_system_menu_shared();
  u32 n = s->dock_count;
  if (n > AURORA_DOCK_MAX) n = AURORA_DOCK_MAX;
  for (u32 i = 0; i < n; i++) {
    const char *p = (const char *)s->dock[i].path;
    for (int a = 0; a < napps; a++) {
      if (apps[a].path && dock_streq(apps[a].path, p)) {
        dock_fill_slot(&s->dock[i], &apps[a]);
        break;
      }
    }
  }
  os_cache_sync();
}

static void dock_capture_thumb(const char *app_path) {
  int idx = dock_find(app_path);
  if (idx < 0)
    return;
  volatile AuroraSystemMenuShared *s = aurora_system_menu_shared();
  volatile u8 *fb = VRAM_TOP_LA;
  volatile uint16_t *dst = s->dock[idx].thumb;
  for (u32 ty = 0; ty < AURORA_DOCK_THUMB_H; ty++) {
    u32 sy = ty * (TOP_SCREEN_HEIGHT / AURORA_DOCK_THUMB_H) + 5u;
    if (sy >= TOP_SCREEN_HEIGHT) sy = TOP_SCREEN_HEIGHT - 1u;
    u32 row_off = (TOP_SCREEN_HEIGHT - 1u - sy) * 3u;
    for (u32 tx = 0; tx < AURORA_DOCK_THUMB_W; tx++) {
      u32 sx = tx * (TOP_SCREEN_WIDTH / AURORA_DOCK_THUMB_W) + 5u;
      if (sx >= TOP_SCREEN_WIDTH) sx = TOP_SCREEN_WIDTH - 1u;
      const volatile u8 *p = fb + sx * TSH * 3u + row_off;
      u32 b = p[0], g = p[1], r = p[2];
      dst[ty * AURORA_DOCK_THUMB_W + tx] =
          (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    }
  }
  s->dock[idx].has_thumb = 1u;
  os_cache_sync();
}

static void dock_load(void) {
  static FATFS fs;
  static FIL f;
  static char buf[AURORA_DOCK_MAX * 168 + 32];
  volatile AuroraSystemMenuShared *s = aurora_system_menu_shared();
  UINT br = 0;
  s->dock_count = 0;
  for (u32 i = 0; i < AURORA_DOCK_MAX; i++) {
    s->dock[i].name[0] = '\0';
    s->dock[i].path[0] = '\0';
    s->dock[i].has_icon = 0u;
    s->dock[i].has_banner = 0u;
    s->dock[i].has_thumb = 0u;
    s->dock[i].tint_rgb = 0u;
  }
  if (f_mount(&fs, "", 1) == FR_OK) {
    if (f_open(&f, DOCK_FILE, FA_READ) == FR_OK) {
      if (f_read(&f, buf, sizeof(buf) - 1, &br) != FR_OK)
        br = 0;
      f_close(&f);
    }
    f_mount(NULL, "", 0);
  }
  buf[br] = '\0';
  for (char *l = buf; *l && s->dock_count < AURORA_DOCK_MAX;) {
    char *e = l;
    while (*e && *e != '\r' && *e != '\n')
      e++;
    int more = *e != '\0';
    *e = '\0';
    char *sep = l;
    while (*sep && *sep != '|')
      sep++;
    if (*sep == '|' && sep > l && sep[1]) {
      *sep = '\0';
      u32 idx = s->dock_count;
      dock_cpy((char *)s->dock[idx].name, l, (int)sizeof(s->dock[idx].name));
      dock_cpy((char *)s->dock[idx].path, sep + 1, (int)sizeof(s->dock[idx].path));
      s->dock_count = idx + 1u;
    }
    l = more ? e + 1 : e;
  }
  dock_sync_meta();
}

static void dock_toggle(const HomeApp *app) {
  if (!app || !app->path || !app->path[0])
    return;
  volatile AuroraSystemMenuShared *s = aurora_system_menu_shared();
  int idx = dock_find(app->path);
  if (idx >= 0) {
    u32 n = s->dock_count;
    if (n > AURORA_DOCK_MAX) n = AURORA_DOCK_MAX;
    for (u32 i = (u32)idx; i + 1u < n; i++)
      s->dock[i] = s->dock[i + 1u];
    if (n > 0) {
      s->dock[n - 1u].name[0] = '\0';
      s->dock[n - 1u].path[0] = '\0';
      s->dock[n - 1u].has_icon = 0u;
      s->dock[n - 1u].has_banner = 0u;
      s->dock[n - 1u].has_thumb = 0u;
      s->dock_count = n - 1u;
    }
    dock_save();
  } else if (s->dock_count < AURORA_DOCK_MAX) {
    u32 n = s->dock_count;
    dock_cpy((char *)s->dock[n].name, app->name ? app->name : "App",
             (int)sizeof(s->dock[n].name));
    dock_cpy((char *)s->dock[n].path, app->path, (int)sizeof(s->dock[n].path));
    s->dock[n].has_thumb = 0u;
    dock_fill_slot(&s->dock[n], app);
    s->dock_count = n + 1u;
    dock_save();
  }
}

enum { MI_MOVE, MI_MOVE_TO, MI_NEW, MI_RENAME, MI_REMOVE, MI_DELETE_APP,
       MI_PIN, MI_COUNT };
static const StringId mi_label[MI_COUNT] = {STR_MOVE, STR_MOVE_TO,
                                            STR_NEW_FOLDER, STR_RENAME,
                                            STR_REMOVE_FOLDER,
                                            STR_DELETE_APP, STR_MOVE};

#define MN_X     12
#define MN_Y     10
#define MN_W     (BW - 24)
#define MN_H     (BSH - 20)
#define MN_BW    132
#define MN_BH    34
#define MN_BX(i) (MN_X + 12 + ((i) % 2) * (MN_BW + 8))
#define MN_BY(i) (MN_Y + 56 + ((i) / 2) * (MN_BH + 6))

static void menu_card(const char *title, const char *sub) {
  static const Color scrim = {0x00, 0x00, 0x00};
  volatile u8 *fb = VRAM_BOT_A;
  bottom_valid=0;
  draw_bottom();
  bottom_valid=0;
  draw_filled_rect_alpha(fb, 0, 0, BW, BSH, BSH, scrim, 150);
  anim_popup_behind();
  draw_gradient_round_rect(fb, MN_X, MN_Y, MN_W, MN_H, 14, BSH,
                           COLOR_PANEL_TOP, COLOR_PANEL_BOT);
  ui_text_mid_fit(fb, BW / 2, MN_Y + 12, BSH, title, MN_W - 28, COLOR_WHITE,
                  COLOR_PANEL_TOP, &ui_bold);
  ui_text_mid_fit(fb, BW / 2, MN_Y + 36, BSH, sub, MN_W - 28, COLOR_HM_TEXT2,
                  COLOR_PANEL_TOP, &ui_small);
}

static void menu_draw(int e, int item, const u8 *on) {
  volatile u8 *fb = VRAM_BOT_A;
  char devbuf[48];
  menu_card(entry_name(e), entry_dev(e, devbuf));
  for (int i = 0; i < MI_COUNT; i++) {
    int x = MN_BX(i), y = MN_BY(i);
    if (i == item)
      draw_filled_round_rect(fb, x - 3, y - 3, MN_BW + 6, MN_BH + 6, 12, BSH,
                             g_accent);
    if (on[i])
      draw_gradient_round_rect(fb, x, y, MN_BW, MN_BH, 10, BSH,
                               COLOR_HM_SLOT_TOP, COLOR_HM_SLOT_BOT);
    else
      draw_gradient_round_rect(fb, x, y, MN_BW, MN_BH, 10, BSH,
                               COLOR_HM_EMPTY_TOP, COLOR_HM_EMPTY_BOT);
    const char *lbl = L(mi_label[i]);
    if (i == MI_PIN) {
      if (e != HL_NONE && !HL_IS_FOLDER(e) && apps[e].path) {
        if (dock_find(apps[e].path) >= 0)
          lbl = "Unpin";
        else if (aurora_system_menu_shared()->dock_count >= AURORA_DOCK_MAX)
          lbl = "Dock Full (3/3)";
        else
          lbl = "Pin to Dock";
      } else {
        lbl = "Pin to Dock";
      }
    }
    ui_text_mid_fit(fb, x + MN_BW / 2, y + (MN_BH - ui_th(&ui_bold)) / 2, BSH,
                    lbl, MN_BW - 12,
                    on[i] ? COLOR_WHITE : COLOR_HM_TEXT2, COLOR_HM_SLOT,
                    &ui_bold);
  }
  screen_present_bottom();
}

/* Containers `e` could move to, other than the one it is in. */
static int dests(int e, int *out, int max) {
  int n = 0;
  if (view != HOME_ROOT && hl_fits(HOME_ROOT, e) == HL_OK && n < max)
    out[n++] = HOME_ROOT;
  for (int i = 0; i < hl_count(HOME_ROOT); i++) {
    int a = hl_at(HOME_ROOT, i), f;
    if (!HL_IS_FOLDER(a))
      continue;
    f = HL_FOLDER_ID(a);
    if (f != view && hl_fits(f, e) == HL_OK && n < max)
      out[n++] = f;
    for (int j = 0; j < hl_count(f); j++) {
      int b = hl_at(f, j), g;
      if (!HL_IS_FOLDER(b))
        continue;
      g = HL_FOLDER_ID(b);
      if (g != view && hl_fits(g, e) == HL_OK && n < max)
        out[n++] = g;
    }
  }
  return n;
}

/* The chosen MI_*, or -1. */
static int menu_run(int e) {
  static int to[HOME_FOLDERS + 1];
  u8 on[MI_COUNT];
  int item;

  on[MI_MOVE] = (u8)(e != HL_NONE);
  on[MI_MOVE_TO] = (u8)(e != HL_NONE && dests(e, to, HOME_FOLDERS + 1) > 0);
  if (e == HL_NONE)
    on[MI_NEW] = (u8)(hl_depth(view) < HOME_DEPTH &&
                      hl_count(view) < HOME_ITEMS && hl_folders_free() > 0);
  else
    on[MI_NEW] = (u8)(hl_depth(view) + 1 + hl_height(e) <= HOME_DEPTH &&
                      hl_folders_free() > 0);
  on[MI_RENAME] = on[MI_REMOVE] = (u8)HL_IS_FOLDER(e);
  on[MI_DELETE_APP] = (u8)(e != HL_NONE && !HL_IS_FOLDER(e) &&
                            apps[e].path != 0 && hk->remove_app != 0);
  if (e != HL_NONE && !HL_IS_FOLDER(e) && apps[e].path != 0) {
    int pinned = dock_find(apps[e].path) >= 0;
    on[MI_PIN] = (u8)(pinned ||
                      aurora_system_menu_shared()->dock_count < AURORA_DOCK_MAX);
  } else {
    on[MI_PIN] = 0;
  }
  item = 0;
  while (item < MI_COUNT && !on[item])
    item++;
  if (item == MI_COUNT)
    item = 0;

  anim_popup(MN_X, MN_Y, MN_W, MN_H);
  menu_draw(e, item, on);
  for (;;) {
    u32 kd = get_keys_down();
    int prev = item, tx, ty;
    if ((kd & BUTTON_DLEFT) && (item & 1))
      item--;
    if ((kd & BUTTON_DRIGHT) && !(item & 1) && item + 1 < MI_COUNT)
      item++;
    if ((kd & BUTTON_DUP) && item >= 2)
      item -= 2;
    if ((kd & BUTTON_DDOWN) && item + 2 < MI_COUNT)
      item += 2;
    if (touch_tap(&tx, &ty)) {
      if (!touch_in(tx, ty, MN_X, MN_Y, MN_W, MN_H))
        return -1;
      for (int i = 0; i < MI_COUNT; i++)
        if (on[i] && touch_in(tx, ty, MN_BX(i), MN_BY(i), MN_BW, MN_BH))
          return i;
    }
    if ((kd & BUTTON_A) && on[item])
      return item;
    if (kd & (BUTTON_B | BUTTON_Y))
      return -1;
    if (item != prev) {
      ui_sound_move();
      menu_draw(e, item, on);
    }
    ui_idle();
  }
}

#define PK_ROWS 6
#define PK_ROW  26
#define PK_Y0   (MN_Y + 58)

static void pick_draw(int e, const int *to, int n, int item, int top) {
  volatile u8 *fb = VRAM_BOT_A;
  menu_card(L(STR_MOVE_TO), entry_name(e));
  for (int r = 0; r < PK_ROWS && top + r < n; r++) {
    int c = to[top + r], y = PK_Y0 + r * PK_ROW, x = MN_X + 16;
    if (c != HOME_ROOT && hl_parent(c) != HOME_ROOT)
      x += 20;
    if (top + r == item)
      draw_filled_round_rect(fb, MN_X + 8, y, MN_W - 16, PK_ROW - 2, 8, BSH,
                             g_accent);
    icon16(fb, x, y + (PK_ROW - 2 - 16) / 2, BSH,
           c == HOME_ROOT ? ASSET_ICON_APPS_16 : ASSET_ICON_FOLDER_16,
           icon_folder_bits, top + r == item ? COLOR_HM_BAR : COLOR_WHITE);
    ui_text_fit(fb, x + 22, y + (PK_ROW - 2 - ui_th(&ui_font)) / 2, BSH,
                c == HOME_ROOT ? L(STR_HOME) : hl_name(c),
                MN_X + MN_W - 16 - (x + 22),
                top + r == item ? COLOR_HM_BAR : COLOR_WHITE,
                top + r == item ? g_accent : COLOR_PANEL_BOT, &ui_font);
  }
  screen_present_bottom();
}

/* A container for `e`, or -2. */
static int pick_run(int e) {
  static int to[HOME_FOLDERS + 1];
  int n = dests(e, to, HOME_FOLDERS + 1), item = 0, top = 0;
  if (!n)
    return -2;
  anim_popup(MN_X, MN_Y, MN_W, MN_H);
  pick_draw(e, to, n, item, top);
  for (;;) {
    u32 kd = get_keys_down();
    int prev = item, tx, ty;
    if ((kd & BUTTON_DUP) && item > 0)
      item--;
    if ((kd & BUTTON_DDOWN) && item + 1 < n)
      item++;
    if (item < top)
      top = item;
    if (item >= top + PK_ROWS)
      top = item - PK_ROWS + 1;
    if (touch_tap(&tx, &ty)) {
      if (!touch_in(tx, ty, MN_X, MN_Y, MN_W, MN_H))
        return -2;
      for (int r = 0; r < PK_ROWS && top + r < n; r++)
        if (touch_in(tx, ty, MN_X + 8, PK_Y0 + r * PK_ROW, MN_W - 16, PK_ROW))
          return to[top + r];
    }
    if (kd & BUTTON_A)
      return to[item];
    if (kd & BUTTON_B)
      return -2;
    if (item != prev) {
      ui_sound_move();
      pick_draw(e, to, n, item, top);
    }
    ui_idle();
  }
}

static void name_top(const char *title) {
  volatile u8 *fb = VRAM_TOP_LA;
  ui_wallpaper(fb, TOP_SCREEN_WIDTH, TSH, TSH);
  status_bar_draw();
  draw_gradient_round_rect(fb, PV_BOX_X, PV_BOX_Y, PV_BOX, PV_BOX, 16, TSH,
                           folder_tint, folder_bot);
  ui_icon(fb, PV_BOX_X, PV_BOX_Y, PV_BOX, TSH, ASSET_ICON_FOLDER_64,
          icon_folder_bits, COLOR_WHITE);
  draw_gradient_round_rect(fb, PV_CARD_X, PV_CARD_Y, PV_CARD_W, PV_CARD_H, 11,
                           TSH, COLOR_PANEL_TOP, COLOR_PANEL_BOT);
  ui_text_mid(fb, TOP_SCREEN_WIDTH / 2, PV_CARD_Y + 7, TSH, title,
              COLOR_WHITE, COLOR_PANEL_TOP, &ui_title);
  ui_text_mid(fb, TOP_SCREEN_WIDTH / 2, PV_CARD_Y + 31, TSH,
              L(STR_FOLDER_NAME), COLOR_HM_TEXT2, COLOR_PANEL_BOT, &ui_font);
  ui_text_mid(fb, TOP_SCREEN_WIDTH / 2, PV_HINT_Y, TSH,
              "B: Delete   L: Caps   START: Done   SELECT: Cancel",
              COLOR_HM_TEXT2, COLOR_HM_BG_BOT, &ui_small);
  screen_present_top();
}

/* 1 with a name in `buf`, which starts as `start`. */
static int ask_name(const char *title, const char *start, char *buf) {
  int ok, i = 0;
  while (start[i] && i < HOME_NAME - 1) {
    buf[i] = start[i];
    i++;
  }
  buf[i] = 0;
  anim_transition(ANIM_PUSH, ANIM_BOTH);
  name_top(title);
  ok = keyboard_edit(buf, HOME_NAME, KB_NAME, L(STR_FOLDER_NAME), g_accent);
  anim_transition(ANIM_POP, ANIM_BOTH);
  /* keyboard_edit returns on a key press; let it go before the menu reacts */
  while (get_keys() & (BUTTON_B | BUTTON_A | BUTTON_START | BUTTON_SELECT))
    ui_idle();
  get_keys_down();
  return ok && buf[0];
}

static void act(int item, int k) {
  static char name[HOME_NAME];
  int e = entry_at(k), r = HL_OK;

  if (item == MI_MOVE) {
    start_carry(k, 0);
    back_home();
    return;
  }
  if (item == MI_MOVE_TO) {
    int c = pick_run(e);
    if (c == -2)
      ui_sound_back();
    else
      ui_sound_select();
    anim_transition(ANIM_FADE, ANIM_BOT);
    if (c != -2) {
      int i = hl_index(view, e), got = hl_take(view, i);
      r = hl_put(c, hl_count(c), got);
      if (r != HL_OK)
        hl_put(view, i, got);
      else
        save();
    }
  } else if (item == MI_NEW) {
    if (ask_name(L(STR_NEW_FOLDER), L(STR_FOLDER), name)) {
      if (e == HL_NONE)
        r = hl_new_folder(view, hl_count(view), name);
      else
        r = hl_wrap(view, list_index(k), name);
      if (r >= 0) {
        save();
        rebuild();
        select_k(hl_index(view, HL_FOLDER(r)));
        r = HL_OK;
      }
    }
  } else if (item == MI_RENAME) {
    if (ask_name(L(STR_RENAME), hl_name(HL_FOLDER_ID(e)), name)) {
      hl_rename(HL_FOLDER_ID(e), name);
      save();
    }
  } else if (item == MI_REMOVE) {
    int f = HL_FOLDER_ID(e);
    if (!hl_count(f) || fv_confirm(L(STR_REMOVE_ASK), L(STR_REMOVE_HINT),
                                   L(STR_REMOVE_FOLDER), L(STR_CANCEL))) {
      r = hl_unfold(f);
      if (r == HL_OK)
        save();
    }
  } else if (item == MI_DELETE_APP && e != HL_NONE && !HL_IS_FOLDER(e) &&
             hk->remove_app) {
    banner_leave_home();
    hk->remove_app(&apps[e]);
  } else if (item == MI_PIN && e != HL_NONE && !HL_IS_FOLDER(e) &&
             apps[e].path) {
    dock_toggle(&apps[e]);
  }
  back_home();
  if (r < 0) {
    message(r);
    back_home();
  }
}

static void menu(int k) {
  int item;
  ui_sound_select();
  item = menu_run(entry_at(k));
  if (item < 0)
    ui_sound_back();
  else
    ui_sound_select();
  anim_transition(ANIM_FADE, ANIM_BOT);
  if (item < 0)
    back_home();
  else
    act(item, k);
}

static void press(int k) {
  int e = entry_at(k);
  if (e == HL_NONE)
    return;
  anim_to(&lift[anim_idx(e)], -2);
  settle(110);
}

static void activate(int k) {
  int e = entry_at(k);
  if (e == HL_NONE)
    return;
  ui_sound_select();
  press(k);
  if (HL_IS_FOLDER(e)) {
    open_folder(HL_FOLDER_ID(e));
  } else {
    banner_leave_home();
    hk->open(&apps[e]);
    back_home();
  }
}

static void power_ask(void) {
  if (fv_confirm(L(STR_POWER_ASK), "", L(STR_POWER_OFF), L(STR_CANCEL))) {
    banner_leave_home();
    hk->power_off();
  }
  back_home();
}

enum {
  SM_PLUGIN = 0,
  SM_HOME,
  SM_BRIGHTNESS,
  SM_RELOAD_PLUGINS,
  SM_POWER_OFF,
};

typedef struct {
  int kind;
  int slot_idx;
} SmItem;

static int navigation_requested;
static int home_started;

int home_navigation_requested(void) { return navigation_requested; }

void home_navigation_clear(void) { navigation_requested = 0; }

#define SM_SIDEBAR_W   AMENU_SIDEBAR_W
#define SM_HEADER_H    38
#define SM_LIST_Y      44
#define SM_ROW_H       22
#define SM_VISIBLE     7
#define SM_FOOTER_Y    208

static u32 sm_saved_bottom[BOT_FB_SIZE / 4];
static int sm_slide_x;
static int sm_close_req;

static Color sm_rgb(uint32_t rgb) {
  Color c;
  c.r = (u8)((rgb >> 16) & 0xFFu);
  c.g = (u8)((rgb >> 8) & 0xFFu);
  c.b = (u8)(rgb & 0xFFu);
  return c;
}

static uint32_t sm_accent_rgb(void) {
  return ((uint32_t)g_accent.r << 16) | ((uint32_t)g_accent.g << 8) | g_accent.b;
}

static Color sm_darken(Color c, int num, int den) {
  Color o;
  o.r = (u8)((int)c.r * num / den);
  o.g = (u8)((int)c.g * num / den);
  o.b = (u8)((int)c.b * num / den);
  return o;
}

static void sm_sidebar_clear(void) {
  volatile u8 *dst = VRAM_BOT_A;
  const u8 *src = (const u8 *)sm_saved_bottom;
  int sx = sm_slide_x;
  int right_edge = sx + SM_SIDEBAR_W;
  int vis = right_edge > 0 ? (right_edge * 256) / SM_SIDEBAR_W : 0;
  if (vis > 256) vis = 256;
  u32 dim = (u32)(256 - (124 * vis) / 256);

  Color pt = sm_darken(g_theme.panel_top, 7, 10);
  Color pb = sm_darken(g_theme.panel_bot, 6, 10);

  screen_touch(dst);
  for (int x = 0; x < BW; x++) {
    u32 col_off = (u32)x * BSH * 3u;
    for (int i = 0; i < BSH * 3; i++)
      dst[col_off + i] = (u8)((src[col_off + i] * dim) >> 8);
  }
  if (right_edge > 0) {
    draw_gradient_round_rect(dst, sx + 6, 8, SM_SIDEBAR_W - 10, BSH - 16,
                             10, BSH, pt, pb);
  }
}

static void sm_sidebar_header(const char *title, const char *subtitle) {
  volatile u8 *fb = VRAM_BOT_A;
  int sx = sm_slide_x;
  Color bg = sm_darken(g_theme.panel_top, 7, 10);
  int ty = 10 + (26 - ui_th(&ui_bold)) / 2;
  ui_text_fit(fb, sx + 16, ty, BSH, title, SM_SIDEBAR_W - 76,
              g_theme.text, bg, &ui_bold);
  if (subtitle && subtitle[0]) {
    int sw = ui_tw(&ui_small, subtitle);
    int sy = 10 + (26 - ui_th(&ui_small)) / 2 + 1;
    ui_text(fb, sx + SM_SIDEBAR_W - 14 - sw, sy, BSH, subtitle,
            g_theme.text_dim, bg, &ui_small);
  }
  draw_filled_rect(fb, sx + 14, SM_HEADER_H - 1, SM_SIDEBAR_W - 26, 1,
                   BSH, g_theme.panel_top);
}

static void sm_sidebar_item(int row, const char *label, const char *value,
                            int selected) {
  if (row < 0 || row >= SM_VISIBLE) return;
  volatile u8 *fb = VRAM_BOT_A;
  int sx = sm_slide_x;
  int y = SM_LIST_Y + row * SM_ROW_H;
  Color bg = selected ? g_accent : g_theme.slot_top;

  if (selected) {
    draw_gradient_round_rect(fb, sx + 12, y, SM_SIDEBAR_W - 22, SM_ROW_H - 3,
                             7, BSH, g_accent, sm_darken(g_accent, 7, 10));
  } else {
    draw_gradient_round_rect(fb, sx + 12, y, SM_SIDEBAR_W - 22, SM_ROW_H - 3,
                             7, BSH, g_theme.slot_top, g_theme.slot_bot);
  }
  int vw = (value && value[0]) ? ui_tw(&ui_small, value) : 0;
  int max_lbl = SM_SIDEBAR_W - 36 - (vw ? vw + 8 : 0);
  if (max_lbl < 32) max_lbl = 32;
  int ly = y + (SM_ROW_H - 3 - ui_th(&ui_font)) / 2;
  ui_text_fit(fb, sx + 18, ly, BSH, label, max_lbl,
              selected ? COLOR_WHITE : g_theme.text, bg, &ui_font);
  if (vw > 0) {
    int vx = sx + SM_SIDEBAR_W - 16 - vw;
    int vy = y + (SM_ROW_H - 3 - ui_th(&ui_small)) / 2;
    Color vc = selected ? COLOR_WHITE : g_theme.text_dim;
    if (!selected && value[0] == 'O' && value[1] == 'N' && value[2] == '\0')
      vc = sm_rgb(0x4CD964u);
    else if (!selected && value[0] == 'O' && value[1] == 'F' && value[2] == 'F')
      vc = sm_rgb(0xFF5A52u);
    ui_text(fb, vx, vy, BSH, value, vc, bg, &ui_small);
  }
}

static void sm_sidebar_separator(int row, const char *label) {
  if (row < 0 || row >= SM_VISIBLE) return;
  volatile u8 *fb = VRAM_BOT_A;
  int sx = sm_slide_x;
  int y = SM_LIST_Y + row * SM_ROW_H;
  draw_filled_rect(fb, sx + 14, y + SM_ROW_H / 2, SM_SIDEBAR_W - 26, 1,
                   BSH, sm_rgb(0x1D212Cu));
  if (label && label[0])
    ui_text_fit(fb, sx + 16, y + 4, BSH, label, SM_SIDEBAR_W - 30,
                sm_rgb(0x6C7382u), sm_rgb(0x0E1015u), &ui_small);
}

static void sm_sidebar_footer(const char *hint) {
  volatile u8 *fb = VRAM_BOT_A;
  int sx = sm_slide_x;
  draw_filled_rect(fb, sx + 14, SM_FOOTER_Y, SM_SIDEBAR_W - 26, 1,
                   BSH, sm_rgb(0x1D212Cu));
  int fy = SM_FOOTER_Y + (BSH - 8 - SM_FOOTER_Y - ui_th(&ui_small)) / 2;
  ui_text_fit(fb, sx + 14, fy, BSH, hint, SM_SIDEBAR_W - 26,
              sm_rgb(0x6C7382u), sm_rgb(0x0E1015u), &ui_small);
}

static void sm_draw_pixel(int x, int y, uint32_t rgb) {
  draw_pixel(VRAM_BOT_A, x + sm_slide_x, y, BSH, sm_rgb(rgb));
}
static void sm_draw_rect(int x, int y, int w, int h, uint32_t rgb) {
  draw_filled_rect(VRAM_BOT_A, x + sm_slide_x, y, w, h, BSH, sm_rgb(rgb));
}
static void sm_draw_rect_alpha(int x, int y, int w, int h, uint32_t rgb, int a) {
  draw_filled_rect_alpha(VRAM_BOT_A, x + sm_slide_x, y, w, h, BSH, sm_rgb(rgb), a);
}
static void sm_draw_text_api(int x, int y, const char *s, uint32_t rgb) {
  ui_text(VRAM_BOT_A, x + sm_slide_x, y, BSH, s, sm_rgb(rgb),
          sm_rgb(0x0E1015u), &ui_font);
}
static void sm_hud_rect(int screen, int x, int y, int w, int h, uint32_t rgb, int a) {
  if (screen == 0)
    draw_filled_rect_alpha(VRAM_TOP_LA, x, y, w, h, TSH, sm_rgb(rgb), a);
  else
    draw_filled_rect_alpha(VRAM_BOT_A, x, y, w, h, BSH, sm_rgb(rgb), a);
}
static void sm_hud_text(int screen, int x, int y, const char *s, uint32_t rgb) {
  if (screen == 0)
    ui_text(VRAM_TOP_LA, x, y, TSH, s, sm_rgb(rgb), COLOR_BLACK, &ui_small);
  else
    ui_text(VRAM_BOT_A, x, y, BSH, s, sm_rgb(rgb), COLOR_BLACK, &ui_small);
}
static uint32_t sm_now_us(void) { return timer_ticks() * 15u; }
static uint32_t sm_keys_held(void) { return get_keys(); }
static int sm_get_brightness(void) {
  volatile AuroraSystemMenuShared *s = aurora_system_menu_shared();
  return s->brightness <= 4u ? (int)s->brightness : 3;
}
static void sm_set_brightness(int level) {
  volatile AuroraSystemMenuShared *s = aurora_system_menu_shared();
  if (level < 0) level = 0;
  if (level > 4) level = 4;
  s->brightness = (uint32_t)level;
  audio_set_brightness((uint32_t)level);
}
static void sm_request_home(void) {
  navigation_requested = 1;
  sm_close_req = 1;
}

static int sm_app_active(void) { return 0; }
static int sm_app_modding(void) { return 1; }
static const char *sm_app_path(void) { return "/AURORAOS.BIN"; }

static int sm_addr_valid(uint32_t addr, uint32_t len) {
  if (len == 0u || addr + len < addr) return 0;
  if (addr >= 0x22000000u && addr + len <= 0x23000000u) return 1;
  if (addr >= 0x24000000u && addr + len <= 0x26000000u) return 1;
  return 0;
}

static int sm_mem_read(uint32_t addr, void *dst, uint32_t len) {
  if (!dst || !sm_addr_valid(addr, len)) return 0;
  const volatile uint8_t *src = (const volatile uint8_t *)addr;
  uint8_t *out = (uint8_t *)dst;
  for (uint32_t i = 0; i < len; i++)
    out[i] = src[i];
  return 1;
}

static int sm_mem_write(uint32_t addr, const void *src, uint32_t len) {
  if (!src || !sm_addr_valid(addr, len)) return 0;
  volatile uint8_t *dst = (volatile uint8_t *)addr;
  const uint8_t *in = (const uint8_t *)src;
  for (uint32_t i = 0; i < len; i++)
    dst[i] = in[i];
  os_cache_sync();
  return 1;
}

static uint32_t sm_mem_find(uint32_t start, uint32_t len, const void *pat,
                            uint32_t pat_len) {
  if (!pat || pat_len == 0u || pat_len > len || !sm_addr_valid(start, len))
    return 0u;
  const volatile uint8_t *mem = (const volatile uint8_t *)start;
  const uint8_t *p = (const uint8_t *)pat;
  uint32_t limit = len - pat_len;
  for (uint32_t i = 0; i <= limit; i++) {
    uint32_t j = 0;
    while (j < pat_len && mem[i + j] == p[j]) j++;
    if (j == pat_len) return start + i;
  }
  return 0u;
}

static int sm_set_mod_keys(uint32_t k) {
  volatile AuroraSystemMenuShared *s = aurora_system_menu_shared();
  s->mod_keys = k & 0xFFFu;
  os_cache_sync();
  return 1;
}

static void sm_set_accent(uint32_t rgb) {
  volatile AuroraSystemMenuShared *s = aurora_system_menu_shared();
  g_accent = sm_rgb(rgb);
  s->accent_rgb = rgb & 0xFFFFFFu;
  ui_bg_invalidate();
  os_cache_sync();
}

static void sm_set_led_color(uint8_t r, uint8_t g, uint8_t b) {
  power_set_led(r, g, b);
}

static void sm_play_tone(uint32_t freq_hz) {
  if (freq_hz)
    audio_play_tone(freq_hz);
  else
    audio_stop();
}

static int sm_get_battery(int *charging, int *mv, int *temp_c) {
  if (charging) *charging = battery_charging();
  if (mv)       *mv = battery_millivolts();
  if (temp_c)   *temp_c = battery_temperature();
  return battery_percent();
}

static void sm_get_sys_info(uint32_t *cpu_mhz, uint32_t *is_n3ds,
                            uint32_t *core_ver, uint32_t *gpu_ops) {
  const AudioCtrl *ct = (const AudioCtrl *)AUDIO_CTRL_ADDR;
  const GpuShared *g  = (const GpuShared *)GPU_SHARED_ADDR;
  uint32_t div = ct->clock_div ? ct->clock_div : 1u;
  if (cpu_mhz)  *cpu_mhz  = div * 268u;
  if (is_n3ds)  *is_n3ds  = (ct->socinfo & (1u << 2)) ? 1u : 0u;
  if (core_ver) *core_ver = ct->version;
  if (gpu_ops)  *gpu_ops  = g->ops;
}

static void sm_reboot(void) { power_reboot(); }

static const AmenuApi g_api9 = {
  AMENU_ABI_VERSION,
  SM_SIDEBAR_W, BW, BSH,
  sm_sidebar_clear, sm_sidebar_header, sm_sidebar_item,
  sm_sidebar_separator, sm_sidebar_footer,
  sm_draw_pixel, sm_draw_rect, sm_draw_rect_alpha, sm_draw_text_api,
  sm_hud_rect, sm_hud_text,
  sm_accent_rgb, sm_now_us, sm_keys_held,
  sm_get_brightness, sm_set_brightness, sm_request_home,
  sm_app_active, sm_app_modding, sm_app_path,
  sm_mem_read, sm_mem_write, sm_mem_find, sm_set_mod_keys,
  sm_set_accent, sm_set_led_color, sm_play_tone,
  sm_get_battery, sm_get_sys_info, sm_reboot,
};

static const AmenuPlugin *sm_plugin_get(AmenuSlot *slot) {
  if (!slot || !slot->loaded || !slot->entry_addr) return 0;
  if (!slot->initialized) {
    AmenuEntryFn entry = (AmenuEntryFn)slot->entry_addr;
    const AmenuPlugin *p = entry(&g_api9);
    slot->initialized = 1;
    if (p && p->abi_version == AMENU_ABI_VERSION) {
      slot->plugin_ptr = (uint32_t)p;
      if (p->on_load) p->on_load(&g_api9);
    } else {
      slot->plugin_ptr = 0;
    }
  }
  return (const AmenuPlugin *)slot->plugin_ptr;
}

static int sm_build_items(SmItem *items) {
  int n = 0;
  AmenuTable *tbl = amenu_table();
  if (tbl->magic == AMENU_TABLE_MAGIC && tbl->version == AMENU_ABI_VERSION) {
    uint32_t cnt = tbl->count > AMENU_MAX_PLUGINS ? AMENU_MAX_PLUGINS : tbl->count;
    for (uint32_t i = 0; i < cnt; i++) {
      if (tbl->slots[i].loaded && sm_plugin_get(&tbl->slots[i])) {
        items[n].kind = SM_PLUGIN;
        items[n].slot_idx = (int)i;
        n++;
      }
    }
  }
  if (home_started) {
    items[n].kind = SM_HOME; items[n].slot_idx = -1; n++;
  }
  items[n].kind = SM_BRIGHTNESS;     items[n].slot_idx = -1; n++;
  items[n].kind = SM_RELOAD_PLUGINS; items[n].slot_idx = -1; n++;
  items[n].kind = SM_POWER_OFF;      items[n].slot_idx = -1; n++;
  return n;
}

static int sm_in_rrect(int lx, int ly, int w, int h, int r) {
  int cx = (lx < r) ? r - 1 - lx : (lx >= w - r) ? lx - (w - r) : 0;
  int cy = (ly < r) ? r - 1 - ly : (ly >= h - r) ? ly - (h - r) : 0;
  return cx * cx + cy * cy <= r * r;
}

static void sm_draw_dock_thumb(volatile u8 *fb, int x, int y, int w, int h,
                               int r, const volatile uint16_t *thumb, int sel) {
  for (int ly = 0; ly < h; ly++) {
    int py = y + ly;
    if (py < 0 || py >= BSH) continue;
    int ty = (ly * (int)AURORA_DOCK_THUMB_H) / h;
    u32 yoff = (u32)(BSH - 1 - py) * 3u;
    for (int lx = 0; lx < w; lx++) {
      int px = x + lx;
      if (px < 0 || px >= BW) continue;
      if (!sm_in_rrect(lx, ly, w, h, r)) continue;
      u32 off = (u32)px * BSH * 3u + yoff;
      int border = sel && (lx < 2 || lx >= w - 2 || ly < 2 || ly >= h - 2 ||
                           !sm_in_rrect(lx, ly, w, h, r + 2));
      if (border) {
        fb[off]     = g_accent.b;
        fb[off + 1] = g_accent.g;
        fb[off + 2] = g_accent.r;
        continue;
      }
      int tx = (lx * (int)AURORA_DOCK_THUMB_W) / w;
      uint16_t c565 = thumb[ty * (int)AURORA_DOCK_THUMB_W + tx];
      u32 cr = ((c565 >> 11) & 31u) * 255u / 31u;
      u32 cg = ((c565 >> 5) & 63u) * 255u / 63u;
      u32 cb = (c565 & 31u) * 255u / 31u;
      if (sel) {
        cr = (cr * 170u + (u32)g_accent.r * 85u) >> 8;
        cg = (cg * 170u + (u32)g_accent.g * 85u) >> 8;
        cb = (cb * 170u + (u32)g_accent.b * 85u) >> 8;
      } else {
        cr = (cr * 140u) >> 8;
        cg = (cg * 140u) >> 8;
        cb = (cb * 140u) >> 8;
      }
      fb[off]     = (u8)cb;
      fb[off + 1] = (u8)cg;
      fb[off + 2] = (u8)cr;
    }
  }
}

static void sm_draw_dock_icon(volatile u8 *fb, int x, int y,
                              const volatile uint16_t *icon) {
  for (int ly = 0; ly < (int)AURORA_DOCK_ICON_H; ly++) {
    int py = y + ly;
    if (py < 0 || py >= BSH) continue;
    u32 yoff = (u32)(BSH - 1 - py) * 3u;
    for (int lx = 0; lx < (int)AURORA_DOCK_ICON_W; lx++) {
      int px = x + lx;
      if (px < 0 || px >= BW) continue;
      if (!sm_in_rrect(lx, ly, (int)AURORA_DOCK_ICON_W, (int)AURORA_DOCK_ICON_H, 6))
        continue;
      uint16_t c565 = icon[ly * (int)AURORA_DOCK_ICON_W + lx];
      u32 off = (u32)px * BSH * 3u + yoff;
      fb[off]     = (u8)((c565 & 31u) * 255u / 31u);
      fb[off + 1] = (u8)(((c565 >> 5) & 63u) * 255u / 63u);
      fb[off + 2] = (u8)(((c565 >> 11) & 31u) * 255u / 31u);
    }
  }
}

static void sm_draw_dock(int dock_focus, int dock_sel) {
  volatile u8 *fb = VRAM_BOT_A;
  volatile AuroraSystemMenuShared *s = aurora_system_menu_shared();
  int slide_r = (-sm_slide_x * 124) / SM_SIDEBAR_W;
  int dx = SM_SIDEBAR_W + 4 + slide_r;
  int dy = 8, dw = BW - SM_SIDEBAR_W - 10, dh = BSH - 16;
  if (dx >= BW)
    return;

  Color pt = { (u8)(g_theme.panel_top.r * 7 / 10),
               (u8)(g_theme.panel_top.g * 7 / 10),
               (u8)(g_theme.panel_top.b * 7 / 10) };
  Color pb = { (u8)(g_theme.panel_bot.r * 6 / 10),
               (u8)(g_theme.panel_bot.g * 6 / 10),
               (u8)(g_theme.panel_bot.b * 6 / 10) };
  Color acc_bot = { (u8)(g_accent.r * 7 / 10),
                    (u8)(g_accent.g * 7 / 10),
                    (u8)(g_accent.b * 7 / 10) };

  draw_gradient_round_rect(fb, dx, dy, dw, dh, 10, BSH, pt, pb);
  ui_text(fb, dx + 10, dy + 8, BSH, "Dock", g_accent, pt, &ui_bold);

  u32 dcnt = s->dock_count;
  if (dcnt > AURORA_DOCK_MAX) dcnt = AURORA_DOCK_MAX;
  for (u32 i = 0; i < AURORA_DOCK_MAX; i++) {
    int sy = dy + 32 + (int)i * 60;
    int sh = 52;
    int cx = dx + 6, cw = dw - 12;
    int sel = dock_focus && (int)i == dock_sel && i < dcnt;
    if (i < dcnt && s->dock[i].has_thumb) {
      sm_draw_dock_thumb(fb, cx, sy, cw, sh, 8, s->dock[i].thumb, sel);
    } else if (sel) {
      draw_gradient_round_rect(fb, cx, sy, cw, sh, 8, BSH,
                               g_accent, acc_bot);
    } else {
      draw_gradient_round_rect(fb, cx, sy, cw, sh, 8, BSH,
                               g_theme.slot_top, g_theme.slot_bot);
    }
    if (i < dcnt && s->dock[i].name[0]) {
      if (s->dock[i].has_icon) {
        sm_draw_dock_icon(fb, cx + 6, sy + 10, s->dock[i].icon);
      } else {
        Color tc = s->dock[i].tint_rgb ? sm_rgb(s->dock[i].tint_rgb) : g_accent;
        char init[2] = { s->dock[i].name[0], '\0' };
        draw_gradient_round_rect(fb, cx + 6, sy + 10, 32, 32, 6, BSH,
                                 tc, sm_darken(tc, 7, 10));
        ui_text_mid(fb, cx + 22, sy + 10 + (32 - ui_th(&ui_bold)) / 2, BSH,
                    init, COLOR_WHITE, tc, &ui_bold);
      }
      ui_text_fit(fb, cx + 44, sy + (sh - ui_th(&ui_font)) / 2, BSH,
                  (const char *)s->dock[i].name, cw - 48,
                  (sel || s->dock[i].has_thumb) ? COLOR_WHITE : g_theme.text,
                  sel ? g_accent : g_theme.slot_top, &ui_font);
    } else {
      ui_text_mid(fb, cx + cw / 2, sy + (sh - ui_th(&ui_small)) / 2, BSH,
                  "Empty", g_theme.text_dim, g_theme.slot_top, &ui_small);
    }
  }
}

static void sm_draw_root(const SmItem *items, int count, int selected,
                         int scroll, int dock_focus, int dock_sel) {
  AmenuTable *tbl = amenu_table();
  char br_buf[8];
  int br = sm_get_brightness();
  if (!dock_focus && selected >= 0 && selected < count &&
      items[selected].kind == SM_BRIGHTNESS) {
    br_buf[0] = '<'; br_buf[1] = ' ';
    br_buf[2] = (char)('1' + br);
    br_buf[3] = '/'; br_buf[4] = '5';
    br_buf[5] = ' '; br_buf[6] = '>'; br_buf[7] = '\0';
  } else {
    br_buf[0] = (char)('1' + br);
    br_buf[1] = '/'; br_buf[2] = '5'; br_buf[3] = '\0';
  }

  sm_sidebar_clear();
  sm_sidebar_header("Aurora", "MOD: ON");
  for (int r = 0; r < SM_VISIBLE && scroll + r < count; r++) {
    int idx = scroll + r;
    int sel = (!dock_focus) && (idx == selected);
    const SmItem *it = &items[idx];
    if (it->kind == SM_PLUGIN) {
      AmenuSlot *slot = &tbl->slots[it->slot_idx];
      const AmenuPlugin *p = (const AmenuPlugin *)slot->plugin_ptr;
      const char *name = (p && p->name[0]) ? p->name : slot->filename;
      const char *val = ">";
      if (p && !(p->flags & AMENU_FLAG_HAS_MENU) && (p->flags & AMENU_FLAG_HAS_HUD))
        val = slot->hud_enabled ? "ON" : "OFF";
      else if (p && (p->flags & AMENU_FLAG_HAS_HUD) && slot->hud_enabled)
        val = "HUD";
      sm_sidebar_item(r, name, val, sel);
    } else if (it->kind == SM_HOME) {
      sm_sidebar_item(r, "Home", "", sel);
    } else if (it->kind == SM_BRIGHTNESS) {
      sm_sidebar_item(r, "Brightness", br_buf, sel);
    } else if (it->kind == SM_RELOAD_PLUGINS) {
      sm_sidebar_item(r, "Reload /amenu", "", sel);
    } else if (it->kind == SM_POWER_OFF) {
      sm_sidebar_item(r, L(STR_POWER_OFF), "", sel);
    }
  }
  if (dock_focus)
    sm_sidebar_footer("A: Launch  <: Menu  B: Close");
  else if (selected >= 0 && selected < count && items[selected].kind == SM_BRIGHTNESS)
    sm_sidebar_footer("</>: Adjust  B: Close");
  else if (selected >= 0 && selected < count && items[selected].kind == SM_PLUGIN)
    sm_sidebar_footer("A: Open  X: HUD  >: Dock");
  else
    sm_sidebar_footer("A: Select  >: Dock  B: Close");
  sm_draw_dock(dock_focus, dock_sel);
  screen_present_bottom();
}

static void sm_run_plugin(AmenuSlot *slot) {
  const AmenuPlugin *p = sm_plugin_get(slot);
  if (!p || !p->on_menu_frame) return;
  if (p->on_menu_open) p->on_menu_open(&g_api9);
  while (!sm_close_req) {
    u32 down = get_keys_down();
    u32 held = get_keys();
    int keep = p->on_menu_frame(&g_api9, held, down);
    screen_present_bottom();
    if (!keep) break;
    ui_idle();
  }
}

void home_system_menu(void) {
  volatile u32 *bottom = (volatile u32 *)VRAM_BOT_A;
  SmItem items[AMENU_MAX_PLUGINS + 5];
  int count, selected = 0, scroll = 0;
  int dock_focus = 0, dock_sel = 0, launch_dock = -1;
  int dock_bn_idx = -1, dock_bn_ok = 0, dock_top_dirty = 0;
  u32 dock_bn_t0 = 0;

  anim_flush();
  for (u32 i = 0; i < BOT_FB_SIZE / 4; i++)
    sm_saved_bottom[i] = bottom[i];

  sm_close_req = 0;
  count = sm_build_items(items);

  /* Smooth cubic ease-out slide-in */
  for (int step = 1; step <= 8; step++) {
    int inv = 8 - step;
    sm_slide_x = -(SM_SIDEBAR_W * inv * inv * inv) / 512;
    sm_draw_root(items, count, selected, scroll, dock_focus, dock_sel);
    ui_idle();
  }
  sm_slide_x = 0;

  while (!sm_close_req) {
    u32 k = get_keys_down();
    int tx, ty, redraw = 0, activate = 0;
    u32 dcnt = aurora_system_menu_shared()->dock_count;
    if (dcnt > AURORA_DOCK_MAX) dcnt = AURORA_DOCK_MAX;

    if (k & BUTTON_B) {
      ui_sound_back();
      break;
    }

    if (dock_focus) {
      if (k & BUTTON_DLEFT) {
        dock_focus = 0;
        ui_sound_move();
        redraw = 1;
      } else if (k & BUTTON_DUP) {
        if (dcnt > 0)
          dock_sel = dock_sel > 0 ? dock_sel - 1 : (int)dcnt - 1;
        ui_sound_move();
        redraw = 1;
      } else if (k & BUTTON_DDOWN) {
        if (dcnt > 0)
          dock_sel = dock_sel + 1 < (int)dcnt ? dock_sel + 1 : 0;
        ui_sound_move();
        redraw = 1;
      } else if ((k & BUTTON_A) && (u32)dock_sel < dcnt) {
        ui_sound_select();
        launch_dock = dock_sel;
        break;
      }
    } else {
      if (k & BUTTON_DUP) {
        selected = selected > 0 ? selected - 1 : count - 1;
        ui_sound_move();
        redraw = 1;
      }
      if (k & BUTTON_DDOWN) {
        selected = selected + 1 < count ? selected + 1 : 0;
        ui_sound_move();
        redraw = 1;
      }
      if (selected < scroll) scroll = selected;
      if (selected >= scroll + SM_VISIBLE) scroll = selected - SM_VISIBLE + 1;

      if (items[selected].kind == SM_BRIGHTNESS) {
        if (k & BUTTON_DLEFT) {
          sm_set_brightness(sm_get_brightness() - 1);
          ui_sound_move();
          redraw = 1;
        } else if (k & BUTTON_DRIGHT) {
          sm_set_brightness(sm_get_brightness() + 1);
          ui_sound_move();
          redraw = 1;
        }
      } else if ((k & BUTTON_DRIGHT) && dcnt > 0) {
        dock_focus = 1;
        if ((u32)dock_sel >= dcnt) dock_sel = 0;
        ui_sound_move();
        redraw = 1;
      }

      if ((k & BUTTON_X) && items[selected].kind == SM_PLUGIN) {
        AmenuTable *tbl = amenu_table();
        int sidx = items[selected].slot_idx;
        AmenuSlot *slot = &tbl->slots[sidx];
        const AmenuPlugin *p = (const AmenuPlugin *)slot->plugin_ptr;
        if (p && (p->flags & AMENU_FLAG_HAS_HUD)) {
          slot->hud_enabled = !slot->hud_enabled;
          if (slot->hud_enabled) tbl->hud_mask |= (1u << sidx);
          else                   tbl->hud_mask &= ~(1u << sidx);
          os_cache_sync();
          if (p->on_toggle) p->on_toggle(&g_api9, (int)slot->hud_enabled);
          ui_sound_select();
          redraw = 1;
        }
      }
    }

    if (touch_tap(&tx, &ty)) {
      int dx = SM_SIDEBAR_W + 4, dw = BW - SM_SIDEBAR_W - 10;
      if (tx >= dx && tx < dx + dw) {
        int hit_dock = 0;
        for (u32 i = 0; i < dcnt; i++) {
          int sy = 8 + 32 + (int)i * 60;
          if (touch_in(tx, ty, dx + 6, sy, dw - 12, 52)) {
            ui_sound_select();
            launch_dock = (int)i;
            hit_dock = 1;
            break;
          }
        }
        if (hit_dock)
          break;
      } else if (tx >= SM_SIDEBAR_W) {
        ui_sound_back();
        break;
      } else {
        int r = (ty - SM_LIST_Y) / SM_ROW_H;
        if (ty >= SM_LIST_Y && r >= 0 && r < SM_VISIBLE && scroll + r < count) {
          dock_focus = 0;
          selected = scroll + r;
          activate = 1;
        }
      }
    }

    if (!dock_focus && ((k & BUTTON_A) || activate)) {
      int kind = items[selected].kind;
      if (kind == SM_PLUGIN) {
        AmenuTable *tbl = amenu_table();
        int sidx = items[selected].slot_idx;
        AmenuSlot *slot = &tbl->slots[sidx];
        const AmenuPlugin *p = (const AmenuPlugin *)slot->plugin_ptr;
        ui_sound_select();
        if (p && p->on_menu_frame) {
          sm_run_plugin(slot);
          if (p->flags & AMENU_FLAG_HAS_HUD) {
            slot->hud_enabled = 1;
            tbl->hud_mask |= (1u << sidx);
            os_cache_sync();
          }
          redraw = 1;
        } else if (p && (p->flags & AMENU_FLAG_HAS_HUD)) {
          slot->hud_enabled = !slot->hud_enabled;
          if (slot->hud_enabled) tbl->hud_mask |= (1u << sidx);
          else                   tbl->hud_mask &= ~(1u << sidx);
          os_cache_sync();
          if (p->on_toggle) p->on_toggle(&g_api9, (int)slot->hud_enabled);
          redraw = 1;
        }
      } else if (kind == SM_HOME) {
        ui_sound_select();
        navigation_requested = 1;
        break;
      } else if (kind == SM_RELOAD_PLUGINS) {
        ui_sound_select();
        amenu_load_plugins();
        count = sm_build_items(items);
        if (selected >= count) selected = 0;
        scroll = 0;
        redraw = 1;
      } else if (kind == SM_POWER_OFF) {
        ui_sound_select();
        if (fv_confirm(L(STR_POWER_ASK), "", L(STR_POWER_OFF), L(STR_CANCEL)))
          power_shutdown();
        redraw = 1;
      }
    }

    /* Live top-screen banner preview while focused on a pinned dock app */
    if (!dock_focus && dock_bn_idx >= 0) {
      /* Left the dock: stop and clear the dock banner immediately. */
      ui_sound_banner_stop();
      dock_bn_idx = -1;
      dock_bn_ok = 0;
      ui_wallpaper(VRAM_TOP_LA, TOP_SCREEN_WIDTH, TSH, TSH);
      status_bar_draw();
      top_item();
      screen_present_top();
    } else if (dock_focus && (u32)dock_sel < dcnt) {
      if (dock_bn_idx != dock_sel) {
        ui_sound_banner_stop();
        dock_bn_idx = dock_sel;
        dock_bn_ok = 0;
        dock_top_dirty = 1;
        /* Clear previous banner immediately before loading the new selection. */
        ui_wallpaper(VRAM_TOP_LA, TOP_SCREEN_WIDTH, TSH, TSH);
        status_bar_draw();
        screen_present_top();
        volatile AuroraSystemMenuShared *s = aurora_system_menu_shared();
        const char *dp = (const char *)s->dock[dock_sel].path;
        for (int a = 0; a < napps; a++) {
          if (apps[a].path && dock_streq(apps[a].path, dp)) {
            if (apps[a].banner) {
              if (bn_loaded != a) {
                banner_unload();
                bn_loaded = HL_NONE;
                if (banner_load(apps[a].path, apps[a].banner))
                  bn_loaded = a;
              }
              if (bn_loaded == a) {
                dock_bn_ok = 1;
                dock_bn_t0 = anim_now();
              }
            }
            if (!dock_bn_ok) {
              ui_wallpaper(VRAM_TOP_LA, TOP_SCREEN_WIDTH, TSH, TSH);
              status_bar_draw();
              ui_text_mid(VRAM_TOP_LA, TOP_SCREEN_WIDTH / 2, 110, TSH,
                          apps[a].name ? apps[a].name : "App",
                          COLOR_WHITE, COLOR_HM_BG, &ui_title);
              screen_present_top();
            }
            break;
          }
        }
      }
      if (dock_bn_ok) {
        screen_touch(VRAM_TOP_LA);
        if (banner_draw((u32)anim_ms(dock_bn_t0), 256u, (u32)VRAM_TOP_LA)) {
          bn_on_screen = 1;
          volatile AuroraSystemMenuShared *s = aurora_system_menu_shared();
          if (!s->dock[dock_sel].has_thumb) {
            dock_capture_thumb((const char *)s->dock[dock_sel].path);
            redraw = 1;
          }
          status_bar_draw();
          screen_present_top();
        }
      }
    }

    if (redraw && !sm_close_req)
      sm_draw_root(items, count, selected, scroll, dock_focus, dock_sel);
    ui_idle();
  }

  if (!navigation_requested && launch_dock < 0) {
    /* Smooth cubic ease-in slide-out */
    for (int step = 1; step <= 8; step++) {
      sm_slide_x = -(SM_SIDEBAR_W * step * step * step) / 512;
      sm_draw_root(items, count, selected, scroll, dock_focus, dock_sel);
      ui_idle();
    }
    sm_slide_x = 0;
    for (u32 i = 0; i < BOT_FB_SIZE / 4; i++)
      bottom[i] = sm_saved_bottom[i];
    screen_present_bottom();
    if (dock_top_dirty) {
      ui_sound_banner_stop();
      ui_wallpaper(VRAM_TOP_LA, TOP_SCREEN_WIDTH, TSH, TSH);
      status_bar_draw();
      top_e = HL_NONE - 1;
      bn_state = BN_IDLE;
      banner_focus();
      top_update();
    }
  } else if (launch_dock >= 0) {
    sm_slide_x = 0;
    volatile AuroraSystemMenuShared *s = aurora_system_menu_shared();
    const char *path = (const char *)s->dock[launch_dock].path;
    for (int i = 0; i < napps; i++) {
      if (apps[i].path && dock_streq(apps[i].path, path)) {
        banner_leave_home();
        hk->open(&apps[i]);
        back_home();
        return;
      }
    }
    for (u32 i = 0; i < BOT_FB_SIZE / 4; i++)
      bottom[i] = sm_saved_bottom[i];
    screen_present_bottom();
  }
}

static void button(int b) {
  if (b == BTN_POWER) {
    ui_sound_select();
    power_ask();
  } else if (b == BTN_BACK) {
    if (view != HOME_ROOT)
      go_up();
  } else if (b == BTN_SHOP) {
    ui_sound_select();
    banner_leave_home();
    if (hk->shop) hk->shop();
    back_home();
  } else if (b == BTN_FILES) {
    ui_sound_select();
    banner_leave_home();
    if (hk->files) hk->files();
    back_home();
  } else if (b == BTN_CAMERA) {
    ui_sound_select();
    banner_leave_home();
    if (hk->camera) hk->camera();
    back_home();
  } else if (b == BTN_MUSIC) {
    ui_sound_select();
    banner_leave_home();
    if (hk->music) hk->music();
    back_home();
  } else {
    ui_sound_select();
    banner_leave_home();
    hk->settings();
    back_home();
  }
}

static int hit(int x, int y) {
  int bx, by, bw, bh;
  if (y < BAR_Y + BAR_H + 2) {
    for (int b = BTN_POWER; b <= BTN_SETTINGS; b++) {
      if (!has_btn(b))
        continue;
      btn_rect(b, &bx, &by, &bw, &bh);
      if (touch_in(x, y, bx - 1, 0, bw + 2, BAR_Y + BAR_H + 2))
        return HIT_BTN + b;
    }
    return HIT_NONE;
  }
  if (y >= DOTS_Y - 4 && pages() > 1) {
    int n = pages(), x0 = (BW - (n * 6 + (n - 1) * 8 + 8)) / 2;
    int p = (x - x0 + 4) / 14;
    if (x >= x0 - 4 && p >= 0 && p < n)
      return HIT_DOT + p;
    return HIT_NONE;
  }
  for (int s = 0; s < HOME_PER_PAGE; s++) {
    int sx = GRID_X + (s % HOME_COLS) * SLOT_STEP,
        sy = GRID_Y + (s / HOME_COLS) * SLOT_STEP;
    if (touch_in(x, y, sx - SLOT_RING, sy - SLOT_RING,
                 SLOT_SIZE + 2 * SLOT_RING, SLOT_SIZE + 2 * SLOT_RING))
      return s;
  }
  return HIT_NONE;
}

static void tap(int h) {
  if (h >= HIT_DOT) {
    if (page != h - HIT_DOT)
      ui_sound_move();
    page = h - HIT_DOT;
    refresh();
  } else if (h >= HIT_BTN) {
    button(h - HIT_BTN);
  } else if (h >= 0) {
    activate(cur());
  }
}

static void touch_step(void) {
  int x = 0, y = 0, down = touch_read(&x, &y, 0, 0);
  touch_tap(0, 0); /* keeps its edge in step for the screens opened from here */
  if (down) {
    t_x = x;
    t_y = y;
  }

  switch (tstate) {
  case T_WAIT:
    if (!down)
      tstate = T_IDLE;
    break;

  case T_IDLE:
    if (!down)
      break;
    t_x0 = x;
    t_y0 = y;
    t_t0 = anim_now();
    if (held != HL_NONE) {
      /* the stylus takes over from the D-pad */
      by_touch = 1;
      moved = 1;
      grab_dx = grab_dy = SLOT_SIZE / 2 + HOLD_LIFT;
      tstate = T_DRAG;
      top_update();
      drag_to(x, y);
      break;
    }
    t_hit = hit(x, y);
    if (t_hit >= 0 && t_hit < HOME_PER_PAGE) {
      bar = -1;
      sel = t_hit;
      refresh();
    }
    tstate = T_PRESS;
    break;

  case T_PRESS:
    if (!down) {
      tstate = T_IDLE;
      tap(t_hit);
      break;
    }
    if (iabs(x - t_x0) > SLOP || iabs(y - t_y0) > SLOP) {
      int dx = iabs(x - t_x0), dy = iabs(y - t_y0);
      int on_tile = t_hit >= 0 && t_hit < HOME_PER_PAGE &&
                    entry_at(cur()) != HL_NONE;
      int fast_h_swipe = pages() > 1 && dx > dy * 2 && anim_ms(t_t0) < 160;
      if (t_hit >= HIT_BTN && t_hit < HIT_DOT) {
        tstate = T_WAIT;
      } else if (on_tile && !fast_h_swipe) {
        int a = anim_idx(entry_at(cur()));
        grab_dx = t_x0 - (anim_px(&pos_x[a]) - anim_px(&scroll)) + HOLD_LIFT;
        grab_dy = t_y0 - anim_px(&pos_y[a]) + HOLD_LIFT;
        start_carry(cur(), 1);
        moved = 1;
        tstate = T_DRAG;
        drag_to(x, y);
      } else {
        tstate = T_SWIPE;
      }
      break;
    }
    if (t_hit >= 0 && t_hit < HOME_PER_PAGE &&
        entry_at(cur()) != HL_NONE && anim_ms(t_t0) >= HOLD_MS) {
      int a = anim_idx(entry_at(cur()));
      grab_dx = x - (anim_px(&pos_x[a]) - anim_px(&scroll)) + HOLD_LIFT;
      grab_dy = y - anim_px(&pos_y[a]) + HOLD_LIFT;
      start_carry(cur(), 1);
      moved = 0;
      tstate = T_DRAG;
    }
    break;

  case T_SWIPE:
    if (down) {
      int last = (pages() - 1) * BW, off = page * BW - (x - t_x0);
      if (off < 0)
        off /= 3;
      if (off > last)
        off = last + (off - last) / 3;
      anim_jump(&scroll, off);
      dirty = 1;
    } else {
      int dx = t_x - t_x0, was = page;
      if (dx <= -SWIPE && page + 1 < pages())
        page++;
      else if (dx >= SWIPE && page > 0)
        page--;
      if (page != was)
        ui_sound_move();
      tstate = T_IDLE;
      refresh();
    }
    break;

  case T_DRAG:
    if (held == HL_NONE) {
      tstate = down ? T_WAIT : T_IDLE;
    } else if (down) {
      drag_to(x, y);
    } else {
      tstate = T_IDLE;
      if (!moved) {
        /* held and let go without moving: the menu instead */
        int k = held_c == view ? held_i : -1;
        cancel_carry();
        if (k >= 0)
          menu(k);
      } else {
        drop();
      }
    }
    break;
  }
}

static void nav_keys(u32 k) {
  if (bar >= 0) {
    int b = bar;
    if (k & BUTTON_DLEFT)
      do
        b = b > BTN_POWER ? b - 1 : b;
      while (!has_btn(b));
    if (k & BUTTON_DRIGHT) {
      int n = b;
      do
        n = n < BTN_SETTINGS ? n + 1 : n;
      while (!has_btn(n));
      b = n;
    }
    if (k & (BUTTON_DDOWN | BUTTON_B))
      b = -1;
    if (b != bar) {
      ui_sound_move();
      bar = b;
      refresh();
    }
    if ((k & BUTTON_A) && bar >= 0)
      button(bar);
    return;
  }

  {
    int col = sel % HOME_COLS, row = sel / HOME_COLS, p = page, s = sel;
    if (k & BUTTON_DLEFT) {
      if (col > 0)
        s--;
      else if (p > 0) {
        p--;
        s += HOME_COLS - 1;
      }
    }
    if (k & BUTTON_DRIGHT) {
      if (col < HOME_COLS - 1)
        s++;
      else if (p + 1 < pages()) {
        p++;
        s -= HOME_COLS - 1;
      }
    }
    if (k & BUTTON_DDOWN && row < HOME_ROWS - 1)
      s += HOME_COLS;
    if (k & BUTTON_L && p > 0)
      p--;
    if (k & BUTTON_R && p + 1 < pages())
      p++;
    if (k & BUTTON_DUP) {
      if (row > 0) {
        s -= HOME_COLS;
      } else {
        bar = BTN_SETTINGS;
      }
    }
    if (p != page || s != sel || bar >= 0) {
      ui_sound_move();
      page = p;
      sel = s;
      refresh();
    }
  }
  if (bar >= 0)
    return;
  if (k & BUTTON_A)
    activate(cur());
  else if ((k & BUTTON_B) && view != HOME_ROOT)
    go_up();
  else if (k & BUTTON_Y)
    menu(cur());
}

void home_init(const HomeApp *a, int n, const HomeHooks *hooks) {
  apps = a;
  icon_cache_reset();
  bottom_valid=0;
  napps = n > HOME_ITEMS ? HOME_ITEMS : n;
  hk = hooks;
  for (int i = 0; i < napps; i++)
    keys[i] = apps[i].key;
  hl_load(keys, napps);
  dock_load();
  view = HOME_ROOT;
  page = sel = 0;
  bar = -1;
  held = HL_NONE;
  rebuild();
  dock_sync_meta();
}

void home_reload(const HomeApp *a, int n) {
  apps = a;
  icon_cache_reset();
  bottom_valid=0;
  napps = n > HOME_ITEMS ? HOME_ITEMS : n;
  for (int i = 0; i < napps; i++)
    keys[i] = apps[i].key;
  hl_load(keys, napps);
  bar = -1;
  held = HL_NONE;
  rebuild();
  dock_sync_meta();
}

static int mcu_power_pressed(void) {
  u8 irq[4];
  return I2C_readRegBuf(I2C_DEV_MCU, 0x10, irq, sizeof(irq)) &&
         (irq[0] & 0x01u) != 0;
}

void home_run(void) {
  int clock_tick = 0, pwr_tick = 0, last_min = -1;
  u32 frame_at = 0;

  home_started = 1;
  /* Clear any stale MCU power-button IRQ from boot. */
  (void)mcu_power_pressed();
  for (;;) {
    u32 k;
    int ms;
    if (hk->tick)
      hk->tick();
    k = get_keys_down();

    /* A system menu opened from a nested screen keeps sending Back until
     * that screen returns here; then restore the Home Menu and clear the
     * request so normal navigation resumes. */
    if (home_navigation_requested()) {
      home_navigation_clear();
      if (held != HL_NONE)
        cancel_carry();
      back_home();
      continue;
    }

    if (held != HL_NONE) {
      if (!by_touch) {
        carry_keys(k);
      } else if (k & BUTTON_B) {
        ui_sound_back();
        cancel_carry();
      }
    } else {
      nav_keys(k);
      if (k & BUTTON_START) {
        ui_sound_select();
        banner_leave_home();
        hk->settings();
        back_home();
      } else if (k & BUTTON_X) {
        ui_sound_select();
        banner_leave_home();
        hk->terminal();
        back_home();
      }
    }
    touch_step();

    /* Physical Power button: single click = Power-Off popup, double click = Aurora Menu. */
    if (++pwr_tick >= 16) {
      pwr_tick = 0;
      if (mcu_power_pressed()) {
        int dbl = 0;
        u32 t0 = anim_now();
        while (anim_ms(t0) < 300) {
          ui_idle();
          if (anim_ms(t0) >= 35 && mcu_power_pressed()) {
            dbl = 1;
            break;
          }
        }
        if (dbl) {
          ui_sound_select();
          home_system_menu();
        } else {
          ui_sound_select();
          power_ask();
        }
      }
    }

    /* The MCU is on a slow I2C bus: sample it every 96 passes and repaint only
     * when the minute changes. */
    if (++clock_tick >= 96) {
      RtcTime now;
      clock_tick = 0;
      if (rtc_read(&now) && now.min != last_min) {
        last_min = now.min;
        status_bar_draw();
        bn_bar_valid = 0;
        screen_present_top();
      }
    }

    ms = anim_frame(&frame_at);
    if (ms) {
      static unsigned elapsed;
      elapsed += (unsigned)ms;
      if (elapsed >= 100) {
        elapsed = 0;
        galaxy_phase++;
        /* Animate all visible animated app icons (and folders containing them)
         * even when the cursor is not moving or resting on them. */
        int any_anim = (held >= 0 && !HL_IS_FOLDER(held) && apps[held].icon_animation);
        int start = page * HOME_PER_PAGE;
        int end = start + HOME_PER_PAGE;
        if (end > ndisp) end = ndisp;
        for (int k = start; k < end && !any_anim; k++) {
          int it = disp[k];
          if (it == GAP_ENTRY || it < 0)
            continue;
          if (HL_IS_FOLDER(it)) {
            int f = HL_FOLDER_ID(it);
            int fn = hl_count(f);
            if (fn > 4) fn = 4;
            for (int j = 0; j < fn; j++) {
              int c = hl_at(f, j);
              if (c >= 0 && !HL_IS_FOLDER(c) && apps[c].icon_animation) {
                any_anim = 1;
                break;
              }
            }
          } else if (apps[it].icon_animation) {
            any_anim = 1;
          }
        }
        if (any_anim)
          galaxy_dirty = 1;
      }
      animate(ms);
    }
    ui_idle();
  }
}
