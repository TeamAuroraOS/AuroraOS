/* Rich Home Menu theming: custom top/bottom images (PNG or JPEG, any size),
 * custom looping BGM (bgm.wav), custom UI SFX (select/back/move.wav), and
 * custom palette/metadata (theme.ini) under SD:/Aurora/themes/<name>/. */
#include "theme.h"
#include "ff.h"
#include "image.h"
#include "ui.h"
#include "uisound.h"

HomeTheme g_theme;

static FATFS theme_fs;

static void copy_str(char *dst, const char *src, unsigned max) {
  unsigned i = 0;
  if (!src) {
    dst[0] = '\0';
    return;
  }
  while (src[i] && i + 1 < max) {
    dst[i] = src[i];
    i++;
  }
  dst[i] = '\0';
}

static int str_eq(const char *a, const char *b) {
  while (*a && *a == *b) {
    a++;
    b++;
  }
  return *a == *b;
}

static void theme_path(char *out, const char *name, const char *file) {
  const char *p = THEME_DIR "/";
  while (*p) *out++ = *p++;
  if (name && name[0]) {
    p = name;
    while (*p) *out++ = *p++;
    *out++ = '/';
  }
  p = file;
  while (*p) *out++ = *p++;
  *out = '\0';
}

static int theme_name_valid(const char *name) {
  if (!name || !name[0] || name[0] == '@' ||
      (name[0] == '.' && (!name[1] || (name[1] == '.' && !name[2]))))
    return 0;
  for (const char *p = name; *p; p++)
    if (*p == '/' || *p == '\\' || *p == ':' || (unsigned char)*p < 0x20)
      return 0;
  return 1;
}

static int file_exists(const char *path) {
  static FILINFO fno;
  return f_stat(path, &fno) == FR_OK && !(fno.fattrib & AM_DIR);
}

static int find_image(const char *name, const char *stem, char *out) {
  static const char *const exts[] = {".png", ".jpg", ".jpeg"};
  char fname[32];
  for (int e = 0; e < 3; e++) {
    char *d = fname;
    const char *s = stem;
    while (*s) *d++ = *s++;
    s = exts[e];
    while (*s) *d++ = *s++;
    *d = '\0';
    theme_path(out, name, fname);
    if (file_exists(out))
      return 1;
  }
  out[0] = '\0';
  return 0;
}

void theme_defaults(HomeTheme *t) {
  t->active = 0;
  t->caps = 0;
  t->id[0] = '\0';
  copy_str(t->title, "Aurora", sizeof(t->title));
  t->author[0] = '\0';
  t->bgm_path[0] = '\0';
  t->sfx_select[0] = '\0';
  t->sfx_back[0] = '\0';
  t->sfx_move[0] = '\0';

  t->accent         = COLOR_AURORA;
  t->bg_top         = COLOR_HM_BG_TOP;
  t->bg_bot         = COLOR_HM_BG_BOT;
  t->panel_top      = COLOR_PANEL_TOP;
  t->panel_bot      = COLOR_PANEL_BOT;
  t->slot_top       = COLOR_HM_SLOT_TOP;
  t->slot_bot       = COLOR_HM_SLOT_BOT;
  t->slot_empty_top = COLOR_HM_EMPTY_TOP;
  t->slot_empty_bot = COLOR_HM_EMPTY_BOT;
  t->folder_top     = (Color){0x40, 0x41, 0x4A};
  t->folder_bot     = (Color){0x26, 0x27, 0x2E};
  t->status_bar     = COLOR_HM_BAR;
  t->text           = COLOR_WHITE;
  t->text_dim       = COLOR_HM_TEXT2;
}

static int hex_nib(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static int parse_hex_color(const char *s, Color *out) {
  if (*s == '#') s++;
  int d[6];
  for (int i = 0; i < 6; i++) {
    d[i] = hex_nib(s[i]);
    if (d[i] < 0) return 0;
  }
  out->r = (u8)((d[0] << 4) | d[1]);
  out->g = (u8)((d[2] << 4) | d[3]);
  out->b = (u8)((d[4] << 4) | d[5]);
  return 1;
}

static void trim(char *s) {
  int start = 0;
  while (s[start] == ' ' || s[start] == '\t') start++;
  if (start > 0) {
    int i = 0;
    while (s[start + i]) {
      s[i] = s[start + i];
      i++;
    }
    s[i] = '\0';
  }
  int len = 0;
  while (s[len]) len++;
  while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' ||
                     s[len - 1] == '\r' || s[len - 1] == '\n'))
    s[--len] = '\0';
}

