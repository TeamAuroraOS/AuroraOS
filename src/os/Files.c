/* A directory is read once and sorted rather than re-read while scrolling,
 * since f_readdir is slow; one too big for the array is truncated and
 * marked. */

#include "files.h"
#include "fileops.h"
#include "fileview.h"
#include "assets.h"
#include "ui.h"
#include "anim.h"
#include "container.h"
#include "ff.h"
#include "icons.h"
#include "image.h"
#include "keyboard.h"
#include "lang.h"
#include "timer.h"
#include "touch.h"
#include "wav.h"
#include "audio.h"
#include <string.h>

#define FILES_MAX     256
#define FILES_NAME    (FF_LFN_BUF + 1) /* a name as f_readdir gives it, UTF-8 */
#define FILES_PATH    FO_PATH
#define FILES_VISIBLE 5

#define ROW_X    10
#define ROW_W    (BOT_SCREEN_WIDTH - 20)
#define ROW_H    30
#define ROW_STEP 34
#define ROW_Y0   40
/* The selection ring's width; at ROW_STEP 34 and ROW_H 30 a ring of 2 fills
 * the gap between rows exactly. */
#define ROW_RING 2

typedef struct {
  char name[FILES_NAME];
  u32 size;
  u8 kind;
  u8 is_dir;
} FileEntry;

static FileEntry entries[FILES_MAX];
static int entry_count;
static int truncated;
static char cwd[FILES_PATH] = "0:/";

/* Copy or Move marks an item here; Paste acts on it in the folder shown. */
enum { CLIP_NONE = 0, CLIP_COPY, CLIP_MOVE };
static int clip_mode;
static int clip_dir;
static char clip_path[FILES_PATH];
static char clip_name[FILES_NAME];

extern void delay(volatile u32 cycles);

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static const char *ext_of(const char *name) {
  const char *dot = 0;
  for (const char *p = name; *p; p++)
    if (*p == '.')
      dot = p;
  return dot ? dot + 1 : "";
}

static int ext_is(const char *name, const char *want) {
  const char *e = ext_of(name);
  while (*e && *want) {
    if (lower(*e) != lower(*want))
      return 0;
    e++;
    want++;
  }
  return !*e && !*want;
}

static void copy_name(char *dst, const char *src) {
  int n = 0;
  while (src[n] && n < FILES_NAME - 1) {
    dst[n] = src[n];
    n++;
  }
  dst[n] = 0;
}

/* `dir` joined with `name`; 0 when that would not fit `outsz`. */
static int join(char *out, int outsz, const char *dir, const char *name) {
  int n = 0;
  while (dir[n]) {
    if (n >= outsz - 1)
      return 0;
    out[n] = dir[n];
    n++;
  }
  if (n && out[n - 1] != '/') {
    if (n >= outsz - 1)
      return 0;
    out[n++] = '/';
  }
  for (int i = 0; name[i]; i++) {
    if (n >= outsz - 1)
      return 0;
    out[n++] = name[i];
  }
  out[n] = 0;
  return 1;
}

/* An AOS1/AUR1/AURC container looks like any other .bin, so the magic
 * decides. */
static int is_aurora_container(const char *dir, const char *name) {
  static FIL f;
  char path[FILES_PATH];
  char magic[4];
  UINT br;

  if (!join(path, (int)sizeof(path), dir, name))
    return 0;
  if (f_open(&f, path, FA_READ) != FR_OK)
    return 0;
  if (f_read(&f, magic, 4, &br) != FR_OK || br != 4) {
    f_close(&f);
    return 0;
  }
  f_close(&f);
  return aurora_magic_kind(magic) != 0;
}

FileKind files_kind(const char *name, int is_dir) {
  if (is_dir)
    return FKIND_DIR;
  if (ext_is(name, "png") || ext_is(name, "jpg") || ext_is(name, "jpeg") ||
      ext_is(name, "bmp"))
    return FKIND_IMAGE;
  if (ext_is(name, "wav") || ext_is(name, "mp3") || ext_is(name, "aaf"))
    return FKIND_AUDIO;
  if (ext_is(name, "bin"))
    return FKIND_BIN;
  if (ext_is(name, "txt") || ext_is(name, "log"))
    return FKIND_TEXT;
  return FKIND_OTHER;
}

static const char *kind_label(int kind, const char *name) {
  switch (kind) {
    case FKIND_DIR:    return "Folder";
    case FKIND_AURORA: return "Aurora app";
    case FKIND_IMAGE:
      if (ext_is(name, "png")) return "PNG image";
      if (ext_is(name, "bmp")) return "BMP image";
      return "JPEG image";
    case FKIND_AUDIO:
      if (ext_is(name, "wav")) return "WAV audio";
      if (ext_is(name, "mp3")) return "MP3 audio";
      return "Aurora audio";
    case FKIND_BIN:    return "Binary file";
    case FKIND_TEXT:   return ext_is(name, "log") ? "Log file" : "Text file";
    default:           return "File";
  }
}

/* `big` is the top screen's 64px art, otherwise the 24px size that fits the
 * 30px rows. */
static u32 kind_asset(int kind, int big) {
  switch (kind) {
    case FKIND_DIR:    return big ? ASSET_APP_FILES_64 : ASSET_APP_FILES_24;
    case FKIND_AURORA: return big ? ASSET_ICON_FILE_AURORA_64
                                  : ASSET_ICON_FILE_AURORA_24;
    case FKIND_IMAGE:
    case FKIND_AUDIO:  return big ? ASSET_ICON_FILE_MEDIA_64
                                  : ASSET_ICON_FILE_MEDIA_24;
    case FKIND_TEXT:   return big ? ASSET_ICON_FILE_TEXT_64
                                  : ASSET_ICON_FILE_TEXT_24;
    case FKIND_BIN:    return big ? ASSET_ICON_FILE_BIN_64
                                  : ASSET_ICON_FILE_BIN_24;
    default:           return big ? ASSET_ICON_UNKNOWN_64 : ASSET_ICON_UNKNOWN_24;
  }
}

static char *put_u32(char *p, u32 v) {
  char tmp[12];
  int n = 0;
  if (!v)
    *p++ = '0';
  while (v) {
    tmp[n++] = (char)('0' + v % 10);
    v /= 10;
  }
  while (n--)
    *p++ = tmp[n];
  return p;
}

