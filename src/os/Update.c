/* System updates: Settings > System Update, the Terminal's update command
 * (TermCmds.c) and the clean-up at every start. The OS asks the update
 * server about the newest release, fetches the updater (updater/, a C SDK
 * app) and starts it; the updater installs. See docs/update.md. */
#include "update.h"
#include "anim.h"
#include "appnet.h"
#include "assets.h"
#include "container.h"
#include "ff.h"
#include "fileview.h"
#include "json.h"
#include "loader.h"
#include "nand.h"
#include "power.h"
#include "statusbar.h"
#include "term.h"
#include "touch.h"
#include "ui.h"
#include "uisound.h"
#include "wifi.h"
#include <string.h>

#define UP_NOTES_FILE UPDATE_DIR "/notes.txt"
#define UP_LIST_MAX   3

static char up_notes[6400];
static JsonTok up_tok[160];
static char up_url[256];
static u8 up_buf[4096];

static char *up_cpy(char *d, const char *s) {
  while ((*d = *s)) {
    d++;
    s++;
  }
  return d;
}

static char *up_num(char *d, u32 v) {
  char t[12];
  int n = 0;
  do {
    t[n++] = (char)('0' + v % 10u);
    v /= 10u;
  } while (v);
  while (n)
    *d++ = t[--n];
  *d = 0;
  return d;
}

/* "1.4 MB", "820 KB". */
static char *up_size(char *d, u32 bytes) {
  if (bytes >= 1024u * 1024u) {
    u32 tenths = (u32)(((u64)bytes * 10u + 512u * 1024u) / (1024u * 1024u));
    d = up_num(d, tenths / 10u);
    *d++ = '.';
    d = up_num(d, tenths % 10u);
    return up_cpy(d, " MB");
  }
  d = up_num(d, (bytes + 1023u) / 1024u);
  return up_cpy(d, " KB");
}

static void up_why(char *why, u32 max, const char *a, const char *b) {
  u32 n = 0;
  for (const char *s = a; s && *s && n + 1u < max; s++)
    why[n++] = *s;
  for (const char *s = b; s && *s && n + 1u < max; s++)
    why[n++] = *s;
  if (max)
    why[n] = 0;
}

static int up_prefix(const char *s, const char *pre) {
  while (*pre)
    if (*s++ != *pre++)
      return 0;
  return 1;
}

static int up_exists(const char *path) {
  FILINFO fno;
  return f_stat(path, &fno) == FR_OK && !(fno.fattrib & AM_DIR);
}