static void parse_theme_ini(const char *path, HomeTheme *t) {
  static FIL f;
  static char buf[2048];
  UINT br = 0;
  if (f_open(&f, path, FA_READ) != FR_OK)
    return;
  if (f_read(&f, buf, sizeof(buf) - 1, &br) != FR_OK)
    br = 0;
  f_close(&f);
  buf[br] = '\0';

  char *p = buf;
  while (*p) {
    char *line = p;
    while (*p && *p != '\n') p++;
    if (*p == '\n') *p++ = '\0';
    trim(line);
    if (!line[0] || line[0] == '#' || line[0] == ';' || line[0] == '[')
      continue;
    char *eq = line;
    while (*eq && *eq != '=') eq++;
    if (*eq != '=') continue;
    *eq = '\0';
    char *key = line;
    char *val = eq + 1;
    trim(key);
    trim(val);

    Color c;
    if (str_eq(key, "name") && val[0]) {
      copy_str(t->title, val, sizeof(t->title));
    } else if (str_eq(key, "author")) {
      copy_str(t->author, val, sizeof(t->author));
    } else if (str_eq(key, "accent") && parse_hex_color(val, &c)) {
      t->accent = c;
      t->caps |= THEME_CAP_ACCENT | THEME_CAP_COLORS;
    } else if (str_eq(key, "bg_top") && parse_hex_color(val, &c)) {
      t->bg_top = c;
      t->caps |= THEME_CAP_COLORS;
    } else if (str_eq(key, "bg_bottom") && parse_hex_color(val, &c)) {
      t->bg_bot = c;
      t->caps |= THEME_CAP_COLORS;
    } else if (str_eq(key, "panel_top") && parse_hex_color(val, &c)) {
      t->panel_top = c;
      t->caps |= THEME_CAP_COLORS;
    } else if (str_eq(key, "panel_bottom") && parse_hex_color(val, &c)) {
      t->panel_bot = c;
      t->caps |= THEME_CAP_COLORS;
    } else if (str_eq(key, "slot_top") && parse_hex_color(val, &c)) {
      t->slot_top = c;
      t->caps |= THEME_CAP_COLORS;
    } else if (str_eq(key, "slot_bottom") && parse_hex_color(val, &c)) {
      t->slot_bot = c;
      t->caps |= THEME_CAP_COLORS;
    } else if (str_eq(key, "slot_empty_top") && parse_hex_color(val, &c)) {
      t->slot_empty_top = c;
      t->caps |= THEME_CAP_COLORS;
    } else if (str_eq(key, "slot_empty_bottom") && parse_hex_color(val, &c)) {
      t->slot_empty_bot = c;
      t->caps |= THEME_CAP_COLORS;
    } else if (str_eq(key, "folder_top") && parse_hex_color(val, &c)) {
      t->folder_top = c;
      t->caps |= THEME_CAP_COLORS;
    } else if (str_eq(key, "folder_bottom") && parse_hex_color(val, &c)) {
      t->folder_bot = c;
      t->caps |= THEME_CAP_COLORS;
    } else if (str_eq(key, "status_bar") && parse_hex_color(val, &c)) {
      t->status_bar = c;
      t->caps |= THEME_CAP_COLORS;
    } else if (str_eq(key, "text") && parse_hex_color(val, &c)) {
      t->text = c;
      t->caps |= THEME_CAP_COLORS;
    } else if (str_eq(key, "text_dim") && parse_hex_color(val, &c)) {
      t->text_dim = c;
      t->caps |= THEME_CAP_COLORS;
    }
  }
}

/* Probe a theme directory (card must already be mounted).
 * Populates `t` and top/bottom image paths. Returns 1 if the folder has at
 * least one supported theme asset (top/bottom image, bgm.wav, sfx, or theme.ini). */
static int theme_probe_dir(const char *name, HomeTheme *t, char *top_img,
                           char *bot_img) {
  char p[128];
  if (!theme_name_valid(name))
    return 0;
  theme_defaults(t);
  copy_str(t->id, name, sizeof(t->id));
  copy_str(t->title, name, sizeof(t->title));

  if (find_image(name, "top", top_img))
    t->caps |= THEME_CAP_TOP_IMG;
  if (find_image(name, "bottom", bot_img))
    t->caps |= THEME_CAP_BOT_IMG;

  theme_path(p, name, "bgm.wav");
  if (file_exists(p)) {
    copy_str(t->bgm_path, p, sizeof(t->bgm_path));
    t->caps |= THEME_CAP_BGM;
  }

  theme_path(p, name, "select.wav");
  if (file_exists(p)) {
    copy_str(t->sfx_select, p, sizeof(t->sfx_select));
    t->caps |= THEME_CAP_SFX;
  }
  theme_path(p, name, "back.wav");
  if (file_exists(p)) {
    copy_str(t->sfx_back, p, sizeof(t->sfx_back));
    t->caps |= THEME_CAP_SFX;
  }
  theme_path(p, name, "move.wav");
  if (file_exists(p)) {
    copy_str(t->sfx_move, p, sizeof(t->sfx_move));
    t->caps |= THEME_CAP_SFX;
  }

  theme_path(p, name, "theme.ini");
  if (file_exists(p))
    parse_theme_ini(p, t);

  return t->caps != 0u;
}