static char *put_str(char *p, const char *s) {
  while (*s)
    *p++ = *s++;
  return p;
}

/* "934 B", "12.1 KB", "3.4 MB". */
static void fmt_size(char *out, u32 bytes) {
  const char *unit;
  u32 whole, frac;
  if (bytes < 1024u) {
    char *p = put_u32(out, bytes);
    *p++ = ' ';
    *p++ = 'B';
    *p = 0;
    return;
  }
  if (bytes < 1024u * 1024u) {
    whole = bytes / 1024u;
    frac = ((bytes % 1024u) * 10u) / 1024u;
    unit = "KB";
  } else {
    whole = bytes / (1024u * 1024u);
    frac = ((bytes % (1024u * 1024u)) / 1024u * 10u) / 1024u;
    unit = "MB";
  }
  char *p = put_u32(out, whole);
  *p++ = '.';
  *p++ = (char)('0' + frac);
  *p++ = ' ';
  *p++ = unit[0];
  *p++ = unit[1];
  *p = 0;
}

/* "Folder" or "Text file - 12.1 KB". */
static void fmt_detail(char *out, const FileEntry *e) {
  char *p = put_str(out, kind_label(e->kind, e->name));
  if (!e->is_dir) {
    p = put_str(p, " - ");
    fmt_size(p, e->size);
  } else {
    *p = 0;
  }
}

static int name_cmp(const char *a, const char *b) {
  while (*a && *b) {
    char ca = lower(*a), cb = lower(*b);
    if (ca != cb)
      return ca < cb ? -1 : 1;
    a++;
    b++;
  }
  return *a ? 1 : (*b ? -1 : 0);
}

/* Folders first, then by name. Insertion sort: the lists are short and this
 * avoids needing a scratch array. */
static void sort_entries(void) {
  static FileEntry key;
  for (int i = 1; i < entry_count; i++) {
    key = entries[i];
    int j = i - 1;
    while (j >= 0) {
      const FileEntry *e = &entries[j];
      int after = (e->is_dir != key.is_dir) ? (key.is_dir && !e->is_dir)
                                            : (name_cmp(e->name, key.name) > 0);
      if (!after)
        break;
      entries[j + 1] = entries[j];
      j--;
    }
    entries[j + 1] = key;
  }
}

static int read_dir(void) {
  static DIR dir;
  static FILINFO fno;

  entry_count = 0;
  truncated = 0;
  if (f_opendir(&dir, cwd) != FR_OK)
    return 0;

  while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0]) {
    if (fno.fattrib & AM_SYS)
      continue;
    if (entry_count >= FILES_MAX) {
      truncated = 1;
      break;
    }
    FileEntry *e = &entries[entry_count];
    copy_name(e->name, fno.fname);
    e->is_dir = (fno.fattrib & AM_DIR) ? 1 : 0;
    e->size = e->is_dir ? 0 : (u32)fno.fsize;
    e->kind = (u8)files_kind(e->name, e->is_dir);
    entry_count++;
  }
  f_closedir(&dir);

  /* Only now open the .bin candidates, so the magic check costs one read per
   * .bin rather than one per entry. */
  for (int i = 0; i < entry_count; i++)
    if (entries[i].kind == FKIND_BIN && is_aurora_container(cwd, entries[i].name))
      entries[i].kind = FKIND_AURORA;

  sort_entries();
  return 1;
}

/* Rereads the folder and puts the selection on `name`, or keeps the old index
 * when it is gone. */
static int reload_at(int sel, const char *name) {
  read_dir();
  for (int i = 0; name && i < entry_count; i++)
    if (!name_cmp(entries[i].name, name))
      return i;
  if (sel > entry_count - 1)
    sel = entry_count - 1;
  return sel < 0 ? 0 : sel;
}

/* 0, leaving cwd alone, when the path would be too long. */
static int path_join(const char *name) {
  static char next[FILES_PATH];
  if (!join(next, (int)sizeof(next), cwd, name))
    return 0;
  memcpy(cwd, next, strlen(next) + 1u);
  return 1;
}

/* Returns 0 when already at the drive root, so the caller can leave instead. */
static int path_up(void) {
  int n = 0, slash = -1;
  while (cwd[n])
    n++;
  if (n && cwd[n - 1] == '/')
    n--; /* ignore a trailing slash when looking for the parent */
  for (int i = 0; i < n; i++)
    if (cwd[i] == '/')
      slash = i;
  if (slash < 2 || n <= 3)
    return 0; /* "0:/" is as far up as this goes */
  /* Cutting at the root's own slash would leave "0:" and no path at all, so
   * that one case keeps the slash. */
  cwd[slash == 2 ? 3 : slash] = 0;
  return 1;
}

/* Long paths lose their middle, keeping the drive and the current folder, and
 * whatever is still too wide is cut at the end. */
static void fit_path(char *out, int outsz, const char *path, const Font *f,
                     int avail) {
  static char mid[FILES_PATH + 8];
  int n = (int)strlen(path), keep = 0;

  if (ui_tw(f, path) > avail)
    for (int i = n - 1; i > 3; i--)
      if (path[i] == '/') {
        keep = i;
        break;
      }
  if (!keep) {
    ui_fit(out, outsz, f, path, avail);
    return;
  }
  memcpy(mid, path, 3);
  memcpy(mid + 3, "...", 3);
  memcpy(mid + 6, path + keep, (u32)(n - keep) + 1u);
  ui_fit(out, outsz, f, mid, avail);
}

#define ICON_BOX  96
#define ICON_Y    34
#define CARD_X    40
#define CARD_Y    148
#define CARD_W    (TOP_SCREEN_WIDTH - 80)
#define CARD_H    62

/* Everything on the top screen that only changes with the directory or the
 * clipboard, kept apart so a cursor move repaints two small boxes. */
