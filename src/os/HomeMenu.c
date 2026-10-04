#include "homemenu.h"
#include "anim.h"
#include "fileview.h"
#include "homelayout.h"
#include "icons.h"
#include "keyboard.h"
#include "lang.h"
#include "power.h"
#include "statusbar.h"
#include "touch.h"
#include "ui.h"

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
enum { BTN_POWER, BTN_BACK, BTN_SETTINGS };

/* Touch, in ms and pixels. */
#define HOLD_MS   450 /* a press this long picks the tile up */
#define SPRING_MS 700 /* held over a folder this long, it opens */
#define EDGE_MS   550 /* held at a side of the screen this long, the page turns */
#define BACK_MS   600 /* held over the back button this long, it goes up */
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

static const Color folder_top = {0x40, 0x41, 0x4A},
                   folder_bot = {0x26, 0x27, 0x2E},
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

static void entry_icon16(volatile u8 *fb, int x, int y, int sh, int e) {
  if (HL_IS_FOLDER(e))
    icon16(fb, x, y, sh, ASSET_ICON_FOLDER_16, icon_folder_bits,
           COLOR_HM_TEXT2);
  else
    icon16(fb, x, y, sh, apps[e].asset_xs, apps[e].icon, COLOR_WHITE);
}

static void entry_icon32(volatile u8 *fb, int x, int y, int sh, int e) {
  if (HL_IS_FOLDER(e))
    ui_icon(fb, x, y, 32, sh, ASSET_ICON_FOLDER_32, icon_folder_bits,
            COLOR_HM_TEXT2);
  else
    ui_icon(fb, x, y, 32, sh, apps[e].asset_sm, apps[e].icon, COLOR_WHITE);
}

static void tile(volatile u8 *fb, int e, int x, int y, int size) {
  int r = 10 + (size - SLOT_SIZE) / 4;
  if (!HL_IS_FOLDER(e)) {
    draw_gradient_round_rect(fb, x, y, size, size, r, BSH, COLOR_HM_SLOT_TOP,
                             COLOR_HM_SLOT_BOT);
    ui_icon(fb, x, y, size, BSH, apps[e].asset_sm, apps[e].icon, COLOR_WHITE);
    return;
  }
  draw_gradient_round_rect(fb, x, y, size, size, r, BSH, folder_top,
                           folder_bot);
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
  return w > BW - 130 ? BW - 130 : w;
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
  } else {
    *x = BW - 46;
    *w = 32;
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

  {
    int sx = BW - 62, sy = 16;
    draw_filled_round_rect(fb, sx, sy, 11, 11, 5, BSH, COLOR_HM_TEXT2);
    draw_filled_round_rect(fb, sx + 2, sy + 2, 7, 7, 3, BSH, COLOR_HM_BAR);
    draw_filled_rect(fb, sx + 10, sy + 10, 4, 2, BSH, COLOR_HM_TEXT2);
  }
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

static void draw_bottom(void) {
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
                                 BSH, COLOR_HM_EMPTY_TOP, COLOR_HM_EMPTY_BOT);
    }
  }

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
    /* on a slot, the ring moves with its page as the pages slide */
    int w = anim_px(&ring_w), h = anim_px(&ring_h), r = h / 2;
    int dx = bar < 0 ? page * BW - sc : 0;
    if (r > 10 + SLOT_LIFT / 2 + SLOT_RING)
      r = 10 + SLOT_LIFT / 2 + SLOT_RING;
    draw_round_ring(fb, anim_px(&ring_x) + dx, anim_px(&ring_y), w, h, r,
                    SLOT_RING + 1, BSH, g_accent);
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
    ui_icon(fb, PV_BOX_X, PV_BOX_Y, PV_BOX, TSH, asset, bits, COLOR_WHITE);
  }

  draw_gradient_round_rect(fb, PV_CARD_X, PV_CARD_Y, PV_CARD_W, PV_CARD_H, 11,
                           TSH, COLOR_PANEL_TOP, COLOR_PANEL_BOT);
  ui_text_mid_fit(fb, TOP_SCREEN_WIDTH / 2, PV_CARD_Y + 7, TSH, name,
                  PV_CARD_W - 24, COLOR_WHITE, COLOR_PANEL_TOP, &ui_title);
  ui_text_mid_fit(fb, TOP_SCREEN_WIDTH / 2, PV_CARD_Y + 31, TSH, dev,
                  PV_CARD_W - 24, COLOR_HM_TEXT2, COLOR_PANEL_BOT, &ui_font);
  if (held != HL_NONE)
    ui_text_mid(fb, TOP_SCREEN_WIDTH / 2, PV_HINT_Y, TSH,
                L(by_touch ? STR_MOVE_TOUCH : STR_MOVE_KEYS), COLOR_HM_TEXT2,
                COLOR_HM_BG_BOT, &ui_small);
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
  if (top_view == view && top_held == held && top_bar == bar && top_e == e &&
      top_k == cur() && top_touch == by_touch)
    return;
  dir = cur() > top_k ? 1 : cur() < top_k ? -1 : 0;
  top_note();
  anim_xfade_begin(ANIM_TOP);
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
  rebuild();
  ui_wallpaper(VRAM_TOP_LA, TOP_SCREEN_WIDTH, TSH, TSH);
  status_bar_draw();
  top_item();
  top_note();
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
  if (moved_any) {
    draw_bottom();
    show |= ANIM_BOT;
  }
  if (anim_xfade_frame(ANIM_TOP))
    show |= ANIM_TOP;
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
    if ((k & (BUTTON_A | BUTTON_B)) || touch_tap(&tx, &ty))
      break;
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
    const int third = SLOT_SIZE * 3 / 10;
    int li = list_index(k);
    if (lx > third && lx < SLOT_SIZE - third && can_go_into(k))
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
  int kind = 0, arg = 0, bx, by, bw, bh;

  anim_jump(&held_x, x - grab_dx);
  anim_jump(&held_y, y - grab_dy);
  dirty = 1;
  if (!moved && (iabs(x - t_x0) > SLOP || iabs(y - t_y0) > SLOP))
    moved = 1;
  if (!moved)
    return;

  btn_rect(BTN_BACK, &bx, &by, &bw, &bh);
  if (y < BAR_Y + BAR_H + 4) {
    if (view != HOME_ROOT && x < bx + bw + 20)
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
    place(0);
  } else if (kind == 3 && anim_ms(hov_t0) >= EDGE_MS && page + 1 < pages()) {
    page++;
    hov_t0 = anim_now();
    place(0);
  } else if (kind == 4 && anim_ms(hov_t0) >= SPRING_MS) {
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
      gap = g;
      select_k(gap);
      refresh();
    }
  }
  if (k & BUTTON_A)
    drop();
  else if (k & BUTTON_B)
    cancel_carry();
}

