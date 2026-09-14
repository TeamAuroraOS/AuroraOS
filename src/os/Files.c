/* File Explorer. FatFs is built without long file names, so names are 8.3. A
 * directory is read once and sorted rather than re-read while scrolling, since
 * f_readdir is slow; a directory too big for the array is truncated and marked. */

#include "files.h"
#include "fileview.h"
#include "assets.h"
#include "ui.h"
#include "container.h"
#include "ff.h"
#include "icons.h"
#include "image.h"
#include "lang.h"
#include "touch.h"
#include "wav.h"
#include "audio.h"
#include <string.h>

#define FILES_MAX     256
#define FILES_NAME    16  /* 8.3 plus the dot and a terminator */
#define FILES_PATH    192
#define FILES_VISIBLE 5

#define ROW_X    10
#define ROW_W    (BOT_SCREEN_WIDTH - 20)
#define ROW_H    30
#define ROW_STEP 34
#define ROW_Y0   40
/* At ROW_STEP 34 and ROW_H 30 a 2px ring makes neighbouring rings just touch.
 * Wider rings overlap, and a two-row update would no longer match a full
 * redraw. */
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

/* An AOS1/AUR1 container looks like any other .bin, so the magic decides. */
static int is_aurora_container(const char *dir, const char *name) {
  static FIL f;
  char path[FILES_PATH + FILES_NAME];
  char magic[4];
  UINT br;
  int n = 0;

  while (dir[n] && n < FILES_PATH - 1) {
    path[n] = dir[n];
    n++;
  }
  if (n && path[n - 1] != '/')
    path[n++] = '/';
  for (int i = 0; name[i] && n < (int)sizeof(path) - 1; i++)
    path[n++] = name[i];
  path[n] = 0;

  if (f_open(&f, path, FA_READ) != FR_OK)
    return 0;
  if (f_read(&f, magic, 4, &br) != FR_OK || br != 4) {
    f_close(&f);
    return 0;
  }
  f_close(&f);
  return (magic[0] == 'A' && magic[1] == 'O' && magic[2] == 'S' &&
          magic[3] == '1') ||
         (magic[0] == 'A' && magic[1] == 'U' && magic[2] == 'R' &&
          magic[3] == '1');
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
  for (int i = 1; i < entry_count; i++) {
    FileEntry key = entries[i];
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
    int n = 0;
    while (fno.fname[n] && n < FILES_NAME - 1) {
      e->name[n] = fno.fname[n];
      n++;
    }
    e->name[n] = 0;
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

static void path_join(const char *name) {
  int n = 0;
  while (cwd[n])
    n++;
  if (n && cwd[n - 1] != '/' && n < FILES_PATH - 1)
    cwd[n++] = '/';
  for (int i = 0; name[i] && n < FILES_PATH - 1; i++)
    cwd[n++] = name[i];
  cwd[n] = 0;
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

/* Long paths lose their middle, keeping the drive and the current folder. */
static void fit_path(char *out, int outsz, const char *path, const Font *f,
                     int avail) {
  int n = 0;
  while (path[n])
    n++;
  if (ui_tw(f, path) <= avail || n < 12) {
    int i = 0;
    while (path[i] && i < outsz - 1) {
      out[i] = path[i];
      i++;
    }
    out[i] = 0;
    return;
  }
  int keep = 0;
  for (int i = n - 1; i > 3; i--)
    if (path[i] == '/') {
      keep = i;
      break;
    }
  if (!keep) { /* one very long component: clip it rather than elide */
    int i = 0;
    while (path[i] && i < outsz - 1) {
      out[i] = path[i];
      i++;
    }
    out[i] = 0;
    return;
  }
  int w = 0;
  out[w++] = path[0];
  out[w++] = path[1];
  out[w++] = path[2];
  out[w++] = '.';
  out[w++] = '.';
  out[w++] = '.';
  for (int i = keep; path[i] && w < outsz - 1; i++)
    out[w++] = path[i];
  out[w] = 0;
}

#define ICON_BOX  96
#define ICON_Y    34
#define CARD_X    40
#define CARD_Y    148
#define CARD_W    (TOP_SCREEN_WIDTH - 80)
#define CARD_H    62

/* Everything on the top screen that only changes with the directory, kept apart
 * so a cursor move repaints two small boxes. */
static void files_top_static(void) {
  ui_wallpaper(VRAM_TOP_LA, TOP_SCREEN_WIDTH, TOP_SCREEN_HEIGHT,
               TOP_SCREEN_HEIGHT);
  draw_filled_rect(VRAM_TOP_LA, 0, 0, TOP_SCREEN_WIDTH, 24, TOP_SCREEN_HEIGHT,
                   COLOR_HM_BAR);
  ui_text(VRAM_TOP_LA, 10, 4, TOP_SCREEN_HEIGHT, "Files", COLOR_WHITE,
          COLOR_HM_BAR, &ui_bold);

  char shown[64];
  fit_path(shown, sizeof(shown), cwd, &ui_small, TOP_SCREEN_WIDTH - 130);
  ui_text(VRAM_TOP_LA, 120, 6, TOP_SCREEN_HEIGHT, shown, COLOR_HM_TEXT2,
          COLOR_HM_BAR, &ui_small);
}

/* Both areas are repainted first: the icon and the card corners blend, so
 * drawing over the last frame would smear. */
static void files_top_item(int sel) {
  if (entry_count <= 0) {
    ui_wallpaper_rect(VRAM_TOP_LA, 0, 24, TOP_SCREEN_WIDTH,
                      TOP_SCREEN_HEIGHT - 24, TOP_SCREEN_HEIGHT);
    ui_text_mid(VRAM_TOP_LA, TOP_SCREEN_WIDTH / 2, 110, TOP_SCREEN_HEIGHT,
                "Empty folder", COLOR_HM_TEXT2, COLOR_HM_BG_BOT, &ui_title);
    return;
  }

  const FileEntry *e = &entries[sel];
  ui_wallpaper_rect(VRAM_TOP_LA, (TOP_SCREEN_WIDTH - ICON_BOX) / 2, ICON_Y,
                    ICON_BOX, ICON_BOX, TOP_SCREEN_HEIGHT);
  ui_icon(VRAM_TOP_LA, (TOP_SCREEN_WIDTH - ICON_BOX) / 2, ICON_Y, ICON_BOX,
          TOP_SCREEN_HEIGHT, kind_asset(e->kind, 1), icon_boot_bits,
          COLOR_WHITE);

  ui_patch_round_rect(VRAM_TOP_LA, CARD_X, CARD_Y, CARD_W, CARD_H, 11, 0,
                      TOP_SCREEN_HEIGHT);
  draw_gradient_round_rect(VRAM_TOP_LA, CARD_X, CARD_Y, CARD_W, CARD_H, 11,
                           TOP_SCREEN_HEIGHT, COLOR_PANEL_TOP, COLOR_PANEL_BOT);
  ui_text_mid(VRAM_TOP_LA, TOP_SCREEN_WIDTH / 2, CARD_Y + 7, TOP_SCREEN_HEIGHT,
              e->name, COLOR_WHITE, COLOR_PANEL_TOP, &ui_title);

  char detail[48];
  const char *label = kind_label(e->kind, e->name);
  int w = 0;
  while (label[w] && w < 24) {
    detail[w] = label[w];
    w++;
  }
  if (!e->is_dir) {
    detail[w++] = ' ';
    detail[w++] = '-';
    detail[w++] = ' ';
    fmt_size(detail + w, e->size);
  } else {
    detail[w] = 0;
  }
  ui_text_mid(VRAM_TOP_LA, TOP_SCREEN_WIDTH / 2, CARD_Y + 31, TOP_SCREEN_HEIGHT,
              detail, COLOR_HM_TEXT2, COLOR_PANEL_BOT, &ui_font);
}

static void files_draw_top(int sel) {
  files_top_static();
  files_top_item(sel);
  screen_present_top();
}

static int list_top(int sel) {
  int top = sel - FILES_VISIBLE / 2;
  if (top > entry_count - FILES_VISIBLE)
    top = entry_count - FILES_VISIBLE;
  if (top < 0)
    top = 0;
  return top;
}

/* Background included, so a row can be repainted on its own. */
static void files_row(int r, int i, int selected) {
  int y = ROW_Y0 + r * ROW_STEP;
  const FileEntry *e = &entries[i];
  char right[16];

  /* The ring is drawn over the background, so a row losing it needs that
   * frame back; the card below covers everything except its own corners. */
  if (selected)
    draw_filled_round_rect(VRAM_BOT_A, ROW_X - ROW_RING, y - ROW_RING,
                           ROW_W + 2 * ROW_RING, ROW_H + 2 * ROW_RING, 10,
                           BOT_SCREEN_HEIGHT, g_accent);
  else
    ui_patch_round_rect(VRAM_BOT_A, ROW_X, y, ROW_W, ROW_H, 8, ROW_RING,
                        BOT_SCREEN_HEIGHT);

  draw_gradient_round_rect(VRAM_BOT_A, ROW_X, y, ROW_W, ROW_H, 8,
                           BOT_SCREEN_HEIGHT, COLOR_HM_SLOT_TOP,
                           COLOR_HM_SLOT_BOT);
  ui_icon(VRAM_BOT_A, ROW_X + 4, y, ROW_H, BOT_SCREEN_HEIGHT,
          kind_asset(e->kind, 0), icon_boot_bits, COLOR_WHITE);
  ui_text(VRAM_BOT_A, ROW_X + 40, y + (ROW_H - ui_th(&ui_font)) / 2,
          BOT_SCREEN_HEIGHT, e->name, COLOR_WHITE, COLOR_HM_SLOT, &ui_font);

  if (e->is_dir) {
    right[0] = '>';
    right[1] = 0;
  } else {
    fmt_size(right, e->size);
  }
  ui_text(VRAM_BOT_A, ROW_X + ROW_W - 10 - ui_tw(&ui_small, right),
          y + (ROW_H - ui_th(&ui_small)) / 2, BOT_SCREEN_HEIGHT, right,
          COLOR_HM_TEXT2, COLOR_HM_SLOT, &ui_small);
}

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
  ui_text(VRAM_BOT_A, ROW_X, BOT_SCREEN_HEIGHT - 22, BOT_SCREEN_HEIGHT, foot,
          COLOR_HM_TEXT2, COLOR_HM_BG_BOT, &ui_small);
}

static void files_draw_bottom(int sel) {
  ui_wallpaper(VRAM_BOT_A, BOT_SCREEN_WIDTH, BOT_SCREEN_HEIGHT,
               BOT_SCREEN_HEIGHT);
  draw_filled_round_rect(VRAM_BOT_A, 8, 6, BOT_SCREEN_WIDTH - 16, 26, 9,
                         BOT_SCREEN_HEIGHT, COLOR_HM_BAR);
  char shown[64];
  fit_path(shown, sizeof(shown), cwd, &ui_font, BOT_SCREEN_WIDTH - 34);
  ui_text(VRAM_BOT_A, 18, 10, BOT_SCREEN_HEIGHT, shown, COLOR_WHITE,
          COLOR_HM_BAR, &ui_font);

  if (entry_count <= 0) {
    ui_text_mid(VRAM_BOT_A, BOT_SCREEN_WIDTH / 2, 110, BOT_SCREEN_HEIGHT,
                "Nothing here", COLOR_HM_TEXT2, COLOR_HM_BG_BOT, &ui_font);
    ui_text_mid(VRAM_BOT_A, BOT_SCREEN_WIDTH / 2, 132, BOT_SCREEN_HEIGHT,
                "B: back", COLOR_HM_TEXT2, COLOR_HM_BG_BOT, &ui_small);
    screen_present_bottom();
    return;
  }

  int top = list_top(sel);
  for (int r = 0; r < FILES_VISIBLE && top + r < entry_count; r++)
    files_row(r, top + r, top + r == sel);

  files_footer(sel);
  ui_text(VRAM_BOT_A, BOT_SCREEN_WIDTH - 118, BOT_SCREEN_HEIGHT - 22,
          BOT_SCREEN_HEIGHT, "A: open   B: up", COLOR_HM_TEXT2, COLOR_HM_BG_BOT,
          &ui_small);
  screen_present_bottom();
}

/* Moving the cursor inside the visible window touches exactly two rows; only a
 * scroll needs the whole list again. */
static void files_update(int old_sel, int new_sel) {
  int top_old = list_top(old_sel), top_new = list_top(new_sel);
  if (top_old != top_new || entry_count <= 0) {
    files_draw_bottom(new_sel);
  } else {
    files_row(old_sel - top_new, old_sel, 0);
    files_row(new_sel - top_new, new_sel, 1);
    files_footer(new_sel);
    screen_present_bottom();
  }
  files_top_item(new_sel);
  screen_present_top();
}

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
      return;
    ui_idle();
  }
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

  draw_filled_rect(VRAM_TOP_LA, 0, 0, TOP_SCREEN_WIDTH, TOP_SCREEN_HEIGHT,
                   TOP_SCREEN_HEIGHT, COLOR_HM_BG);
  image_draw_fit(VRAM_TOP_LA, 0, 24, TOP_SCREEN_WIDTH, TOP_SCREEN_HEIGHT - 24,
                 TOP_SCREEN_HEIGHT, img);
  draw_filled_rect(VRAM_TOP_LA, 0, 0, TOP_SCREEN_WIDTH, 24, TOP_SCREEN_HEIGHT,
                   COLOR_HM_BAR);
  ui_text(VRAM_TOP_LA, 10, 4, TOP_SCREEN_HEIGHT, name, COLOR_WHITE,
          COLOR_HM_BAR, &ui_bold);
  ui_text(VRAM_TOP_LA, TOP_SCREEN_WIDTH - 12 - ui_tw(&ui_small, detail), 6,
          TOP_SCREEN_HEIGHT, detail, COLOR_HM_TEXT2, COLOR_HM_BAR, &ui_small);
  screen_present_top();

  while (1) {
    u32 k = get_keys_down();
    int tx, ty;
    if ((k & (BUTTON_A | BUTTON_B)) || touch_tap(&tx, &ty))
      return;
    ui_idle();
  }
}