static void files_top_static(void) {
  char shown[128];

  ui_wallpaper(VRAM_TOP_LA, TOP_SCREEN_WIDTH, TOP_SCREEN_HEIGHT,
               TOP_SCREEN_HEIGHT);
  draw_filled_rect(VRAM_TOP_LA, 0, 0, TOP_SCREEN_WIDTH, 24, TOP_SCREEN_HEIGHT,
                   COLOR_HM_BAR);
  ui_text(VRAM_TOP_LA, 10, 4, TOP_SCREEN_HEIGHT, "Files", COLOR_WHITE,
          COLOR_HM_BAR, &ui_bold);

  fit_path(shown, sizeof(shown), cwd, &ui_small, TOP_SCREEN_WIDTH - 130);
  ui_text(VRAM_TOP_LA, 120, 6, TOP_SCREEN_HEIGHT, shown, COLOR_HM_TEXT2,
          COLOR_HM_BAR, &ui_small);

  if (clip_mode != CLIP_NONE) {
    char line[FILES_NAME + 40];
    char *p = put_str(line, clip_mode == CLIP_COPY ? "Copying " : "Moving ");
    p = put_str(p, clip_name);
    *p = 0;
    ui_text_mid_fit(VRAM_TOP_LA, TOP_SCREEN_WIDTH / 2, CARD_Y + CARD_H + 8,
                    TOP_SCREEN_HEIGHT, line, CARD_W, COLOR_HM_TEXT2,
                    COLOR_HM_BG_BOT, &ui_small);
  }
}

/* Both areas are repainted first: the icon and the card corners blend, so
 * drawing over the last frame would smear. */
static void files_top_item(int sel) {
  if (entry_count <= 0) {
    ui_wallpaper_rect(VRAM_TOP_LA, 0, 24, TOP_SCREEN_WIDTH,
                      CARD_Y + CARD_H - 24, TOP_SCREEN_HEIGHT);
    ui_text_mid(VRAM_TOP_LA, TOP_SCREEN_WIDTH / 2, 110, TOP_SCREEN_HEIGHT,
                "Empty folder", COLOR_HM_TEXT2, COLOR_HM_BG_BOT, &ui_title);
    return;
  }

  const FileEntry *e = &entries[sel];
  char detail[64];

  ui_wallpaper_rect(VRAM_TOP_LA, (TOP_SCREEN_WIDTH - ICON_BOX) / 2, ICON_Y,
                    ICON_BOX, ICON_BOX, TOP_SCREEN_HEIGHT);
  ui_icon(VRAM_TOP_LA, (TOP_SCREEN_WIDTH - ICON_BOX) / 2, ICON_Y, ICON_BOX,
          TOP_SCREEN_HEIGHT, kind_asset(e->kind, 1), icon_boot_bits,
          COLOR_WHITE);

  ui_patch_round_rect(VRAM_TOP_LA, CARD_X, CARD_Y, CARD_W, CARD_H, 11, 0,
                      TOP_SCREEN_HEIGHT);
  draw_gradient_round_rect(VRAM_TOP_LA, CARD_X, CARD_Y, CARD_W, CARD_H, 11,
                           TOP_SCREEN_HEIGHT, COLOR_PANEL_TOP, COLOR_PANEL_BOT);
  ui_text_mid_fit(VRAM_TOP_LA, TOP_SCREEN_WIDTH / 2, CARD_Y + 7,
                  TOP_SCREEN_HEIGHT, e->name, CARD_W - 24, COLOR_WHITE,
                  COLOR_PANEL_TOP, &ui_title);
  fmt_detail(detail, e);
  ui_text_mid(VRAM_TOP_LA, TOP_SCREEN_WIDTH / 2, CARD_Y + 31, TOP_SCREEN_HEIGHT,
              detail, COLOR_HM_TEXT2, COLOR_PANEL_BOT, &ui_font);
}

static void files_draw_top(int sel) {
  files_top_static();
  files_top_item(sel);
  screen_present_top();
}

static void files_fade_top(int sel) {
  anim_xfade_begin(ANIM_TOP);
  files_top_item(sel);
  if (entry_count <= 0) {
    anim_xfade_area(ANIM_TOP, 0, 24, TOP_SCREEN_WIDTH, CARD_Y + CARD_H - 24);
  } else {
    anim_xfade_area(ANIM_TOP, (TOP_SCREEN_WIDTH - ICON_BOX) / 2, ICON_Y,
                    ICON_BOX, ICON_BOX);
    anim_xfade_area(ANIM_TOP, CARD_X, CARD_Y, CARD_W, CARD_H);
  }
  anim_xfade_start(ANIM_TOP, 0);
  screen_present_top();
}

/* The rows scroll between the path bar and the footer. */
#define LIST_TOP 36
#define LIST_END (ROW_Y0 + FILES_VISIBLE * ROW_STEP)

static AnimList flist = {.visible = FILES_VISIBLE, .step = ROW_STEP};

static int list_top(int sel) {
  flist.count = entry_count;
  return anim_list_top(&flist, sel);
}

/* One row's card at `y`; the selection ring is drawn separately. */
static void files_row(int y, int i) {
  const FileEntry *e = &entries[i];
  char right[16];

  draw_gradient_round_rect(VRAM_BOT_A, ROW_X, y, ROW_W, ROW_H, 8,
                           BOT_SCREEN_HEIGHT, COLOR_HM_SLOT_TOP,
                           COLOR_HM_SLOT_BOT);
  ui_icon(VRAM_BOT_A, ROW_X + 4, y, ROW_H, BOT_SCREEN_HEIGHT,
          kind_asset(e->kind, 0), icon_boot_bits, COLOR_WHITE);

  if (e->is_dir) {
    right[0] = '>';
    right[1] = 0;
  } else {
    fmt_size(right, e->size);
  }
  int rw = ui_tw(&ui_small, right);
  ui_text_fit(VRAM_BOT_A, ROW_X + 40, y + (ROW_H - ui_th(&ui_font)) / 2,
              BOT_SCREEN_HEIGHT, e->name, ROW_W - 40 - 24 - rw, COLOR_WHITE,
              COLOR_HM_SLOT, &ui_font);
  ui_text(VRAM_BOT_A, ROW_X + ROW_W - 10 - rw,
          y + (ROW_H - ui_th(&ui_small)) / 2, BOT_SCREEN_HEIGHT, right,
          COLOR_HM_TEXT2, COLOR_HM_SLOT, &ui_small);
}

#define FOOT_Y   (BOT_SCREEN_HEIGHT - 22)
#define MENU_W   64
#define MENU_H   20
#define MENU_X   (BOT_SCREEN_WIDTH - 10 - MENU_W)
#define MENU_Y   (BOT_SCREEN_HEIGHT - 24)