static int up_write(const char *path, const char *text) {
  static FIL f;
  UINT n = (UINT)strlen(text), bw = 0;
  if (f_open(&f, path, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK)
    return 0;
  FRESULT fr = f_write(&f, text, n, &bw);
  if (f_close(&f) != FR_OK)
    fr = FR_DISK_ERR;
  return fr == FR_OK && bw == n;
}

/* A small key=value file into `buf`; 0 when it is not there. */
static int up_read(const char *path, char *buf, u32 max) {
  static FIL f;
  UINT br = 0;
  if (f_open(&f, path, FA_READ) != FR_OK)
    return 0;
  if (f_read(&f, buf, max - 1u, &br) != FR_OK)
    br = 0;
  f_close(&f);
  buf[br] = 0;
  return 1;
}

/* The value of `key` ("name=") in key=value lines, to the end of its line. */
static void up_value(const char *buf, const char *key, char *out, u32 max) {
  u32 k = (u32)strlen(key);
  out[0] = 0;
  for (const char *l = buf; *l;) {
    if (up_prefix(l, key)) {
      u32 n = 0;
      for (l += k; *l && *l != '\r' && *l != '\n' && n + 1u < max; l++)
        out[n++] = *l;
      out[n] = 0;
      return;
    }
    while (*l && *l != '\n')
      l++;
    if (*l)
      l++;
  }
}

static int up_hex_eq(const u8 d[32], const char *hex) {
  static const char digits[] = "0123456789abcdef";
  for (int i = 0; i < 32; i++) {
    char h = hex[2 * i], l = hex[2 * i + 1];
    if (h >= 'A' && h <= 'F')
      h = (char)(h + 32);
    if (l >= 'A' && l <= 'F')
      l = (char)(l + 32);
    if (h != digits[d[i] >> 4] || l != digits[d[i] & 15])
      return 0;
  }
  return hex[64] == 0;
}

static int up_file_sha(const char *path, u8 digest[32], u32 *size) {
  static FIL f;
  Sha256 s;
  UINT br;
  if (f_open(&f, path, FA_READ) != FR_OK)
    return 0;
  sha256_init(&s);
  *size = 0;
  for (;;) {
    if (f_read(&f, up_buf, sizeof(up_buf), &br) != FR_OK) {
      f_close(&f);
      return 0;
    }
    if (!br)
      break;
    sha256_update(&s, up_buf, br);
    *size += br;
  }
  f_close(&f);
  sha256_final(&s, digest);
  return 1;
}

int update_battery_ok(void) {
  if (battery_charging())
    return 1;
  int pct = battery_percent();
  return pct < 0 || pct >= UPDATE_MIN_BATTERY; /* -1: the MCU did not say */
}

const char *update_notes(void) { return up_notes; }

int update_check(UpdateInfo *u, char *why, u32 max) {
  AppNetReply r;
  Json j;
  memset(u, 0, sizeof(*u));
  up_notes[0] = 0;
  char *p = up_cpy(up_url, UPDATE_SERVER UPDATE_LATEST "?current=");
  p += appnet_url_encode(p, 64, AURORA_VERSION_NUM);
  p = up_cpy(p, "&build=");
  up_num(p, AURORA_BUILD_DATE);

  int status = appnet_http("GET", up_url, 0, 0, 0, &r);
  if (!status) {
    up_why(why, max, appnet_error(), 0);
    return 0;
  }
  if (status == 503) {
    up_why(why, max, "No release is ready on the update server yet.", 0);
    return 0;
  }
  if (status != 200) {
    char code[12];
    up_num(code, (u32)status);
    up_why(why, max, "The update server answered HTTP ", code);
    return 0;
  }
  if (!json_parse(&j, r.body, r.len, up_tok,
                  sizeof(up_tok) / sizeof(up_tok[0])) ||
      j.tok[0].type != JSON_OBJ) {
    up_why(why, max, "The update server's reply could not be read.", 0);
    return 0;
  }
  u->update = json_bool(&j, json_get(&j, 0, "update"), 0);
  json_text(&j, json_get(&j, 0, "name"), u->name, sizeof(u->name));
  json_str(&j, json_get(&j, 0, "published"), u->published,
           sizeof(u->published));
  if (!u->update)
    return 1;

  int a = json_get(&j, 0, "updater");
  int size = json_int(&j, json_get(&j, 0, "size"), 0);
  u->size = size > 0 ? (u32)size : 0;
  if (a >= 0 && j.tok[a].type == JSON_OBJ) {
    json_str(&j, json_get(&j, a, "url"), u->app_url, sizeof(u->app_url));
    json_str(&j, json_get(&j, a, "sha256"), u->app_sha256,
             sizeof(u->app_sha256));
    int n = json_int(&j, json_get(&j, a, "size"), 0);
    u->app_size = n > 0 ? (u32)n : 0;
  }
  if (!up_prefix(u->app_url, "https://") || strlen(u->app_sha256) != 64 ||
      !u->app_size || u->app_size > AURORA_APP_MAX_SIZE || !u->name[0]) {
    u->update = 0;
    up_why(why, max, "The update server's reply is incomplete.", 0);
    return 0;
  }
  json_text(&j, json_get(&j, 0, "notes"), up_notes, sizeof(up_notes));
  return 1;
}

/* Where the firm that booted this OS lives, on the mounted card: the path it
 * reported (AURORA_FIRM_INFO_ADDR), else the usual Luma3DS payload when it is
 * there, else 0. */
static const char *up_firm_path(void) {
  static char path[AURORA_FIRM_PATH_MAX];
  const volatile u32 *magic = (const volatile u32 *)AURORA_FIRM_INFO_ADDR;
  const volatile char *s = (const volatile char *)(AURORA_FIRM_INFO_ADDR + 4u);
  if (*magic == AURORA_FIRM_INFO_MAGIC && s[0] == '/') {
    u32 n = 0;
    while (n + 1u < sizeof(path) && (u8)s[n] >= 0x20 && s[n] != 0x7F) {
      path[n] = s[n];
      n++;
    }
    path[n] = 0;
    if (!s[n] && up_exists(path))
      return path;
  }
  return up_exists(UPDATE_DEFAULT_FIRM) ? UPDATE_DEFAULT_FIRM : 0;
}

/* What the updater needs from this OS. */
static int up_write_plan(void) {
  static char plan[AURORA_FIRM_PATH_MAX + 160];
  const char *firm = up_firm_path();
  char *p = up_cpy(plan, "current=" AURORA_VERSION_NUM "\r\nbuild=");
  p = up_num(p, AURORA_BUILD_DATE);
  p = up_cpy(p, "\r\nfrom=" AURORA_VERSION "\r\nfirm=");
  p = up_cpy(p, firm ? firm : "");
  up_cpy(p, "\r\n");
  return up_write(UPDATE_PLAN, plan);
}

static int up_app_ok(const char *path) {
  static FIL f;
  aos_header_t h;
  if (f_open(&f, path, FA_READ) != FR_OK)
    return 0;
  int ok = aurora_parse_header(&f, &h) == AURORA_OK &&
           aurora_magic_kind(h.magic) != 0 &&
           h.arm9_load_addr == AURORA_APP_LOAD_ADDR && h.arm9_size &&
           h.arm9_size <= AURORA_APP_MAX_SIZE;
  f_close(&f);
  return ok;
}

int update_fetch(const UpdateInfo *u, void (*progress)(u32 done, u32 total),
                 char *why, u32 max) {
  u8 digest[32];
  u32 size = 0;
  if (!update_battery_ok()) {
    up_why(why, max, "Charge the battery past 20% or plug in the charger.", 0);
    return 0;
  }
  if (!appnet_mount()) {
    up_why(why, max, "The SD card could not be read.", 0);
    return 0;
  }
  f_mkdir("/Aurora");
  f_mkdir(UPDATE_DIR);
  f_unlink(UPDATE_APP);
  if (!appnet_download(u->app_url, 0, UPDATE_APP, progress)) {
    up_why(why, max, appnet_error(), 0);
    return 0;
  }
  if (!up_file_sha(UPDATE_APP, digest, &size) || size != u->app_size ||
      !up_hex_eq(digest, u->app_sha256) || !up_app_ok(UPDATE_APP)) {
    f_unlink(UPDATE_APP);
    up_why(why, max, "The updater did not download correctly. Try again.", 0);
    return 0;
  }
  if (!up_write_plan()) {
    f_unlink(UPDATE_APP);
    up_why(why, max, "Could not write to the SD card.", 0);
    return 0;
  }
  return 1;
}

/* The files a backup can hold, with where each goes back to. */
typedef struct {
  const char *live;
  const char *backup;
  const char *swap;
} UpItem;

static int up_items(UpItem *items, char *firm, u32 firm_max) {
  static char info[AURORA_FIRM_PATH_MAX + 96];
  int n = 0;
  items[n++] = (UpItem){UPDATE_OS_PATH, UPDATE_BACKUP_DIR "/AURORAOS.BIN",
                        UPDATE_BACKUP_DIR "/AURORAOS.swp"};
  items[n++] = (UpItem){UPDATE_ASSETS_PATH, UPDATE_BACKUP_DIR "/assets.pak",
                        UPDATE_BACKUP_DIR "/assets.swp"};
  firm[0] = 0;
  if (up_read(UPDATE_BACKUP_INFO, info, sizeof(info)))
    up_value(info, "firm=", firm, firm_max);
  if (!firm[0]) {
    const char *f = up_firm_path();
    up_why(firm, firm_max, f, 0);
  }
  if (firm[0])
    items[n++] = (UpItem){firm, UPDATE_BACKUP_DIR "/Aurora.firm",
                          UPDATE_BACKUP_DIR "/Aurora.swp"};
  return n;
}

int update_backup(char *name, u32 max) {
  static char info[AURORA_FIRM_PATH_MAX + 96];
  static char firm[AURORA_FIRM_PATH_MAX];
  UpItem items[UP_LIST_MAX];
  int found = 0;
  name[0] = 0;
  if (!appnet_mount())
    return 0;
  int n = up_items(items, firm, sizeof(firm));
  for (int i = 0; i < n; i++)
    found |= up_exists(items[i].backup);
  if (found && up_read(UPDATE_BACKUP_INFO, info, sizeof(info)))
    up_value(info, "name=", name, max);
  if (found && !name[0])
    up_why(name, max, "the previous version", 0);
  return found;
}

/* Trades the live file and its backup; doing it twice undoes it. */
static int up_swap(const UpItem *it) {
  int had = up_exists(it->live);
  f_unlink(it->swap);
  if (had && f_rename(it->live, it->swap) != FR_OK)
    return 0;
  if (f_rename(it->backup, it->live) != FR_OK) {
    if (had)
      f_rename(it->swap, it->live);
    return 0;
  }
  if (had && f_rename(it->swap, it->backup) != FR_OK) {
    f_rename(it->live, it->backup);
    f_rename(it->swap, it->live);
    return 0;
  }
  return 1;
}

int update_restore(char *why, u32 max) {
  static char firm[AURORA_FIRM_PATH_MAX];
  static char info[AURORA_FIRM_PATH_MAX + 96];
  UpItem items[UP_LIST_MAX];
  int done[UP_LIST_MAX] = {0};
  if (!appnet_mount()) {
    up_why(why, max, "The SD card could not be read.", 0);
    return 0;
  }
  int n = up_items(items, firm, sizeof(firm));
  if (!up_exists(items[0].backup)) {
    up_why(why, max, "The backup has no AURORAOS.BIN.", 0);
    return 0;
  }
  for (int i = 0; i < n; i++) {
    if (!up_exists(items[i].backup))
      continue;
    if (!up_swap(&items[i])) {
      while (i--)
        if (done[i])
          up_swap(&items[i]);
      up_why(why, max, "Could not move the files on the SD card.", 0);
      return 0;
    }
    done[i] = 1;
  }
  /* The backup now holds this version. */
  char *p = up_cpy(info, "name=" AURORA_VERSION "\r\nfirm=");
  p = up_cpy(p, firm);
  up_cpy(p, "\r\n");
  up_write(UPDATE_BACKUP_INFO, info);
  return 1;
}

/* Every file in `dir`, then `dir`. */
static void up_clear_dir(const char *dir) {
  static DIR d;
  static FILINFO fno;
  static char path[64 + FF_LFN_BUF];
  if (f_opendir(&d, dir) != FR_OK)
    return;
  while (f_readdir(&d, &fno) == FR_OK && fno.fname[0]) {
    if (fno.fattrib & AM_DIR)
      continue;
    char *p = up_cpy(path, dir);
    *p++ = '/';
    up_cpy(p, fno.fname);
    f_unlink(path);
  }
  f_closedir(&d);
  f_unlink(dir);
}

int update_at_boot(const char **title, const char **line) {
  static FATFS fs;
  static char result[512];
  static char t[96], l[200];
  static char ok[4], to[48], why[160], note[160];
  FILINFO fno;
  int have = 0;
  if (f_mount(&fs, "", 1) != FR_OK) {
    f_mount(NULL, "", 0);
    return 0;
  }
  if (f_stat(UPDATE_DIR, &fno) == FR_OK) {
    f_unlink(UPDATE_APP);
    f_unlink(UPDATE_APP ".part");
    f_unlink(UPDATE_PLAN);
    f_unlink(UP_NOTES_FILE);
    up_clear_dir(UPDATE_NEW_DIR);
    if (up_read(UPDATE_RESULT, result, sizeof(result))) {
      f_unlink(UPDATE_RESULT);
      up_value(result, "ok=", ok, sizeof(ok));
      up_value(result, "to=", to, sizeof(to));
      up_value(result, "why=", why, sizeof(why));
      up_value(result, "note=", note, sizeof(note));
      if (ok[0] == '1') {
        up_why(t, sizeof(t), "Updated to ", to[0] ? to : AURORA_VERSION);
        up_why(l, sizeof(l),
               note[0] ? note
                       : "Settings > System Update can restore the last "
                         "version.",
               0);
      } else {
        up_why(t, sizeof(t), "The update did not finish", 0);
        up_why(l, sizeof(l), why[0] ? why : "Nothing was changed.", 0);
      }
      *title = t;
      *line = l;
      have = 1;
    }
  }
  f_mount(NULL, "", 0);
  return have;
}

/* Settings > System Update. */

#define ROW_X    12
#define ROW_W    (BOT_SCREEN_WIDTH - 24)
#define ROW_H    34
#define ROW_STEP 42
#define ROW_Y0   8
#define SPIN_Y   136

enum { UP_CHECK, UP_INSTALL, UP_NOTES, UP_RESTORE };

static UpdateInfo up_info;
static int up_checked;
static int up_rows[UP_LIST_MAX], up_nrows;
static int up_has_backup;
static char up_backup_name[48];
static char up_msg[3][160];
static Color up_msg_color;
static FATFS up_fs;
static int up_mounted;
static const char *up_busy_line;
static u32 up_shown;

static AnimList up_list = {.count = 1, .visible = UP_LIST_MAX,
                           .step = ROW_STEP};

static int up_mount(void) {
  if (!up_mounted)
    up_mounted = f_mount(&up_fs, "", 1) == FR_OK;
  if (!up_mounted)
    f_mount(NULL, "", 0);
  return up_mounted;
}

static void up_unmount(void) {
  if (up_mounted)
    f_mount(NULL, "", 0);
  up_mounted = 0;
}

static void up_say(Color c, const char *a, const char *b, const char *d) {
  up_msg_color = c;
  up_why(up_msg[0], sizeof(up_msg[0]), a, 0);
  up_why(up_msg[1], sizeof(up_msg[1]), b, 0);
  up_why(up_msg[2], sizeof(up_msg[2]), d, 0);
}

static void up_top_base(void) {
  ui_wallpaper(VRAM_TOP_LA, TOP_SCREEN_WIDTH, TOP_SCREEN_HEIGHT,
               TOP_SCREEN_HEIGHT);
  status_bar_draw();
}

/* The release's name and notes, as many lines as fit; Release Notes shows
 * the rest. */
static void up_top_notes(void) {
  static char head[96], line[160];
  volatile u8 *fb = VRAM_TOP_LA;
  const int sh = TOP_SCREEN_HEIGHT, x0 = 14, maxw = TOP_SCREEN_WIDTH - 28;
  const int lh = ui_th(&ui_small) + 3, y_end = 214;
  up_top_base();
  ui_icon(fb, x0, 28, 40, sh, ASSET_ICON_FILE_AURORA_48, 0,
          COLOR_WHITE);
  ui_text_fit(fb, x0 + 50, 28, sh, up_info.name, maxw - 50, COLOR_WHITE,
              COLOR_HM_BG_BOT, &ui_bold);
  char *p = up_cpy(head, up_info.published[0] ? "Released " : "");
  p = up_cpy(p, up_info.published);
  p = up_cpy(p, up_info.published[0] ? "  -  " : "");
  p = up_size(p, up_info.size);
  up_cpy(p, " to download");
  ui_text_fit(fb, x0 + 50, 50, sh, head, maxw - 50, COLOR_HM_TEXT2,
              COLOR_HM_BG_BOT, &ui_small);

  int y = 78, more = 0;
  for (const char *s = up_notes; *s && !more;) {
    /* One display line: up to the newline, or the last space that fits. */
    u32 n = 0, cut = 0;
    while (s[n] && s[n] != '\n' && n + 1u < sizeof(line)) {
      line[n] = s[n];
      line[n + 1] = 0;
      if (ui_tw(&ui_small, line) > maxw)
        break;
      if (s[n] == ' ')
        cut = n;
      n++;
    }
    if (s[n] && s[n] != '\n' && cut)
      n = cut;
    line[n] = 0;
    if (y + lh > y_end) {
      more = 1;
      break;
    }
    ui_text(fb, x0, y, sh, line, COLOR_WHITE, COLOR_HM_BG_BOT, &ui_small);
    y += lh;
    s += n;
    if (*s == '\n' || *s == ' ')
      s++;
  }
  ui_text_mid(fb, TOP_SCREEN_WIDTH / 2, 224, sh,
              more ? "Release Notes shows the rest.  B: Back"
                   : "A: Select  B: Back",
              COLOR_HM_TEXT2, COLOR_HM_BG_BOT, &ui_small);
  screen_present_top();
}

static void up_top(void) {
  if (up_checked && up_info.update) {
    up_top_notes();
    return;
  }
  up_top_base();
  ui_icon(VRAM_TOP_LA, (TOP_SCREEN_WIDTH - 96) / 2, 32, 96, TOP_SCREEN_HEIGHT,
          ASSET_ICON_FILE_AURORA_64, 0, COLOR_WHITE);
  ui_text_mid(VRAM_TOP_LA, TOP_SCREEN_WIDTH / 2, 130, TOP_SCREEN_HEIGHT,
              "System Update", COLOR_WHITE, COLOR_HM_BG_BOT, &ui_title);
  for (int i = 0; i < 3; i++)
    ui_text_mid_fit(VRAM_TOP_LA, TOP_SCREEN_WIDTH / 2, 164 + i * 19,
                    TOP_SCREEN_HEIGHT, up_msg[i], TOP_SCREEN_WIDTH - 24,
                    i ? COLOR_HM_TEXT2 : up_msg_color, COLOR_HM_BG_BOT,
                    &ui_font);
  ui_text_mid(VRAM_TOP_LA, TOP_SCREEN_WIDTH / 2, 224, TOP_SCREEN_HEIGHT,
              "A: Select  B: Back", COLOR_HM_TEXT2, COLOR_HM_BG_BOT, &ui_small);
  screen_present_top();
}

static void up_build_rows(void) {
  up_nrows = 0;
  if (up_checked && up_info.update) {
    up_rows[up_nrows++] = UP_INSTALL;
    up_rows[up_nrows++] = UP_NOTES;
  } else {
    up_rows[up_nrows++] = UP_CHECK;
  }
  if (up_has_backup)
    up_rows[up_nrows++] = UP_RESTORE;
  up_list.count = up_nrows;
}

static void up_paint(void) {
  static char label[96];
  volatile u8 *fb = VRAM_BOT_A;
  const int sh = BOT_SCREEN_HEIGHT;
  ui_wallpaper(fb, BOT_SCREEN_WIDTH, sh, sh);
  for (int i = 0; i < up_nrows; i++) {
    int y = ROW_Y0 + i * ROW_STEP;
    const Font *f = i ? &ui_font : &ui_bold;
    u32 icon;
    switch (up_rows[i]) {
      case UP_INSTALL:
        icon = ASSET_ICON_FILE_AURORA_32;
        up_cpy(up_cpy(label, "Install "), up_info.name);
        break;
      case UP_NOTES:
        icon = ASSET_ICON_FILE_TEXT_32;
        up_cpy(label, "Release Notes");
        break;
      case UP_RESTORE:
        icon = ASSET_ICON_WRENCH_32;
        up_cpy(up_cpy(label, "Restore "), up_backup_name);
        break;
      default:
        icon = ASSET_ICON_SEARCH_32;
        up_cpy(label, up_checked ? "Check Again" : "Check for Updates");
        break;
    }
    if (i)
      draw_gradient_round_rect(fb, ROW_X, y, ROW_W, ROW_H, 8, sh,
                               COLOR_HM_SLOT_TOP, COLOR_HM_SLOT_BOT);
    else
      draw_filled_round_rect(fb, ROW_X, y, ROW_W, ROW_H, 8, sh, g_accent);
    ui_icon(fb, ROW_X + 6, y, ROW_H, sh, icon, 0, COLOR_WHITE);
    ui_text_fit(fb, ROW_X + 44, y + (ROW_H - ui_th(f)) / 2, sh, label,
                ROW_W - 52, COLOR_WHITE, i ? COLOR_HM_SLOT : g_accent, f);
  }
  int y = ROW_Y0 + UP_LIST_MAX * ROW_STEP + 12;
  ui_text_mid(fb, BOT_SCREEN_WIDTH / 2, y, sh, "Installed", COLOR_HM_TEXT2,
              COLOR_HM_BG_TOP, &ui_small);
  ui_text_mid_fit(fb, BOT_SCREEN_WIDTH / 2, y + 16, sh, AURORA_VERSION, ROW_W,
                  COLOR_WHITE, COLOR_HM_BG_TOP, &ui_font);
  ui_text_mid(fb, BOT_SCREEN_WIDTH / 2, y + 40, sh, "update.aurora3ds.xyz",
              COLOR_HM_TEXT2, COLOR_HM_BG_TOP, &ui_small);
  draw_round_ring(fb, ROW_X - 3, ROW_Y0 + anim_px(&up_list.ring) - 3,
                  ROW_W + 6, ROW_H + 6, 11, 4, sh, g_accent);
  screen_present_bottom();
}

static void up_spin(u32 ms) {
  static const s8 dx[8] = {0, 14, 20, 14, 0, -14, -20, -14};
  static const s8 dy[8] = {-20, -14, 0, 14, 20, 14, 0, -14};
  static const Color dim = {0x44, 0x44, 0x48};
  volatile u8 *fb = VRAM_BOT_A;
  const int sh = BOT_SCREEN_HEIGHT, cx = BOT_SCREEN_WIDTH / 2;
  int lit = (int)(ms / 100u) % 8;
  ui_wallpaper_rect(fb, cx - 26, SPIN_Y - 26, 52, 52, sh);
  for (int i = 0; i < 8; i++) {
    int age = (lit - i + 8) % 8;
    Color c = age == 0 ? g_accent : age < 3 ? COLOR_HM_TEXT2 : dim;
    draw_filled_round_rect(fb, cx + dx[i] - 3, SPIN_Y + dy[i] - 3, 7, 7, 3, sh,
                           c);
  }
  screen_present_bottom();
}

/* The bottom screen while the network works: a title, a line and the
 * spinner, drawn before the command so it is the frame the core leaves. */
static void up_busy(const char *title, const char *line) {
  volatile u8 *fb = VRAM_BOT_A;
  const int sh = BOT_SCREEN_HEIGHT, cx = BOT_SCREEN_WIDTH / 2;
  up_say(COLOR_WHITE, line, "", "");
  if (!(up_checked && up_info.update))
    up_top();
  ui_wallpaper(fb, BOT_SCREEN_WIDTH, sh, sh);
  ui_text_mid_fit(fb, cx, 34, sh, title, BOT_SCREEN_WIDTH - 24, COLOR_WHITE,
                  COLOR_HM_BG_TOP, &ui_bold);
  ui_text_mid_fit(fb, cx, 64, sh, line, BOT_SCREEN_WIDTH - 24, COLOR_HM_TEXT2,
                  COLOR_HM_BG_TOP, &ui_font);
  ui_text_mid(fb, cx, 214, sh, "Please wait...", COLOR_HM_TEXT2,
              COLOR_HM_BG_BOT, &ui_small);
  up_busy_line = line;
  up_spin(0);
}

static void up_wait(u32 ms) {
  if (up_busy_line)
    up_spin(ms);
}

/* The updater's download: a bar and how much has come. */
static void up_progress(u32 done, u32 total) {
  static char text[64];
  volatile u8 *fb = VRAM_BOT_A;
  const int sh = BOT_SCREEN_HEIGHT, cx = BOT_SCREEN_WIDTH / 2;
  const int bx = 40, bw = BOT_SCREEN_WIDTH - 80, by = 120, bh = 10;
  if (done / 16384u == up_shown && done != total)
    return;
  up_shown = done / 16384u;
  up_busy_line = 0;
  ui_wallpaper(fb, BOT_SCREEN_WIDTH, sh, sh);
  ui_text_mid(fb, cx, 34, sh, "Downloading the updater", COLOR_WHITE,
              COLOR_HM_BG_TOP, &ui_bold);
  draw_filled_round_rect(fb, bx, by, bw, bh, 5, sh, COLOR_HM_SLOT_BOT);
  int fill = total ? (int)((u64)bw * done / total) : 0;
  if (fill > bw)
    fill = bw;
  if (fill >= 10)
    draw_filled_round_rect(fb, bx, by, fill, bh, 5, sh, g_accent);
  char *p = up_size(text, done);
  if (total) {
    p = up_cpy(p, " of ");
    up_size(p, total);
  }
  ui_text_mid(fb, cx, by + 22, sh, text, COLOR_HM_TEXT2, COLOR_HM_BG_TOP,
              &ui_small);
  screen_present_bottom();
}

/* On the network the core keeps up, joined first when it is not. */
static int up_online(const char *title, char *why, int size) {
  static char ssid[33];
  if (appnet_online())
    return 1;
  up_busy(title, "Joining the saved Wi-Fi network");
  up_unmount(); /* staging the Wi-Fi firmware mounts the card */
  return wifi_net_join(ssid, sizeof(ssid), why, size, up_spin);
}

static void up_refresh_backup(void) {
  up_has_backup = up_mount() && update_backup(up_backup_name,
                                              sizeof(up_backup_name));
}

static void up_check(void) {
  static char why[160], line[96];
  if (!up_online("Checking for updates", why, sizeof(why))) {
    up_say(COLOR_RED, "Could not join the network", why, "");
    return;
  }
  up_busy("Checking for updates", "Asking update.aurora3ds.xyz");
  wifi_direct_on();
  int ok = update_check(&up_info, why, sizeof(why));
  wifi_direct_off();
  up_busy_line = 0;
  up_checked = ok;
  if (!ok) {
    up_say(COLOR_RED, "Could not check for updates", why, "");
    return;
  }
  if (up_info.update) {
    char *p = up_cpy(line, up_info.name);
    up_cpy(p, " is available");
    up_say(COLOR_WHITE, line, "", "");
    return;
  }
  up_say(COLOR_WHITE, "AuroraOS is up to date",
         up_info.name[0] ? "The newest release is" : "", up_info.name);
}

static void up_install(void (*launch)(const char *path)) {
  static char why[160], q[96];
  char *p = up_cpy(q, "Install ");
  up_cpy(up_cpy(p, up_info.name), "?");
  if (!fv_confirm(q, "Aurora restarts into the updater. Keep it powered.",
                  "Install", "Not now"))
    return;
  if (!update_battery_ok()) {
    up_say(COLOR_RED, "The battery is too low",
           "Charge it past 20% or plug in the charger.", "");
    return;
  }
  if (!up_online("Installing the update", why, sizeof(why))) {
    up_say(COLOR_RED, "Could not join the network", why, "");
    return;
  }
  up_shown = 0xFFFFFFFFu;
  up_busy("Installing the update", "Downloading the updater");
  wifi_direct_on();
  int ok = up_mount() && update_fetch(&up_info, up_progress, why, sizeof(why));
  wifi_direct_off();
  up_busy_line = 0;
  if (!ok) {
    up_say(COLOR_RED, "Could not download the updater", why, "");
    return;
  }
  up_unmount();
  t_appnet_hooks(0, 0);
  launch(UPDATE_APP); /* returns only when it did not start */
  t_appnet_hooks(up_mount, up_wait);
  up_say(COLOR_RED, "The updater did not start", "", "");
}

static void up_restore(void (*boot)(const char *path)) {
  static char why[160], q[96];
  char *p = up_cpy(q, "Restore ");
  up_cpy(up_cpy(p, up_backup_name), "?");
  if (!fv_confirm(q, "The files the last update replaced come back, and "
                     "Aurora restarts.",
                  "Restore", "Not now"))
    return;
  if (!up_mount() || !update_restore(why, sizeof(why))) {
    up_say(COLOR_RED, "Could not restore", why, "");
    return;
  }
  up_unmount();
  t_appnet_hooks(0, 0);
  boot(UPDATE_OS_PATH); /* returns only when it did not start */
  t_appnet_hooks(up_mount, up_wait);
  up_refresh_backup();
  up_say(COLOR_RED, "The restored AuroraOS did not start",
         "Restore again to go back.", "");
}

static void up_open_notes(void) {
  const char *text = up_notes[0] ? up_notes : "This release has no notes.";
  if (up_mount()) {
    f_mkdir("/Aurora");
    f_mkdir(UPDATE_DIR);
  }
  if (!up_mounted || !up_write(UP_NOTES_FILE, text)) {
    up_say(COLOR_RED, "Could not write to the SD card", "", "");
    return;
  }
  text_view(UP_NOTES_FILE, "Release Notes", (u32)strlen(text));
}

void update_screen(void (*launch)(const char *path),
                   void (*boot)(const char *path)) {
  int sel = 0;
  u32 frame_at = 0;
  t_appnet_hooks(up_mount, up_wait);
  up_refresh_backup();
  if (!up_checked)
    up_say(COLOR_WHITE, "Check for a newer AuroraOS.",
           "Updates come from update.aurora3ds.xyz.", "");
  up_build_rows();
  up_top();
  anim_list_jump(&up_list, sel);
  up_paint();
  for (;;) {
    u32 k = get_keys_down();
    int prev = sel, activate = 0, tx, ty;
    if ((k & BUTTON_DUP) && sel > 0)
      sel--;
    if ((k & BUTTON_DDOWN) && sel < up_nrows - 1)
      sel++;
    if (touch_tap(&tx, &ty))
      for (int i = 0; i < up_nrows; i++)
        if (touch_in(tx, ty, ROW_X - 3, ROW_Y0 + i * ROW_STEP - 3, ROW_W + 6,
                     ROW_H + 6)) {
          sel = i;
          activate = 1;
        }
    if (sel != prev) {
      if (!activate)
        ui_sound_move();
      anim_list_to(&up_list, sel);
      if (!anim_list_moving(&up_list))
        up_paint();
    }
    if ((k & BUTTON_A) || activate) {
      int row = up_rows[sel];
      ui_sound_select();
      if (row == UP_CHECK)
        up_check();
      else if (row == UP_INSTALL)
        up_install(launch);
      else if (row == UP_NOTES)
        up_open_notes();
      else
        up_restore(boot);
      wifi_direct_off();
      if (row == UP_CHECK) /* text_view did its own */
        anim_transition(ANIM_POP, ANIM_BOTH);
      up_refresh_backup();
      up_build_rows();
      if (sel >= up_nrows)
        sel = up_nrows - 1;
      if (row == UP_CHECK)
        sel = 0;
      up_top();
      anim_list_jump(&up_list, sel);
      up_paint();
    }
    if (k & BUTTON_B) {
      ui_sound_back();
      break;
    }
    int ms = anim_frame(&frame_at);
    if (ms && anim_list_step(&up_list, ms))
      up_paint();
    ui_idle();
  }
  t_appnet_hooks(0, 0);
  up_unmount();
}