static void entry_path(char *out, int outsz, const char *name) {
  int n = 0;
  while (cwd[n] && n < outsz - 1)
    out[n] = cwd[n], n++;
  if (n && out[n - 1] != '/' && n < outsz - 1)
    out[n++] = '/';
  for (int i = 0; name[i] && n < outsz - 1; i++)
    out[n++] = name[i];
  out[n] = 0;
}

static void open_media(const FileEntry *e) {
  char path[FILES_PATH + FILES_NAME];
  entry_path(path, (int)sizeof(path), e->name);

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
        *p++ = ' ';
        *p++ = 'H';
        *p++ = 'z';
        *p++ = ' ';
        p = put_u32(p, depth);
        *p++ = '-';
        *p++ = 'b';
        *p++ = 'i';
        *p++ = 't';
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

void files_screen(void (*launch)(const char *path)) {
  static FATFS fs;
  int sel = 0;

  if (f_mount(&fs, "", 1) != FR_OK) {
    ui_wallpaper(VRAM_BOT_A, BOT_SCREEN_WIDTH, BOT_SCREEN_HEIGHT,
                 BOT_SCREEN_HEIGHT);
    files_message("No SD card", "Insert a card and try again");
    return;
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

    if (touch_tap(&tx, &ty) && entry_count > 0) {
      int top = list_top(sel);
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
      files_update(prev, sel);

    if (((k & BUTTON_A) || open_now) && entry_count > 0) {
      const FileEntry *e = &entries[sel];
      if (e->is_dir) {
        path_join(e->name);
        if (!read_dir()) {
          path_up();
          read_dir();
        }
        sel = 0;
        files_draw_bottom(sel);
        files_draw_top(sel);
      } else if (e->kind == FKIND_AURORA && launch) {
        char path[FILES_PATH + FILES_NAME];
        int n = 0;
        /* The launcher takes a card-relative path, not a drive-qualified one. */
        for (int i = 3; cwd[i]; i++)
          path[n++] = cwd[i];
        if (n && path[n - 1] != '/')
          path[n++] = '/';
        for (int i = 0; e->name[i]; i++)
          path[n++] = e->name[i];
        path[n] = 0;
        f_mount(NULL, "", 0);
        launch(path); /* does not return on success */
        f_mount(&fs, "", 1);
        files_draw_bottom(sel);
        files_draw_top(sel);
      } else {
        open_media(e);
        files_draw_bottom(sel);
        files_draw_top(sel);
      }
    }

    if (k & BUTTON_B) {
      if (!path_up())
        break;
      read_dir();
      sel = 0;
      files_draw_bottom(sel);
      files_draw_top(sel);
    }
    if (k & BUTTON_START)
      break;
    ui_idle();
  }

  f_mount(NULL, "", 0);
}