static void files_footer(int sel) {
  char foot[40];
  char *p = foot;
  p = put_u32(p, (u32)(sel + 1));
  *p++ = ' ';
  *p++ = '/';
  *p++ = ' ';
  p = put_u32(p, (u32)entry_count);
  if (truncated)
    *p++ = '+';
  *p = 0;
  ui_wallpaper_rect(VRAM_BOT_A, ROW_X, BOT_SCREEN_HEIGHT - 24, 90, 20,
                    BOT_SCREEN_HEIGHT);
  ui_text(VRAM_BOT_A, ROW_X, FOOT_Y, BOT_SCREEN_HEIGHT, foot, COLOR_HM_TEXT2,
          COLOR_HM_BG_BOT, &ui_small);
}

static void files_hints(const char *hint) {
  ui_text(VRAM_BOT_A, MENU_X - 12 - ui_tw(&ui_small, hint), FOOT_Y,
          BOT_SCREEN_HEIGHT, hint, COLOR_HM_TEXT2, COLOR_HM_BG_BOT, &ui_small);
  draw_gradient_round_rect(VRAM_BOT_A, MENU_X, MENU_Y, MENU_W, MENU_H, 8,
                           BOT_SCREEN_HEIGHT, COLOR_HM_SLOT_TOP,
                           COLOR_HM_SLOT_BOT);
  ui_text_mid(VRAM_BOT_A, MENU_X + MENU_W / 2,
              MENU_Y + (MENU_H - ui_th(&ui_small)) / 2, BOT_SCREEN_HEIGHT,
              "X: Menu", COLOR_WHITE, COLOR_HM_SLOT, &ui_small);
}

/* The list as files_draw_bottom shows it, where its animation has it, without
 * presenting, so a dialog can go over it. */
static void files_paint_bottom(int sel) {
  char shown[128];

  ui_wallpaper(VRAM_BOT_A, BOT_SCREEN_WIDTH, BOT_SCREEN_HEIGHT,
               BOT_SCREEN_HEIGHT);
  if (entry_count > 0) {
    int scroll = anim_px(&flist.scroll);
    for (int i = 0; i < entry_count; i++) {
      int y = ROW_Y0 + i * ROW_STEP - scroll;
      if (y + ROW_H > LIST_TOP && y < LIST_END)
        files_row(y, i);
    }
    draw_round_ring(VRAM_BOT_A, ROW_X - ROW_RING,
                    ROW_Y0 + anim_px(&flist.ring) - scroll - ROW_RING,
                    ROW_W + 2 * ROW_RING, ROW_H + 2 * ROW_RING, 10,
                    ROW_RING + 1, BOT_SCREEN_HEIGHT, g_accent);
    ui_wallpaper_rect(VRAM_BOT_A, 0, 0, BOT_SCREEN_WIDTH, LIST_TOP,
                      BOT_SCREEN_HEIGHT);
    ui_wallpaper_rect(VRAM_BOT_A, 0, LIST_END, BOT_SCREEN_WIDTH,
                      BOT_SCREEN_HEIGHT - LIST_END, BOT_SCREEN_HEIGHT);
  }

  draw_filled_round_rect(VRAM_BOT_A, 8, 6, BOT_SCREEN_WIDTH - 16, 26, 9,
                         BOT_SCREEN_HEIGHT, COLOR_HM_BAR);
  fit_path(shown, sizeof(shown), cwd, &ui_font, BOT_SCREEN_WIDTH - 34);
  ui_text(VRAM_BOT_A, 18, 10, BOT_SCREEN_HEIGHT, shown, COLOR_WHITE,
          COLOR_HM_BAR, &ui_font);

  if (entry_count <= 0) {
    ui_text_mid(VRAM_BOT_A, BOT_SCREEN_WIDTH / 2, 110, BOT_SCREEN_HEIGHT,
                "Nothing here", COLOR_HM_TEXT2, COLOR_HM_BG_BOT, &ui_font);
    files_hints("B: up");
    return;
  }
  files_footer(sel);
  files_hints("A: open   B: up");
}

static void files_draw_bottom(int sel) {
  flist.count = entry_count;
  anim_list_jump(&flist, sel);
  files_paint_bottom(sel);
  screen_present_bottom();
}

static void files_update(int new_sel) {
  flist.count = entry_count;
  anim_list_to(&flist, new_sel);
  if (!anim_list_moving(&flist)) {
    files_paint_bottom(new_sel);
    screen_present_bottom();
  }
  files_fade_top(new_sel);
}

static void files_redraw(int sel) {
  files_draw_bottom(sel);
  files_draw_top(sel);
}

/* After a dialog: the list fades back when the caller redraws it. */
static void files_fade(void) { anim_transition(ANIM_FADE, ANIM_BOT); }

/* Left on screen without waiting for a key, for a notice that something slow is
 * running. */
static void files_toast(const char *title, const char *detail) {
  ui_dialog(VRAM_BOT_A, BOT_SCREEN_WIDTH, BOT_SCREEN_HEIGHT, BOT_SCREEN_HEIGHT,
            title, detail, g_accent, 1);
  screen_present_bottom();
}

static void files_message(const char *title, const char *detail) {
  ui_dialog(VRAM_BOT_A, BOT_SCREEN_WIDTH, BOT_SCREEN_HEIGHT, BOT_SCREEN_HEIGHT,
            title, detail, g_accent, 1);
  screen_present_bottom();
  while (1) {
    u32 k = get_keys_down();
    int tx, ty;
    if ((k & (BUTTON_A | BUTTON_B)) || touch_tap(&tx, &ty))
      break;
    ui_idle();
  }
  files_fade();
}