static int theme_mount_prepare(void) {
  if (f_mount(&theme_fs, "", 1) != FR_OK)
    return 0;
  f_mkdir("Aurora");
  f_mkdir(THEME_DIR);
  return 1;
}

/* Load and apply theme `name` (card must be mounted). */
static int theme_apply_mounted(const char *name, Color fallback_accent) {
  HomeTheme next;
  char top_img[128], bot_img[128];

  if (!name || !name[0]) {
    theme_defaults(&g_theme);
    g_accent = fallback_accent;
    ui_theme_clear();
    ui_sound_set_theme("", "", "", "");
    return 1;
  }

  if (!theme_probe_dir(name, &next, top_img, bot_img))
    return 0;

  next.active = 1;
  g_theme = next;
  g_accent = (next.caps & THEME_CAP_ACCENT) ? next.accent : fallback_accent;

  ui_theme_load_ex(top_img[0] ? top_img : 0, bot_img[0] ? bot_img : 0,
                   next.bg_top, next.bg_bot);
  ui_sound_set_theme(next.bgm_path, next.sfx_select, next.sfx_back,
                     next.sfx_move);
  return 1;
}

void theme_boot_load(Color fallback_accent) {
  static FIL f;
  char saved[THEME_NAME_MAX];
  UINT br = 0;

  theme_defaults(&g_theme);
  if (!theme_mount_prepare())
    return;
  if (f_open(&f, THEME_CONFIG, FA_READ) == FR_OK) {
    if (f_read(&f, saved, sizeof(saved) - 1, &br) != FR_OK)
      br = 0;
    f_close(&f);
  }
  saved[br] = '\0';
  while (br && (saved[br - 1] == '\r' || saved[br - 1] == '\n'))
    saved[--br] = '\0';
  if (saved[0] && !str_eq(saved, THEME_BUILTIN))
    theme_apply_mounted(saved, fallback_accent);
  f_mount(NULL, "", 0);
}

int theme_select(const char *id, Color fallback_accent) {
  static FIL f;
  char saved[THEME_NAME_MAX + 1];
  const char *keep = (id && id[0]) ? id : THEME_BUILTIN;
  UINT len = 0, bw;

  if (!theme_mount_prepare())
    return 0;

  int ok = theme_apply_mounted(id, fallback_accent);
  if (ok) {
    while (keep[len] && len < THEME_NAME_MAX - 1) {
      saved[len] = keep[len];
      len++;
    }
    saved[len++] = '\n';
    if (f_open(&f, THEME_CONFIG, FA_CREATE_ALWAYS | FA_WRITE) == FR_OK) {
      f_write(&f, saved, len, &bw);
      f_close(&f);
    }
  }
  f_mount(NULL, "", 0);
  if (ok)
    ui_sound_reload_theme();
  return ok;
}

int theme_scan_entries(ThemeEntry *out, int max) {
  static DIR dir;
  static FILINFO info;
  int n = 0;
  if (max <= 0)
    return 0;

  out[0].id[0] = '\0';
  copy_str(out[0].title, "Aurora (Default)", sizeof(out[0].title));
  copy_str(out[0].author, "Built-in", sizeof(out[0].author));
  out[0].caps = 0;
  n = 1;

  if (!theme_mount_prepare())
    return n;

  if (f_opendir(&dir, THEME_DIR) == FR_OK) {
    while (n < max && f_readdir(&dir, &info) == FR_OK && info.fname[0]) {
      HomeTheme probe;
      char top_img[128], bot_img[128];
      if (!(info.fattrib & AM_DIR) || !theme_name_valid(info.fname))
        continue;
      if (!theme_probe_dir(info.fname, &probe, top_img, bot_img))
        continue;
      copy_str(out[n].id, probe.id, sizeof(out[n].id));
      copy_str(out[n].title, probe.title, sizeof(out[n].title));
      copy_str(out[n].author, probe.author, sizeof(out[n].author));
      out[n].caps = probe.caps;
      n++;
    }
    f_closedir(&dir);
  }
  f_mount(NULL, "", 0);
  return n;
}