enum { MI_MOVE, MI_MOVE_TO, MI_NEW, MI_RENAME, MI_REMOVE, MI_COUNT };
static const StringId mi_label[MI_COUNT] = {STR_MOVE, STR_MOVE_TO,
                                            STR_NEW_FOLDER, STR_RENAME,
                                            STR_REMOVE_FOLDER};

#define MN_X     12
#define MN_Y     10
#define MN_W     (BW - 24)
#define MN_H     (BSH - 20)
#define MN_BW    132
#define MN_BH    42
#define MN_BX(i) (MN_X + 12 + ((i) % 2) * (MN_BW + 8))
#define MN_BY(i) (MN_Y + 62 + ((i) / 2) * (MN_BH + 10))

static void menu_card(const char *title, const char *sub) {
  static const Color scrim = {0x00, 0x00, 0x00};
  volatile u8 *fb = VRAM_BOT_A;
  draw_bottom();
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
    ui_text_mid_fit(fb, x + MN_BW / 2, y + (MN_BH - ui_th(&ui_bold)) / 2, BSH,
                    L(mi_label[i]), MN_BW - 12,
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
    if (item != prev)
      menu_draw(e, item, on);
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
    if (item != prev)
      pick_draw(e, to, n, item, top);
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
  }
  back_home();
  if (r < 0) {
    message(r);
    back_home();
  }
}

static void menu(int k) {
  int item = menu_run(entry_at(k));
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
  press(k);
  if (HL_IS_FOLDER(e)) {
    open_folder(HL_FOLDER_ID(e));
  } else {
    hk->open(&apps[e]);
    back_home();
  }
}

static void power_ask(void) {
  if (fv_confirm(L(STR_POWER_ASK), "", L(STR_POWER_OFF), L(STR_CANCEL)))
    hk->power_off();
  back_home();
}

static void button(int b) {
  if (b == BTN_POWER) {
    power_ask();
  } else if (b == BTN_BACK) {
    if (view != HOME_ROOT)
      go_up();
  } else {
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
      if (touch_in(x, y, bx - 4, 0, bw + 8, BAR_Y + BAR_H + 2))
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
      tstate = (t_hit >= HIT_BTN && t_hit < HIT_DOT) ? T_WAIT : T_SWIPE;
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
      int dx = t_x - t_x0;
      if (dx <= -SWIPE && page + 1 < pages())
        page++;
      else if (dx >= SWIPE && page > 0)
        page--;
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
        bar = col == 0 ? BTN_POWER
              : col <= 2 ? (has_btn(BTN_BACK) ? BTN_BACK : BTN_POWER)
                         : BTN_SETTINGS;
      }
    }
    if (p != page || s != sel || bar >= 0) {
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
  napps = n > HOME_ITEMS ? HOME_ITEMS : n;
  hk = hooks;
  for (int i = 0; i < napps; i++)
    keys[i] = apps[i].key;
  hl_load(keys, napps);
  view = HOME_ROOT;
  page = sel = 0;
  bar = -1;
  held = HL_NONE;
  rebuild();
}

void home_reload(const HomeApp *a, int n) {
  apps = a;
  napps = n > HOME_ITEMS ? HOME_ITEMS : n;
  for (int i = 0; i < napps; i++)
    keys[i] = apps[i].key;
  hl_load(keys, napps);
  bar = -1;
  held = HL_NONE;
  rebuild();
}

void home_run(void) {
  int clock_tick = 0, last_min = -1;
  u32 frame_at = 0;

  for (;;) {
    u32 k;
    int ms;
    if (hk->tick)
      hk->tick();
    k = get_keys_down();

    if (held != HL_NONE) {
      if (!by_touch)
        carry_keys(k);
      else if (k & BUTTON_B)
        cancel_carry();
    } else {
      nav_keys(k);
      if (k & BUTTON_START) {
        hk->settings();
        back_home();
      } else if (k & BUTTON_X) {
        hk->terminal();
        back_home();
      }
    }
    touch_step();

    /* The MCU is on a slow I2C bus: sample it every 96 passes and repaint only
     * when the minute changes. */
    if (++clock_tick >= 96) {
      RtcTime now;
      clock_tick = 0;
      if (rtc_read(&now) && now.min != last_min) {
        last_min = now.min;
        status_bar_draw();
        screen_present_top();
      }
    }

    ms = anim_frame(&frame_at);
    if (ms)
      animate(ms);
    ui_idle();
  }
}