/* Until the user backs out; the file list stays on the bottom screen. */
static void view_image(const char *name, const Image *img) {
  char detail[48];
  char *p = detail;
  p = put_u32(p, (u32)img->w);
  *p++ = ' ';
  *p++ = 'x';
  *p++ = ' ';
  p = put_u32(p, (u32)img->h);
  *p = 0;
  int dw = ui_tw(&ui_small, detail);

  anim_transition(ANIM_FADE, ANIM_TOP);
  draw_filled_rect(VRAM_TOP_LA, 0, 0, TOP_SCREEN_WIDTH, TOP_SCREEN_HEIGHT,
                   TOP_SCREEN_HEIGHT, COLOR_HM_BG);
  image_draw_fit(VRAM_TOP_LA, 0, 24, TOP_SCREEN_WIDTH, TOP_SCREEN_HEIGHT - 24,
                 TOP_SCREEN_HEIGHT, img);
  draw_filled_rect(VRAM_TOP_LA, 0, 0, TOP_SCREEN_WIDTH, 24, TOP_SCREEN_HEIGHT,
                   COLOR_HM_BAR);
  ui_text_fit(VRAM_TOP_LA, 10, 4, TOP_SCREEN_HEIGHT, name,
              TOP_SCREEN_WIDTH - 36 - dw, COLOR_WHITE, COLOR_HM_BAR, &ui_bold);
  ui_text(VRAM_TOP_LA, TOP_SCREEN_WIDTH - 12 - dw, 6, TOP_SCREEN_HEIGHT, detail,
          COLOR_HM_TEXT2, COLOR_HM_BAR, &ui_small);
  screen_present_top();

  while (1) {
    u32 k = get_keys_down();
    int tx, ty;
    if ((k & (BUTTON_A | BUTTON_B)) || touch_tap(&tx, &ty))
      break;
    ui_idle();
  }
  anim_transition(ANIM_FADE, ANIM_BOTH);
}

static void open_media(const FileEntry *e) {
  char path[FILES_PATH];
  if (!join(path, (int)sizeof(path), cwd, e->name)) {
    files_message(e->name, fo_error(FO_TOO_DEEP));
    return;
  }

  if (e->kind == FKIND_IMAGE) {
    Image img;
    ImageResult r;
    if (ext_is(e->name, "jpg") || ext_is(e->name, "jpeg") ||
        ext_is(e->name, "png"))
      files_toast(e->name, "Decoding...");
    r = image_load(path, &img);
    if (r == IMG_OK)
      view_image(e->name, &img);
    else
      files_message(kind_label(e->kind, e->name), image_error(r));
    return;
  }

  if (e->kind == FKIND_AUDIO) {
    if (ext_is(e->name, "wav")) {
      u32 ns = 0, rate = 0, depth = 0;
      WavResult r = wav_play(path, &ns, &rate, &depth);
      if (r == WAV_OK) {
        char d[48];
        char *p = put_u32(d, rate);
        p = put_str(p, " Hz ");
        p = put_u32(p, depth);
        p = put_str(p, "-bit");
        *p = 0;
        files_message("Playing", d);
        audio_stop();
      } else {
        files_message("WAV audio", wav_error(r));
      }
      return;
    }
    if (!ext_is(e->name, "mp3")) {
      files_message(kind_label(e->kind, e->name), "Open it from the Music app");
      return;
    }
    /* MP3 is not decoded yet: offered as bytes, like any unsupported file. */
  }

  if (e->kind == FKIND_TEXT) {
    text_view(path, e->name, e->size);
    return;
  }

  if (fv_confirm("File is Unknown or Unsupported.", "View HEX?", "View HEX",
                 "Cancel"))
    hex_edit(path, e->name, e->size);
}

/* Progress card for a copy or delete. Redrawn at most every PG_EVERY_US, since
 * each redraw repaints the list under it. */
#define PG_W        272
#define PG_H        124
#define PG_X        ((BOT_SCREEN_WIDTH - PG_W) / 2)
#define PG_Y        ((BOT_SCREEN_HEIGHT - PG_H) / 2)
#define PG_BAR_W    (PG_W - 40)
#define PG_EVERY_US 120000u

static int pg_sel;
static const char *pg_title;
static u32 pg_mark;
static int pg_drawn;

static void progress_start(const char *title, int sel) {
  pg_title = title;
  pg_sel = sel;
  pg_drawn = 0;
  anim_popup(PG_X, PG_Y, PG_W, PG_H); /* runs when the card is first shown */
}

static void progress_draw(const char *name, u32 done, u32 total) {
  static const Color scrim = {0x00, 0x00, 0x00};
  volatile u8 *fb = VRAM_BOT_A;
  const int sh = BOT_SCREEN_HEIGHT;

  files_paint_bottom(pg_sel);
  draw_filled_rect_alpha(fb, 0, 0, BOT_SCREEN_WIDTH, sh, sh, scrim, 150);
  anim_popup_behind();
  draw_gradient_round_rect(fb, PG_X, PG_Y, PG_W, PG_H, 14, sh, COLOR_PANEL_TOP,
                           COLOR_PANEL_BOT);
  ui_text_mid(fb, BOT_SCREEN_WIDTH / 2, PG_Y + 14, sh, pg_title, COLOR_WHITE,
              COLOR_PANEL_TOP, &ui_bold);
  ui_text_mid_fit(fb, BOT_SCREEN_WIDTH / 2, PG_Y + 40, sh, name, PG_W - 24,
                  COLOR_HM_TEXT2, COLOR_PANEL_TOP, &ui_font);
  if (total) {
    /* Scaled down first, so the product stays within 32 bits. */
    u32 d = done, t = total, fill;
    while (t > 0xFFFFFu) {
      d >>= 1;
      t >>= 1;
    }
    fill = t ? d * (u32)PG_BAR_W / t : 0;
    if (fill > (u32)PG_BAR_W)
      fill = (u32)PG_BAR_W;
    draw_filled_round_rect(fb, PG_X + 20, PG_Y + 72, PG_BAR_W, 8, 4, sh,
                           COLOR_HM_SLOT);
    if (fill >= 8u)
      draw_filled_round_rect(fb, PG_X + 20, PG_Y + 72, (int)fill, 8, 4, sh,
                             g_accent);
  } else {
    char count[24];
    char *p = put_u32(count, done);
    p = put_str(p, done == 1 ? " item" : " items");
    *p = 0;
    ui_text_mid(fb, BOT_SCREEN_WIDTH / 2, PG_Y + 66, sh, count, COLOR_HM_TEXT2,
                COLOR_PANEL_BOT, &ui_small);
  }
  ui_text_mid(fb, BOT_SCREEN_WIDTH / 2, PG_Y + PG_H - 24, sh, "B: Cancel",
              COLOR_HM_TEXT2, COLOR_PANEL_BOT, &ui_small);
  screen_present_bottom();
  ui_idle();
}

/* FoProgress. B is read directly: get_keys_down() would lose the edge the
 * list loop is tracking. */
static int files_progress(const char *name, u32 done, u32 total) {
  if (get_keys() & BUTTON_B)
    return 0;
  if (pg_drawn && timer_calibrated() && timer_us_since(pg_mark) < PG_EVERY_US)
    return 1;
  pg_mark = timer_ticks();
  pg_drawn = 1;
  progress_draw(name, done, total);
  return 1;
}

/* After an operation, so a key pressed during it does not act on the list. */
static void files_settle(void) {
  while (get_keys() & BUTTON_B)
    ui_idle();
  get_keys_down();
}

enum { M_COPY, M_MOVE, M_RENAME, M_DELETE, M_NEWDIR, M_PASTE, M_COUNT };

static const char *const menu_label[M_COUNT] = {"Copy",   "Move",
                                                "Rename", "Delete",
                                                "New folder", "Paste"};

#define MN_X     12
#define MN_Y     10
#define MN_W     (BOT_SCREEN_WIDTH - 24)
#define MN_H     (BOT_SCREEN_HEIGHT - 20)
#define MN_BW    132
#define MN_BH    42
#define MN_BX(i) (MN_X + 12 + ((i) % 2) * (MN_BW + 8))
#define MN_BY(i) (MN_Y + 62 + ((i) / 2) * (MN_BH + 10))

static void menu_button(int i, int selected, int enabled) {
  volatile u8 *fb = VRAM_BOT_A;
  const int sh = BOT_SCREEN_HEIGHT;
  int x = MN_BX(i), y = MN_BY(i);

  if (selected)
    draw_filled_round_rect(fb, x - 3, y - 3, MN_BW + 6, MN_BH + 6, 12, sh,
                           g_accent);
  if (enabled)
    draw_gradient_round_rect(fb, x, y, MN_BW, MN_BH, 10, sh, COLOR_HM_SLOT_TOP,
                             COLOR_HM_SLOT_BOT);
  else
    draw_gradient_round_rect(fb, x, y, MN_BW, MN_BH, 10, sh, COLOR_HM_EMPTY_TOP,
                             COLOR_HM_EMPTY_BOT);
  ui_text_mid(fb, x + MN_BW / 2, y + (MN_BH - ui_th(&ui_bold)) / 2, sh,
              menu_label[i], enabled ? COLOR_WHITE : COLOR_HM_TEXT2,
              COLOR_HM_SLOT, &ui_bold);
}

/* Over the list rather than a copy of the screen: the list repaints quickly,
 * and the scrim would darken further with each pass over its own output. */
static void menu_draw(int sel, int item, const u8 *on) {
  static const Color scrim = {0x00, 0x00, 0x00};
  volatile u8 *fb = VRAM_BOT_A;
  const int sh = BOT_SCREEN_HEIGHT;
  char sub[FILES_NAME + 16];

  files_paint_bottom(sel);
  draw_filled_rect_alpha(fb, 0, 0, BOT_SCREEN_WIDTH, sh, sh, scrim, 150);
  anim_popup_behind();
  draw_gradient_round_rect(fb, MN_X, MN_Y, MN_W, MN_H, 14, sh, COLOR_PANEL_TOP,
                           COLOR_PANEL_BOT);
  if (entry_count > 0) {
    ui_text_mid_fit(fb, BOT_SCREEN_WIDTH / 2, MN_Y + 12, sh, entries[sel].name,
                    MN_W - 28, COLOR_WHITE, COLOR_PANEL_TOP, &ui_bold);
    fmt_detail(sub, &entries[sel]);
  } else {
    ui_text_mid(fb, BOT_SCREEN_WIDTH / 2, MN_Y + 12, sh, "This folder",
                COLOR_WHITE, COLOR_PANEL_TOP, &ui_bold);
    copy_name(sub, "Empty");
  }
  if (clip_mode != CLIP_NONE) {
    char *p = put_str(sub, "Paste: ");
    p = put_str(p, clip_name);
    *p = 0;
  }
  ui_text_mid_fit(fb, BOT_SCREEN_WIDTH / 2, MN_Y + 36, sh, sub, MN_W - 28,
                  COLOR_HM_TEXT2, COLOR_PANEL_TOP, &ui_small);
  for (int i = 0; i < M_COUNT; i++)
    menu_button(i, i == item, on[i]);
  screen_present_bottom();
}

/* The chosen M_* item, or -1 when closed with B, X or a tap outside. */
static int files_menu_run(int sel) {
  u8 on[M_COUNT];
  int has = entry_count > 0;
  int item = has ? M_COPY : M_NEWDIR;

  on[M_COPY] = on[M_MOVE] = on[M_RENAME] = on[M_DELETE] = (u8)has;
  on[M_NEWDIR] = 1;
  on[M_PASTE] = (u8)(clip_mode != CLIP_NONE);
  anim_popup(MN_X, MN_Y, MN_W, MN_H);
  menu_draw(sel, item, on);

  for (;;) {
    u32 k = get_keys_down();
    int prev = item, tx, ty;

    if ((k & BUTTON_DLEFT) && (item & 1))
      item--;
    if ((k & BUTTON_DRIGHT) && !(item & 1))
      item++;
    if ((k & BUTTON_DUP) && item >= 2)
      item -= 2;
    if ((k & BUTTON_DDOWN) && item + 2 < M_COUNT)
      item += 2;
    if (touch_tap(&tx, &ty)) {
      if (!touch_in(tx, ty, MN_X, MN_Y, MN_W, MN_H))
        return -1;
      for (int i = 0; i < M_COUNT; i++)
        if (on[i] && touch_in(tx, ty, MN_BX(i), MN_BY(i), MN_BW, MN_BH))
          return i;
    }
    if ((k & BUTTON_A) && on[item])
      return item;
    if (k & (BUTTON_B | BUTTON_X))
      return -1;
    if (item != prev)
      menu_draw(sel, item, on);
    ui_idle();
  }
}

static int files_menu(int sel) {
  int item = files_menu_run(sel);
  files_fade(); /* back to the list, or on to what was chosen */
  return item;
}

/* The top screen while the keyboard has the bottom one. */
static void name_top(const char *title, const char *detail) {
  files_top_static();
  ui_wallpaper_rect(VRAM_TOP_LA, 0, 24, TOP_SCREEN_WIDTH, CARD_Y + CARD_H - 24,
                    TOP_SCREEN_HEIGHT);
  ui_icon(VRAM_TOP_LA, (TOP_SCREEN_WIDTH - ICON_BOX) / 2, ICON_Y, ICON_BOX,
          TOP_SCREEN_HEIGHT, ASSET_APP_FILES_64, icon_boot_bits, COLOR_WHITE);
  draw_gradient_round_rect(VRAM_TOP_LA, CARD_X, CARD_Y, CARD_W, CARD_H, 11,
                           TOP_SCREEN_HEIGHT, COLOR_PANEL_TOP, COLOR_PANEL_BOT);
  ui_text_mid(VRAM_TOP_LA, TOP_SCREEN_WIDTH / 2, CARD_Y + 7, TOP_SCREEN_HEIGHT,
              title, COLOR_WHITE, COLOR_PANEL_TOP, &ui_title);
  ui_text_mid_fit(VRAM_TOP_LA, TOP_SCREEN_WIDTH / 2, CARD_Y + 31,
                  TOP_SCREEN_HEIGHT, detail, CARD_W - 24, COLOR_HM_TEXT2,
                  COLOR_PANEL_BOT, &ui_font);
  ui_wallpaper_rect(VRAM_TOP_LA, 0, CARD_Y + CARD_H + 4, TOP_SCREEN_WIDTH,
                    TOP_SCREEN_HEIGHT - CARD_Y - CARD_H - 4, TOP_SCREEN_HEIGHT);
  ui_text_mid(VRAM_TOP_LA, TOP_SCREEN_WIDTH / 2, CARD_Y + CARD_H + 8,
              TOP_SCREEN_HEIGHT,
              "B: Delete   L: Caps   START: Done   SELECT: Cancel",
              COLOR_HM_TEXT2, COLOR_HM_BG_BOT, &ui_small);
  screen_present_top();
}

static int same_str(const char *a, const char *b) {
  while (*a && *a == *b) {
    a++;
    b++;
  }
  return *a == *b;
}

/* A name typed on the keyboard, starting from `start`. 0 when cancelled or left
 * empty. */
static int ask_name(char *buf, const char *start, const char *title,
                    const char *detail) {
  int ok;
  copy_name(buf, start);
  anim_transition(ANIM_PUSH, ANIM_BOTH);
  name_top(title, detail);
  ok = keyboard_edit(buf, FILES_NAME, KB_FILENAME, "Enter a name", g_accent);
  anim_transition(ANIM_POP, ANIM_BOTH);
  return ok && buf[0] != 0;
}

static void op_rename(int *sel) {
  static char old[FILES_NAME], name[FILES_NAME];
  static char from[FILES_PATH], to[FILES_PATH];
  FoResult r = FO_OK;

  copy_name(old, entries[*sel].name);
  if (!ask_name(name, old, "Rename", old) || same_str(name, old)) {
    files_settle();
    files_redraw(*sel);
    return;
  }
  if (!join(from, FILES_PATH, cwd, old) || !join(to, FILES_PATH, cwd, name))
    r = FO_TOO_DEEP;
  else
    r = fo_move(from, to);
  if (r == FO_OK && clip_mode != CLIP_NONE && fo_within(clip_path, from))
    clip_mode = CLIP_NONE; /* its path is stale now */
  files_settle();
  *sel = reload_at(*sel, r == FO_OK ? name : old);
  files_redraw(*sel);
  if (r != FO_OK) {
    files_message("Could not rename", fo_error(r));
    files_draw_bottom(*sel);
  }
}

static void op_newdir(int *sel) {
  static const char suggested[] = "New folder";
  static char name[FILES_NAME], path[FILES_PATH];
  FoResult r;

  if (!ask_name(name, suggested, suggested, cwd)) {
    files_settle();
    files_redraw(*sel);
    return;
  }
  if (!join(path, FILES_PATH, cwd, name)) {
    r = FO_TOO_DEEP;
  } else {
    /* Only the suggestion is numbered when taken; a typed name that exists is
     * an error the user should see. */
    if (same_str(name, suggested) && fo_unique(path, 1))
      for (const char *s = path; *s; s++)
        if (*s == '/' && s[1])
          copy_name(name, s + 1);
    r = fo_mkdir(path);
  }
  files_settle();
  *sel = reload_at(*sel, r == FO_OK ? name : 0);
  files_redraw(*sel);
  if (r != FO_OK) {
    files_message("Could not make the folder", fo_error(r));
    files_draw_bottom(*sel);
  }
}

static void op_delete(int *sel) {
  static char name[FILES_NAME], path[FILES_PATH];
  const FileEntry *e = &entries[*sel];
  int is_dir = e->is_dir;
  FoResult r;

  copy_name(name, e->name);
  files_paint_bottom(*sel); /* the question goes over the list, not the menu */
  if (!fv_confirm(is_dir ? "Delete folder and contents?" : "Delete this file?",
                  name, "Delete", "Cancel")) {
    files_draw_bottom(*sel);
    return;
  }
  if (!join(path, FILES_PATH, cwd, name)) {
    r = FO_TOO_DEEP;
  } else {
    progress_start("Deleting", *sel);
    r = fo_delete(path, files_progress);
  }
  if (clip_mode != CLIP_NONE && fo_within(clip_path, path))
    clip_mode = CLIP_NONE;
  files_settle();
  *sel = reload_at(*sel, 0);
  files_redraw(*sel);
  if (r != FO_OK && r != FO_CANCELLED) {
    files_message("Could not delete", fo_error(r));
    files_draw_bottom(*sel);
  }
}

static void op_clip(int sel, int mode) {
  const FileEntry *e = &entries[sel];
  if (!join(clip_path, FILES_PATH, cwd, e->name)) {
    files_message(e->name, fo_error(FO_TOO_DEEP));
    files_draw_bottom(sel);
    return;
  }
  copy_name(clip_name, e->name);
  clip_dir = e->is_dir;
  clip_mode = mode;
  files_redraw(sel);
}

static void op_paste(int *sel) {
  static char dst[FILES_PATH];
  const char *name = clip_name;
  FoResult r;

  if (!join(dst, FILES_PATH, cwd, clip_name)) {
    r = FO_TOO_DEEP;
  } else if (clip_mode == CLIP_MOVE) {
    if (fo_within(dst, clip_path) && fo_within(clip_path, dst)) {
      clip_mode = CLIP_NONE; /* already here */
      files_redraw(*sel);
      return;
    }
    r = fo_move(clip_path, dst);
  } else if (!fo_unique(dst, clip_dir)) {
    r = FO_EXISTS;
  } else {
    /* Pasted into its own folder, the copy gets a numbered name. */
    for (const char *s = dst; *s; s++)
      if (*s == '/' && s[1])
        name = s + 1;
    progress_start("Copying", *sel);
    r = fo_copy(clip_path, dst, files_progress);
  }

  if (r == FO_OK && clip_mode == CLIP_MOVE)
    clip_mode = CLIP_NONE;
  files_settle();
  *sel = reload_at(*sel, r == FO_OK ? name : 0);
  files_redraw(*sel);
  if (r != FO_OK && r != FO_CANCELLED) {
    files_message(clip_mode == CLIP_MOVE ? "Could not move" : "Could not paste",
                  fo_error(r));
    files_draw_bottom(*sel);
  }
}

static void files_run_menu(int *sel) {
  switch (files_menu(*sel)) {
    case M_COPY:   op_clip(*sel, CLIP_COPY); break;
    case M_MOVE:   op_clip(*sel, CLIP_MOVE); break;
    case M_RENAME: op_rename(sel); break;
    case M_DELETE: op_delete(sel); break;
    case M_NEWDIR: op_newdir(sel); break;
    case M_PASTE:  op_paste(sel); break;
    default:       files_draw_bottom(*sel); break;
  }
}

/* 0 when there was no card, so only a message was shown. */
static int files_run(void (*launch)(const char *path)) {
  static FATFS fs;
  int sel = 0;
  u32 frame_at = 0;

  if (f_mount(&fs, "", 1) != FR_OK) {
    ui_wallpaper(VRAM_BOT_A, BOT_SCREEN_WIDTH, BOT_SCREEN_HEIGHT,
                 BOT_SCREEN_HEIGHT);
    files_message("No SD card", "Insert a card and try again");
    return 0;
  }

  cwd[0] = '0';
  cwd[1] = ':';
  cwd[2] = '/';
  cwd[3] = 0;
  read_dir();
  files_draw_top(sel);
  files_draw_bottom(sel);

  while (1) {
    u32 k = get_keys_down();
    int prev = sel, open_now = 0, tx, ty;

    if ((k & BUTTON_DUP) && sel > 0)
      sel--;
    if ((k & BUTTON_DDOWN) && sel < entry_count - 1)
      sel++;
    if (k & BUTTON_DLEFT) {
      sel -= FILES_VISIBLE;
      if (sel < 0)
        sel = 0;
    }
    if (k & BUTTON_DRIGHT) {
      sel += FILES_VISIBLE;
      if (sel > entry_count - 1)
        sel = entry_count - 1;
      if (sel < 0)
        sel = 0;
    }

    if (touch_tap(&tx, &ty)) {
      int top = list_top(sel);
      if (touch_in(tx, ty, MENU_X, MENU_Y, MENU_W, MENU_H))
        k |= BUTTON_X;
      for (int r = 0; r < FILES_VISIBLE && top + r < entry_count; r++) {
        int y = ROW_Y0 + r * ROW_STEP;
        if (touch_in(tx, ty, ROW_X - ROW_RING, y - ROW_RING,
                     ROW_W + 2 * ROW_RING, ROW_H + 2 * ROW_RING)) {
          /* A tap selects; tapping the row already selected opens it, which
           * keeps a single tap from launching something by accident. */
          if (top + r == sel)
            open_now = 1;
          sel = top + r;
          break;
        }
      }
    }

    if (sel != prev)
      files_update(sel);

    if (k & BUTTON_X) {
      files_run_menu(&sel);
      ui_idle();
      continue;
    }

    if (((k & BUTTON_A) || open_now) && entry_count > 0) {
      const FileEntry *e = &entries[sel];
      if (e->is_dir) {
        if (!path_join(e->name)) {
          files_message(e->name, fo_error(FO_TOO_DEEP));
        } else if (!read_dir()) {
          path_up();
          read_dir();
        } else {
          anim_transition(ANIM_PUSH, ANIM_BOTH);
        }
        sel = 0;
        files_redraw(sel);
      } else if (e->kind == FKIND_AURORA && launch) {
        char path[FILES_PATH];
        int n = 0;
        /* The launcher takes a card-relative path, not a drive-qualified one. */
        for (int i = 3; cwd[i]; i++)
          path[n++] = cwd[i];
        if (n && path[n - 1] != '/')
          path[n++] = '/';
        for (int i = 0; e->name[i] && n < FILES_PATH - 1; i++)
          path[n++] = e->name[i];
        path[n] = 0;
        f_mount(NULL, "", 0);
        launch(path); /* does not return on success */
        f_mount(&fs, "", 1);
        files_redraw(sel);
      } else {
        open_media(e);
        files_redraw(sel);
      }
    }

    if (k & BUTTON_B) {
      if (!path_up())
        break;
      read_dir();
      sel = 0;
      anim_transition(ANIM_POP, ANIM_BOTH);
      files_redraw(sel);
    }
    if (k & BUTTON_START)
      break;
    int ms = anim_frame(&frame_at);
    if (ms) {
      int show = 0;
      if (anim_list_step(&flist, ms)) {
        files_paint_bottom(sel);
        show |= ANIM_BOT;
      }
      if (anim_xfade_frame(ANIM_TOP))
        show |= ANIM_TOP;
      anim_present(show);
    }
    ui_idle();
  }

  f_mount(NULL, "", 0);
  return 1;
}

void files_screen(void (*launch)(const char *path)) {
  anim_transition(ANIM_PUSH, ANIM_BOTH);
  anim_transition(files_run(launch) ? ANIM_POP : ANIM_FADE, ANIM_BOTH);
}
