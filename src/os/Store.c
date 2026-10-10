/* aShop: a carousel of sections on the bottom screen, lists of apps and news,
 * app pages, and downloads into SD:/Aurora/Apps (apps in C into
 * SD:/Aurora/Apps/C, labelled "C SDK"). It needs a linked Aurora
 * account: the catalogue, the icons and the apps come from the account
 * server's 3ds host with the console's token, in byte ranges that fit the
 * Wi-Fi core's reply buffer. The catalogue and icons are kept on the card,
 * so aShop opens offline too. Banners and the welcome art are PNGs in
 * SD:/Aurora/Store, drawn as plain tiles when missing. See docs/store.md. */

#include "store.h"
#include "account.h"
#include "anim.h"
#include "assets.h"
#include "ff.h"
#include "http.h"
#include "image.h"
#include "json.h"
#include "keyboard.h"
#include "lang.h"
#include "nand.h"
#include "power.h"
#include "statusbar.h"
#include "timer.h"
#include "touch.h"
#include "ui.h"
#include "wifi.h"
#include <string.h>

#define TW    TOP_SCREEN_WIDTH
#define TSH   TOP_SCREEN_HEIGHT
#define BW    BOT_SCREEN_WIDTH
#define BSH   BOT_SCREEN_HEIGHT
#define BAR_H STATUS_APP_HEIGHT

#define ST_DIR     "0:/Aurora/Store"
#define ST_CATALOG ST_DIR "/catalog.json"
#define ST_TAG     ST_DIR "/catalog.tag"
#define ST_ICONS   ST_DIR "/icons"
#define ST_RECORDS ST_DIR "/installed.txt"
#define ST_APPS    "0:/Aurora/Apps"
#define ST_APPS_C  ST_APPS "/C"
#define ST_PATH    160

#define MAX_SECS  8
#define MAX_APPS  64
#define MAX_NEWS  16
#define MAX_ITEMS (MAX_APPS + MAX_NEWS)
#define CAT_MAX   (96 * 1024)
#define POOL_MAX  (64 * 1024)
#define TOK_MAX   2560
#define FILE_MAX  64
#define VER_MAX   16
#define LINES_MAX 200 /* of a page's text, wrapped */

/* Bodies come in pieces sized from the speed the last one came at, to take
 * about PIECE_MS each: a slow link still answers within Http.c's timeout, and
 * a fast one spends its time on data rather than on connections. A reply
 * stays in the core's buffer (WIFI_HTTP_RESP_MAX, headers included). */
#define PIECE_MIN (16u * 1024u)
#define PIECE_MAX (512u * 1024u)
#define PIECE_MS  3000u
#define ICON_MAX  (16 * 1024)
#define TRIES     3 /* for one piece, before a download gives up */

static const Color c_bar = {0x10, 0x37, 0x28};
static const Color c_bg = {0x1A, 0x1A, 0x1A};
static const Color c_card = {0x07, 0x07, 0x07};
static const Color c_btn = {0x2B, 0x2B, 0x2B};
static const Color c_btn_lit = {0x45, 0x45, 0x45};
static const Color c_btn_edge = {0x36, 0x36, 0x36};
static const Color c_ring = {0x50, 0xFF, 0xB9};
static const Color c_line = {0x26, 0x26, 0x26};
static const Color c_track = {0x1F, 0x1F, 0x1F};
static const Color c_track_top = {0x2A, 0x2A, 0x2A};
static const Color c_track_bot = {0x10, 0x10, 0x10};
static const Color c_track_edge = {0x3C, 0x3C, 0x3C};
static const Color c_fill_top = {0x9C, 0xF0, 0xCE};
static const Color c_fill_bot = {0x4E, 0xD3, 0x9C};
static const Color c_floor = {0x1F, 0x2E, 0x28};
static const Color c_icon_top = {0x58, 0x58, 0x58};
static const Color c_icon_bot = {0x3A, 0x3A, 0x3A};
static const Color c_news_top = {0x3A, 0x9A, 0xE8};
static const Color c_news_bot = {0x1F, 0x6A, 0xB0};

/* The welcome screen's stripes, one entry per row of their 16-pixel period. */
#define SD_ {0x1C, 0x5B, 0x42}
#define SL_ {0x29, 0x84, 0x5F}
static const Color stripe[16] = {
    SD_, {0x20, 0x68, 0x4B}, {0x28, 0x82, 0x5E}, SL_, SL_, SL_,
    {0x28, 0x82, 0x5E}, {0x20, 0x67, 0x4A}, SD_, SD_, SD_, SD_, SD_, SD_, SD_,
    SD_};

enum { K_APPS, K_NEWS, K_UPDATES };
enum { A_NEW, A_INSTALLED, A_UPDATE };

typedef struct {
  const char *id, *title, *banner;
  int kind;
  Color color;
  u8 *art;
} Section;

typedef struct {
  const char *id, *name, *dev, *version, *file, *icon, *in, *text;
  const char *sha; /* SHA-256 of the file, 64 hex digits */
  u32 size;
  int c; /* written in C (an AURC file): goes in Apps/C */
  int state;
  u8 *icon_lg, *icon_sm;
} App;

typedef struct {
  const char *id, *title, *date, *text;
} News;

/* An item in a list: an app's index, or ~index for news. */
#define IS_NEWS(it) ((it) < 0)
#define NEWS_OF(it) (~(it))
#define ITEM_ABOUT  (~MAX_NEWS)

static char cat[CAT_MAX + 1];
static char pool[POOL_MAX];
static int pool_used;
static Section secs[MAX_SECS];
static App apps[MAX_APPS];
static News news[MAX_NEWS];
static int nsecs, napps, nnews;

/* Where the catalogue shown came from. */
enum { CAT_NONE, CAT_SERVER, CAT_CARD };
static int cat_from;
static char cat_tag[24]; /* its ETag */

static char rec_file[MAX_APPS][FILE_MAX];
static char rec_ver[MAX_APPS][VER_MAX];
static int nrec;

static FATFS fs;
static int mounted;
static int installs;
static u8 *bag;
static u8 *arena_at;

static char status_text[48];
static int bar_lit = -1;

/* The lines of the text last wrapped for a page, kept while it is shown. */
static u16 ln_off[LINES_MAX];
static u8 ln_len[LINES_MAX];
static int nlines;
static const char *wrapped;

static char lower(char c) {
  return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

static int same(const char *a, const char *b) {
  while (*a && *a == *b) {
    a++;
    b++;
  }
  return *a == *b;
}

static int same_ci(const char *a, const char *b) {
  while (*a && lower(*a) == lower(*b)) {
    a++;
    b++;
  }
  return lower(*a) == lower(*b);
}

/* Whether `needle` is in `hay`, ignoring letter case. */
static int has_ci(const char *hay, const char *needle) {
  for (; *hay; hay++) {
    int i = 0;
    while (needle[i] && lower(hay[i]) == lower(needle[i]))
      i++;
    if (!needle[i])
      return 1;
  }
  return !needle[0];
}

/* Whether the space-separated `list` holds the word `w`. */
static int has_word(const char *list, const char *w) {
  int n = (int)strlen(w);
  while (list && *list) {
    while (*list == ' ')
      list++;
    if (!memcmp(list, w, (size_t)n) && (list[n] == ' ' || !list[n]))
      return 1;
    while (*list && *list != ' ')
      list++;
  }
  return 0;
}

static char *put_str(char *p, const char *s) {
  while (*s)
    *p++ = *s++;
  *p = 0;
  return p;
}

static char *put_u32(char *p, u32 v) {
  char t[12];
  int n = 0;
  do {
    t[n++] = (char)('0' + v % 10u);
    v /= 10u;
  } while (v);
  while (n)
    *p++ = t[--n];
  *p = 0;
  return p;
}

static void fmt_size(char *out, u32 b) {
  char *p = out;
  if (b >= (10u << 20)) {
    p = put_u32(p, b >> 20);
  } else if (b >= (1u << 20)) {
    p = put_u32(p, b >> 20);
    *p++ = '.';
    p = put_u32(p, ((b & 0xFFFFFu) * 10u) >> 20);
  } else {
    p = put_u32(p, (b + 1023u) >> 10);
    put_str(p, " KB");
    return;
  }
  put_str(p, " MB");
}

/* A catalogue size, which may be missing. */
static void size_text(char *out, u32 b) {
  if (b)
    fmt_size(out, b);
  else
    put_str(out, "-");
}

static void path2(char *out, const char *dir, const char *name) {
  char *p = put_str(out, dir);
  *p++ = '/';
  if (strlen(dir) + strlen(name) + 2 < ST_PATH)
    put_str(p, name);
  else
    *p = 0;
}

static int iabs(int v) { return v < 0 ? -v : v; }

static void wait_ms(u32 ms) {
  u32 t = timer_ticks();
  while (timer_us_since(t) < ms * 1000u)
    ui_idle();
}

static int hex(char c) {
  c = lower(c);
  if (c >= '0' && c <= '9')
    return c - '0';
  return (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
}

static Color parse_color(const char *s, Color dflt) {
  int v[6];
  Color c;
  for (int i = 0; i < 6; i++)
    if ((v[i] = hex(s[i])) < 0)
      return dflt;
  c.r = (u8)(v[0] * 16 + v[1]);
  c.g = (u8)(v[2] * 16 + v[3]);
  c.b = (u8)(v[4] * 16 + v[5]);
  return c;
}

static char *trim(char *s) {
  char *e;
  while (*s == ' ' || *s == '\t')
    s++;
  e = s + strlen(s);
  while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r'))
    *--e = 0;
  return s;
}

/* A file name alone: the catalogue must not reach outside its folders. */
static int plain_name(const char *v) {
  if (!v[0] || v[0] == '.')
    return 0;
  for (; *v; v++)
    if (*v == '/' || *v == '\\' || *v == ':')
      return 0;
  return 1;
}

/* Exactly n lower-case hex digits, as the server names files and icons. */
static int hex_name(const char *s, int n) {
  int i = 0;
  while (i < n && ((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f')))
    i++;
  return i == n && !s[n];
}

static Json cj;
static JsonTok ctok[TOK_MAX];

/* A string of the catalogue, copied into the pool: text keeps its line
 * breaks and accented letters. "" when it is missing or the pool is full. */
static const char *take(int obj, const char *key, int text) {
  int t = json_get(&cj, obj, key);
  char *s = pool + pool_used;
  u32 room = (u32)(POOL_MAX - pool_used);
  if (t < 0 || room < 2u)
    return "";
  if (text)
    json_text(&cj, t, s, room);
  else
    json_str(&cj, t, s, room);
  pool_used += (int)strlen(s) + 1;
  return s;
}

/* The catalogue in cat (docs/store.md "The catalogue"); what does not fit
 * the limits, or names a file outside its folder, is left out. */
static int parse(u32 len) {
  static const Color dflt = {0x2E, 0xC4, 0x8A};
  int s, a, n;
  nsecs = napps = nnews = 0;
  pool_used = 0;
  if (!json_parse(&cj, cat, len, ctok, TOK_MAX) || ctok[0].type != JSON_OBJ)
    return 0;
  s = json_get(&cj, 0, "sections");
  a = json_get(&cj, 0, "apps");
  n = json_get(&cj, 0, "news");
  for (u32 i = 0; i < json_count(&cj, s) && nsecs < MAX_SECS; i++) {
    int o = json_at(&cj, s, i);
    Section *x = &secs[nsecs];
    const char *kind;
    x->id = take(o, "id", 0);
    x->title = take(o, "title", 1);
    x->banner = take(o, "banner", 0);
    kind = take(o, "kind", 0);
    x->kind = same_ci(kind, "news")      ? K_NEWS
              : same_ci(kind, "updates") ? K_UPDATES
                                         : K_APPS;
    x->color = parse_color(take(o, "color", 0), dflt);
    x->art = 0;
    if (!plain_name(x->banner))
      x->banner = "";
    if (x->id[0])
      nsecs++;
  }
  for (u32 i = 0; i < json_count(&cj, a) && napps < MAX_APPS; i++) {
    int o = json_at(&cj, a, i);
    App *x = &apps[napps];
    x->id = take(o, "id", 0);
    x->name = take(o, "name", 1);
    x->dev = take(o, "dev", 1);
    x->version = take(o, "version", 0);
    x->file = take(o, "file", 0);
    x->icon = take(o, "icon", 0);
    x->in = take(o, "in", 0);
    x->text = take(o, "text", 1);
    x->sha = take(o, "sha256", 0);
    x->size = (u32)json_int(&cj, json_get(&cj, o, "size"), 0);
    x->c = same_ci(take(o, "lang", 0), "c");
    x->state = A_NEW;
    x->icon_lg = x->icon_sm = 0;
    if (!hex_name(x->icon, 16))
      x->icon = "";
    if (x->id[0] && plain_name(x->file) && strlen(x->file) < FILE_MAX - 6 &&
        strlen(x->version) < VER_MAX && hex_name(x->sha, 64) && x->size)
      napps++;
  }
  for (u32 i = 0; i < json_count(&cj, n) && nnews < MAX_NEWS; i++) {
    int o = json_at(&cj, n, i);
    News *x = &news[nnews++];
    x->id = take(o, "id", 0);
    x->title = take(o, "title", 1);
    x->date = take(o, "date", 1);
    x->text = take(o, "text", 1);
  }
  return 1;
}

/* The copy kept on the card, with its ETag. */
static u32 card_catalog(void) {
  static FIL f;
  UINT br = 0;
  cat_tag[0] = 0;
  if (!mounted || f_open(&f, ST_CATALOG, FA_READ) != FR_OK)
    return 0;
  if (f_size(&f) > CAT_MAX || f_read(&f, cat, CAT_MAX, &br) != FR_OK)
    br = 0;
  f_close(&f);
  if (br && f_open(&f, ST_TAG, FA_READ) == FR_OK) {
    UINT n = 0;
    if (f_read(&f, cat_tag, sizeof(cat_tag) - 1, &n) != FR_OK)
      n = 0;
    cat_tag[n] = 0;
    trim(cat_tag);
    f_close(&f);
  }
  return br;
}

static void write_file(const char *path, const void *data, u32 n) {
  static FIL f;
  UINT bw;
  if (f_open(&f, path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
    return;
  if (f_write(&f, data, n, &bw) != FR_OK || bw != n) {
    f_close(&f);
    f_unlink(path);
    return;
  }
  f_close(&f);
}

static void card_save(u32 len) {
  if (!mounted)
    return;
  f_mkdir(ST_DIR);
  f_unlink(ST_TAG); /* no tag beside a half-written copy */
  write_file(ST_CATALOG, cat, len);
  write_file(ST_TAG, cat_tag, (u32)strlen(cat_tag));
}

/* SD:/Aurora/Store/installed.txt: "file version" for each app aShop put on
 * the card, so it can tell an update from what is already there. */
static void records_load(void) {
  static FIL f;
  static char buf[MAX_APPS * (FILE_MAX + VER_MAX + 2)];
  UINT br = 0;
  char *s = buf;

  nrec = 0;
  if (!mounted || f_open(&f, ST_RECORDS, FA_READ) != FR_OK)
    return;
  if (f_read(&f, buf, sizeof(buf) - 1, &br) != FR_OK)
    br = 0;
  f_close(&f);
  buf[br] = 0;

  while (*s && nrec < MAX_APPS) {
    char *line = s, *sp;
    while (*s && *s != '\n')
      s++;
    if (*s)
      *s++ = 0;
    line = trim(line);
    sp = strchr(line, ' ');
    if (!sp)
      continue;
    *sp = 0;
    sp = trim(sp + 1);
    if (strlen(line) >= FILE_MAX || strlen(sp) >= VER_MAX || !*line)
      continue;
    put_str(rec_file[nrec], line);
    put_str(rec_ver[nrec], sp);
    nrec++;
  }
}

static void records_save(void) {
  static FIL f;
  static char buf[MAX_APPS * (FILE_MAX + VER_MAX + 2)];
  char *p = buf;
  UINT bw;

  if (!mounted)
    return;
  f_mkdir(ST_DIR);
  for (int i = 0; i < nrec; i++) {
    p = put_str(p, rec_file[i]);
    *p++ = ' ';
    p = put_str(p, rec_ver[i]);
    *p++ = '\n';
  }
  if (f_open(&f, ST_RECORDS, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
    return;
  f_write(&f, buf, (UINT)(p - buf), &bw);
  f_close(&f);
}

static const char *record_of(const char *file) {
  for (int i = 0; i < nrec; i++)
    if (same_ci(rec_file[i], file))
      return rec_ver[i];
  return 0;
}

static void record_set(const char *file, const char *ver) {
  int i = 0;
  while (i < nrec && !same_ci(rec_file[i], file))
    i++;
  if (i == nrec) {
    if (nrec >= MAX_APPS || strlen(file) >= FILE_MAX)
      return;
    nrec++;
  }
  put_str(rec_file[i], file);
  rec_ver[i][0] = 0;
  if (strlen(ver) < VER_MAX)
    put_str(rec_ver[i], ver);
  records_save();
}

static const char *app_dir(const App *a) { return a->c ? ST_APPS_C : ST_APPS; }

/* 1 when the app is in its folder, 2 when it is only in the other one (its
 * new version is in the other language), else 0. */
static int app_on_card(const App *a) {
  static FILINFO fno;
  char path[ST_PATH];
  if (!mounted)
    return 0;
  path2(path, app_dir(a), a->file);
  if (f_stat(path, &fno) == FR_OK)
    return 1;
  path2(path, a->c ? ST_APPS : ST_APPS_C, a->file);
  return f_stat(path, &fno) == FR_OK ? 2 : 0;
}

/* An app on the card that aShop did not record, or recorded at another
 * version, or that is in the other folder, has an update. */
static void states_update(void) {
  for (int i = 0; i < napps; i++) {
    App *a = &apps[i];
    const char *rec = record_of(a->file);
    int on = app_on_card(a);
    if (!on)
      a->state = A_NEW;
    else if (on == 1 && (!a->version[0] || (rec && same_ci(rec, a->version))))
      a->state = A_INSTALLED;
    else
      a->state = A_UPDATE;
  }
}

static int updates_count(void) {
  int n = 0;
  for (int i = 0; i < napps; i++)
    n += apps[i].state == A_UPDATE;
  return n;
}

static void status_update(void) {
  int n = updates_count();
  char *p = put_str(status_text, "aShop");
  if (cat_from == CAT_CARD) {
    put_str(p, L(STR_ST_OFFLINE_TAG));
  } else if (n) {
    p = put_str(p, " (");
    p = put_u32(p, (u32)n);
    put_str(p, L(n == 1 ? STR_ST_UPDATE1 : STR_ST_UPDATES));
  }
}

static u8 *arena_take(u32 n) {
  u8 *p = arena_at;
  n = (n + 3u) & ~3u;
  if ((u32)(p + n) > STORE_ARENA_ADDR + STORE_ARENA_SIZE)
    return 0;
  arena_at += n;
  return p;
}

/* The share of the pixel at (x, y) of a corner box that a quarter circle of
 * radius r covers, in sixteenths. */
static int corner_cov(int x, int y, int r) {
  int n = 0, c = 8 * r, rr = c * c;
  for (int j = 0; j < 4; j++) {
    int dy = c - (8 * y + 2 * j + 1);
    for (int i = 0; i < 4; i++) {
      int dx = c - (8 * x + 2 * i + 1);
      n += dx * dx + dy * dy <= rr;
    }
  }
  return n;
}

static void round_art(u8 *rgba, int w, int h, int r) {
  for (int y = 0; y < r; y++)
    for (int x = 0; x < r; x++) {
      int cov = corner_cov(x, y, r);
      int px[4][2] = {{x, y}, {w - 1 - x, y}, {x, h - 1 - y},
                      {w - 1 - x, h - 1 - y}};
      if (cov == 16)
        continue;
      for (int k = 0; k < 4; k++) {
        u8 *p = rgba + (px[k][1] * w + px[k][0]) * 4 + 3;
        *p = (u8)(*p * cov / 16);
      }
    }
}

/* A PNG or JPEG from SD:/Aurora/Store at w x h, or 0. The file and the
 * decoder's rows use the image viewer's buffer, idle while aShop is open. */
#define ART_FILE_MAX (2u * 1024u * 1024u)
static u8 *art_load(const char *name, int w, int h, int r) {
  static FIL f;
  u8 *file = (u8 *)IMAGE_FILE_ADDR, *out;
  char path[ST_PATH];
  UINT br;
  u32 size;

  if (!mounted || !name || !name[0])
    return 0;
  path2(path, ST_DIR, name);
  if (f_open(&f, path, FA_READ) != FR_OK)
    return 0;
  size = f_size(&f);
  if (size == 0 || size > ART_FILE_MAX ||
      f_read(&f, file, size, &br) != FR_OK || br != size) {
    f_close(&f);
    return 0;
  }
  f_close(&f);
  out = arena_take((u32)(w * h * 4));
  if (!out)
    return 0;
  if (image_decode_fit(file, size, out, w, h, file + ART_FILE_MAX,
                       IMAGE_FILE_MAX - ART_FILE_MAX) != IMG_OK) {
    arena_at = out;
    return 0;
  }
  if (r)
    round_art(out, w, h, r);
  return out;
}

#define TILE_W    150
#define TILE_H    108
#define TILE_R    6
#define BAG_W     92
#define BAG_H     94

static void icon_name(char *out, const App *a) {
  put_str(put_str(put_str(out, "icons/"), a->icon), ".png");
}

/* The catalogue's art, decoded at the sizes it is drawn. */
static void art_all(void) {
  char name[32];
  arena_at = (u8 *)STORE_ARENA_ADDR;
  wrapped = 0;
  bag = art_load("bag.png", BAG_W, BAG_H, 0);
  for (int i = 0; i < nsecs; i++)
    secs[i].art = art_load(secs[i].banner, TILE_W, TILE_H, TILE_R);
  for (int i = 0; i < napps; i++) {
    apps[i].icon_lg = apps[i].icon_sm = 0;
    if (!apps[i].icon[0])
      continue;
    icon_name(name, &apps[i]);
    apps[i].icon_lg = art_load(name, 48, 48, 10);
    apps[i].icon_sm = apps[i].icon_lg ? art_load(name, 24, 24, 5) : 0;
  }
}

static News about;

static const News *news_at(int it) {
  return it == ITEM_ABOUT ? &about : &news[NEWS_OF(it)];
}

static const char *item_title(int it) {
  return IS_NEWS(it) ? news_at(it)->title : apps[it].name;
}

static const char *item_side(int it) {
  return IS_NEWS(it) ? news_at(it)->date : apps[it].dev;
}

static const char *item_text(int it) {
  return IS_NEWS(it) ? news_at(it)->text : apps[it].text;
}

static void about_build(void) {
  static char text[768];
  char *p = text;
  p = put_str(p, L(STR_ST_AB_INTRO));
  p = put_str(p, "\n\n");
  p = put_str(p, L(STR_ST_AB_CAT));
  p = put_str(p, L(cat_from == CAT_SERVER ? STR_ST_AB_ONLINE
                                          : STR_ST_AB_OFFLINE));
  p = put_str(p, L(STR_ST_AB_WITH));
  p = put_u32(p, (u32)napps);
  p = put_str(p, L(napps == 1 ? STR_ST_AB_APP : STR_ST_AB_APPS));
  p = put_u32(p, (u32)nnews);
  p = put_str(p, L(nnews == 1 ? STR_ST_AB_NEWS1 : STR_ST_AB_NEWSN));
  p = put_str(p, "\n\n");
  put_str(p, L(STR_ST_AB_HOW));
  about = (News){"about", L(STR_ST_ABOUT), AURORA_VERSION, text};
  wrapped = 0;
}

/* Drawing straight into a framebuffer: (x, y) is at column x, counted from
 * the bottom row. */
static inline volatile u8 *px_at(volatile u8 *fb, int x, int y, int sh) {
  return fb + ((u32)x * (u32)sh + (u32)(sh - 1 - y)) * 3u;
}

static inline void blend(volatile u8 *p, Color c, int a) {
  int inv = 256 - a;
  p[0] = (u8)((c.b * a + p[0] * inv) >> 8);
  p[1] = (u8)((c.g * a + p[1] * inv) >> 8);
  p[2] = (u8)((c.r * a + p[2] * inv) >> 8);
}

/* `c` at `alpha` (0..256) over a rounded rectangle, keeping what is under
 * it. */
static void shade_round(volatile u8 *fb, int sh, int x0, int y0, int w, int h,
                        int r, Color c, int alpha) {
  int wmax = screen_fb_width(fb);
  screen_touch(fb);
  for (int x = 0; x < w; x++) {
    int px = x0 + x, cx = x < r ? x : (x >= w - r ? w - 1 - x : -1);
    if (px < 0 || px >= wmax)
      continue;
    for (int y = 0; y < h; y++) {
      int py = y0 + y, cy = y < r ? y : (y >= h - r ? h - 1 - y : -1);
      int a = alpha;
      if (py < 0 || py >= sh)
        continue;
      if (cx >= 0 && cy >= 0)
        a = a * corner_cov(cx, cy, r) / 16;
      if (a)
        blend(px_at(fb, px, py, sh), c, a);
    }
  }
}

static void stripes(volatile u8 *fb) {
  screen_touch(fb);
  for (int x = 0; x < TW; x++) {
    volatile u8 *p = fb + (u32)x * TSH * 3u;
    for (int y = TSH - 1; y >= BAR_H; y--, p += 3) {
      const Color *c = &stripe[(y - x) & 15];
      p[0] = c->b;
      p[1] = c->g;
      p[2] = c->r;
    }
  }
}

/* The download screen's floor: a checkerboard plane seen from above, fading
 * in from FLOOR_Y. A tile is 0.0955 x (y + 159) pixels wide at row y and its
 * depth 6336 / (y + 159) tiles, which matches the mock-up; each pixel takes
 * four samples. `phase` (1/65536 of a tile) moves it toward the viewer. */
#define FLOOR_Y    124
#define FLOOR_FADE 64
static void checker_floor(volatile u8 *fb, u32 phase) {
  screen_touch(fb);
  for (int y = FLOOR_Y; y < TSH; y++) {
    int a = (y - FLOOR_Y) * 256 / FLOOR_FADE;
    s32 k[2];
    u32 z[2];
    if (a > 256)
      a = 256;
    for (int s = 0; s < 2; s++) {
      u32 d = (u32)(2 * y + s + 318);
      k[s] = (s32)(1372000u / d);
      z[s] = ((830472192u / d) + phase) >> 16;
    }
    for (int x = 0; x < TW; x++) {
      int n = 0;
      for (int t = 0; t < 2; t++) {
        s32 u = ((2 * x + t - 400) * k[0]) >> 17;
        s32 v = ((2 * x + t - 400) * k[1]) >> 17;
        n += ((u ^ (s32)z[0]) & 1) + ((v ^ (s32)z[1]) & 1);
      }
      volatile u8 *p = px_at(fb, x, y, TSH);
      if (n)
        blend(p, c_floor, a * n / 4);
    }
  }
}

/* The status bar is drawn once and kept, so content can scroll under it
 * without reading the clock and battery over I2C every frame. */
static u8 bar_strip[TW * BAR_H * 3];

static void bar_put(void) {
  volatile u8 *fb = VRAM_TOP_LA;
  screen_touch(fb);
  for (int x = 0; x < TW; x++)
    memcpy((void *)(fb + ((u32)x * TSH + (TSH - BAR_H)) * 3u),
           bar_strip + x * BAR_H * 3, BAR_H * 3);
}

static void bar_draw(int downloading) {
  volatile u8 *fb = VRAM_TOP_LA;
  status_bar_draw_app(downloading ? L(STR_ST_DL_BAR) : status_text,
                      downloading ? c_bg : c_bar,
                      downloading ? UI_NO_ASSET : ASSET_APP_STORE_16);
  for (int x = 0; x < TW; x++)
    memcpy(bar_strip + x * BAR_H * 3,
           (const void *)(fb + ((u32)x * TSH + (TSH - BAR_H)) * 3u),
           BAR_H * 3);
}

static int clock_tick, clock_min = -1;

/* The clock is on a slow I2C bus: sampled every 96 passes, and the bar
 * redrawn when the minute turns. */
static void clock_poll(int downloading) {
  RtcTime now;
  if (++clock_tick < 96)
    return;
  clock_tick = 0;
  if (rtc_read(&now) && now.min != clock_min) {
    if (clock_min >= 0) {
      bar_draw(downloading);
      screen_present_top();
    }
    clock_min = now.min;
  }
}

static void asset_rgba(volatile u8 *fb, int x, int y, int sh, int w, int h,
                       const u8 *data) {
  Asset a = {ASSET_TYPE_RGBA, (uint16_t)w, (uint16_t)h, data};
  draw_asset(fb, x, y, sh, &a, COLOR_WHITE);
}

static void app_icon(volatile u8 *fb, int x, int y, int size, int sh,
                     const App *a) {
  const u8 *art = size >= 48 ? a->icon_lg : a->icon_sm;
  if (art) {
    asset_rgba(fb, x, y, sh, size, size, art);
    return;
  }
  draw_gradient_round_rect(fb, x, y, size, size, size / 5, sh, c_icon_top,
                           c_icon_bot);
  ui_icon(fb, x, y, size, sh,
          size >= 48 ? ASSET_ICON_CONSOLE_32 : ASSET_ICON_CONSOLE_16, 0,
          COLOR_WHITE);
}

static void item_icon(volatile u8 *fb, int x, int y, int size, int sh,
                      int it) {
  if (!IS_NEWS(it)) {
    app_icon(fb, x, y, size, sh, &apps[it]);
    return;
  }
  draw_gradient_round_rect(fb, x, y, size, size, size / 5, sh, c_news_top,
                           c_news_bot);
  ui_icon(fb, x, y, size, sh,
          size >= 48 ? ASSET_ICON_INFO_32 : ASSET_ICON_INFO_16, 0,
          COLOR_WHITE);
}

/* The bar of buttons along the bottom: one, two (Back and an action) or
 * three, the outer corners rounded. */
#define BB_Y 214
#define BB_H (BSH - BB_Y)

typedef struct {
  StringId label;
  u32 icon;
  int back; /* a chevron before the label */
} Btn;

/* A side button is 85 pixels, or wider when its label needs it; the middle
 * one takes what is left. */
static int btn_side(const Btn *b) {
  int need = ui_tw(&ui_font, L(b->label)) + 14 +
             (b->icon != UI_NO_ASSET ? 22 : b->back ? 14 : 0);
  return need > 85 ? need : 85;
}

static void btn_rect(const Btn *b, int n, int i, int *x, int *w) {
  int l = n > 1 ? btn_side(&b[0]) : BW, r = n > 2 ? btn_side(&b[2]) : 0;
  if (n == 1) {
    *x = 0;
    *w = BW;
  } else if (n == 2) {
    *x = i ? l + 1 : 0;
    *w = i ? BW - l - 1 : l;
  } else {
    *x = i == 0 ? 0 : i == 1 ? l + 1 : BW - r;
    *w = i == 0 ? l : i == 1 ? BW - l - r - 2 : r;
  }
}

static int btn_at(const Btn *b, int n, int tx, int ty) {
  if (ty < BB_Y - 4)
    return -1;
  for (int i = 0; i < n; i++) {
    int x, w;
    btn_rect(b, n, i, &x, &w);
    if (tx >= x && tx < x + w + 1)
      return i;
  }
  return -1;
}

static void chevron(volatile u8 *fb, int x, int cy, Color c) {
  for (int i = 0; i < 6; i++) {
    draw_filled_rect(fb, x + i, cy - i - 1, 2, 2, BSH, c);
    draw_filled_rect(fb, x + i, cy + i - 1, 2, 2, BSH, c);
  }
}

static void buttons(const Btn *b, int n) {
  volatile u8 *fb = VRAM_BOT_A;
  draw_filled_rect(fb, 0, BB_Y - 2, BW, BB_H + 2, BSH, c_bg);
  draw_filled_round_rect(fb, 0, BB_Y, BW, BB_H + 8, 6, BSH, c_btn);
  for (int i = 0; i < n; i++) {
    int x, w, tw, iw, lx, ty = BB_Y + (BB_H - ui_th(&ui_font)) / 2;
    int left = i == 0 ? 6 : 0, right = i == n - 1 ? 6 : 0;
    btn_rect(b, n, i, &x, &w);
    if (i == bar_lit) {
      draw_filled_round_rect(fb, x, BB_Y, w, BB_H + 8, 6, BSH, c_btn_lit);
      if (!left)
        draw_filled_rect(fb, x, BB_Y, 6, BB_H, BSH, c_btn_lit);
      if (!right)
        draw_filled_rect(fb, x + w - 6, BB_Y, 6, BB_H, BSH, c_btn_lit);
    } else {
      draw_filled_rect(fb, x + left, BB_Y, w - left - right, 1, BSH,
                       c_btn_edge);
    }
    if (i)
      draw_filled_rect(fb, x - 1, BB_Y, 1, BB_H, BSH, c_bg);
    tw = ui_tw(&ui_font, L(b[i].label));
    iw = b[i].icon != UI_NO_ASSET ? 22 : b[i].back ? 14 : 0;
    if (tw > w - 8 - iw)
      tw = w - 8 - iw;
    lx = x + (w - tw - iw) / 2;
    if (b[i].icon != UI_NO_ASSET)
      ui_icon(fb, lx, BB_Y + (BB_H - 16) / 2, 16, BSH, b[i].icon, 0,
              COLOR_WHITE);
    else if (b[i].back)
      chevron(fb, lx, BB_Y + BB_H / 2, COLOR_WHITE);
    ui_text_fit(fb, lx + iw, ty, BSH, L(b[i].label), tw, COLOR_WHITE, c_btn,
                &ui_font);
  }
}

/* Lights a button for a moment before what it does, so a tap shows. */
static void flash(void (*paint)(void), int i) {
  bar_lit = i;
  paint();
  screen_present_bottom();
  wait_ms(90);
  bar_lit = -1;
}

/* After a key ended a screen: let it go, so the screen under does not act
 * on it too. */
static void settle(void) {
  while (get_keys() & (BUTTON_A | BUTTON_B | BUTTON_START | BUTTON_SELECT))
    ui_idle();
  get_keys_down();
}

static int touch_wait;

/* A finger still down from the last screen must lift before a tap counts. */
static void touch_arm(void) {
  int x, y;
  touch_wait = touch_read(&x, &y, 0, 0);
}

static int tapped(int *x, int *y) {
  int down = touch_read(x, y, 0, 0);
  int t = touch_tap(x, y);
  if (touch_wait) {
    touch_wait = down;
    return 0;
  }
  return t;
}

static void message(const char *title, const char *sub) {
  int x, y;
  ui_dialog(VRAM_BOT_A, BW, BSH, BSH, title, sub, c_ring, 1);
  screen_present_bottom();
  touch_arm();
  while (1) {
    u32 k = get_keys_down();
    if ((k & (BUTTON_A | BUTTON_B)) || tapped(&x, &y))
      break;
    ui_idle();
  }
  settle();
  anim_transition(ANIM_FADE, ANIM_BOT);
}

#define BAG_X ((TW - BAG_W) / 2)
#define BAG_Y 58
#define WC_X  50
#define WC_Y  176
#define WC_W  300
#define WC_H  40

static void top_home(const char *card, const char *hint) {
  static const Color under = {0x08, 0x1B, 0x13};
  volatile u8 *fb = VRAM_TOP_LA;
  stripes(fb);
  if (bag)
    asset_rgba(fb, BAG_X, BAG_Y, TSH, BAG_W, BAG_H, bag);
  else
    ui_icon(fb, (TW - 96) / 2, BAG_Y, 96, TSH, ASSET_APP_STORE_64, 0,
            COLOR_WHITE);
  draw_filled_round_rect(fb, WC_X, WC_Y, WC_W, WC_H, 9, TSH, under);
  ui_text_mid_fit(fb, TW / 2, WC_Y + (WC_H - ui_th(&ui_bold)) / 2, TSH, card,
                  WC_W - 24, COLOR_WHITE, under, &ui_bold);
  if (hint) {
    draw_filled_round_rect(fb, 50, WC_Y + WC_H + 4, TW - 100, 18, 6, TSH,
                           under);
    ui_text_mid(fb, TW / 2, WC_Y + WC_H + 7, TSH, hint, COLOR_HM_TEXT2, under,
                &ui_small);
  }
  bar_put();
}

#define PG_X      50
#define PG_W      300
#define PG_CARD_Y 55
#define PG_CARD_H 40
#define PG_TEXT_Y 112

static int line_h(void) { return ui_th(&ui_font) + 2; }

static int width_n(const char *s, int n) {
  char buf[256];
  if (n > 255)
    n = 255;
  memcpy(buf, s, (size_t)n);
  buf[n] = 0;
  return ui_tw(&ui_font, buf);
}

static void add_line(int off, int len) {
  if (nlines < LINES_MAX) {
    ln_off[nlines] = (u16)off;
    ln_len[nlines] = (u8)len;
    nlines++;
  }
}

/* Breaks `t` into lines at spaces to fit PG_W; "\n" ends a line. */
static void wrap(const char *t) {
  int pos = 0;
  if (wrapped == t)
    return;
  wrapped = t;
  nlines = 0;
  while (t[pos] && nlines < LINES_MAX && pos < 60000) {
    int end = pos, ls = pos;
    while (t[end] && t[end] != '\n')
      end++;
    if (end == pos)
      add_line(pos, 0);
    while (ls < end) {
      int fit, i;
      while (ls < end && t[ls] == ' ')
        ls++;
      if (ls >= end)
        break;
      fit = i = ls;
      while (i < end) {
        int we = i;
        while (we < end && t[we] != ' ')
          we++;
        if (fit > ls && (width_n(t + ls, we - ls) > PG_W || we - ls > 250))
          break;
        fit = we;
        i = we;
        while (i < end && t[i] == ' ')
          i++;
      }
      if (fit - ls > 250)
        fit = ls + 250;
      add_line(ls, fit - ls);
      ls = fit;
    }
    pos = t[end] ? end + 1 : end;
  }
}

static int page_max(int it) {
  int h;
  wrap(item_text(it));
  h = PG_TEXT_Y + nlines * line_h() + 10 - TSH;
  return h > 0 ? h : 0;
}

static void page_top(int it, int scroll) {
  volatile u8 *fb = VRAM_TOP_LA;
  const char *side = item_side(it);
  int y0 = PG_CARD_Y - scroll, lh = line_h(), sw = 0, max;
  char buf[256];

  draw_filled_rect(fb, 0, BAR_H, TW, TSH - BAR_H, TSH, c_bg);
  max = page_max(it);
  draw_filled_round_rect(fb, PG_X, y0, PG_W, PG_CARD_H, 9, TSH, COLOR_BLACK);
  if (side && side[0]) {
    sw = ui_tw(&ui_small, side);
    if (sw > 110)
      sw = 110;
    ui_text_fit(fb, PG_X + PG_W - 15 - sw,
                y0 + (PG_CARD_H - ui_th(&ui_small)) / 2, TSH, side, sw,
                COLOR_HM_TEXT2, COLOR_BLACK, &ui_small);
    sw += 12;
  }
  if (!IS_NEWS(it) && apps[it].c) {
    int th = ui_th(&ui_bold);
    int ty = y0 + (PG_CARD_H - th - ui_th(&ui_small)) / 2;
    ui_text_fit(fb, PG_X + 15, ty, TSH, item_title(it), PG_W - 30 - sw,
                COLOR_WHITE, COLOR_BLACK, &ui_bold);
    ui_text(fb, PG_X + 15, ty + th, TSH, "C SDK", c_ring, COLOR_BLACK,
            &ui_small);
  } else {
    ui_text_fit(fb, PG_X + 15, y0 + (PG_CARD_H - ui_th(&ui_bold)) / 2, TSH,
                item_title(it), PG_W - 30 - sw, COLOR_WHITE, COLOR_BLACK,
                &ui_bold);
  }

  for (int i = 0; i < nlines; i++) {
    int y = PG_TEXT_Y - scroll + i * lh;
    if (y + lh <= BAR_H || !ln_len[i])
      continue;
    if (y >= TSH)
      break;
    memcpy(buf, wrapped + ln_off[i], ln_len[i]);
    buf[ln_len[i]] = 0;
    ui_text(fb, PG_X, y, TSH, buf, COLOR_WHITE, c_bg, &ui_font);
  }

  if (max > 0) {
    int track = TSH - BAR_H - 12;
    int len = track * (TSH - BAR_H) / (TSH - BAR_H + max);
    int at = BAR_H + 6 + (track - len) * scroll / max;
    draw_filled_round_rect(fb, TW - 10, BAR_H + 6, 3, track, 1, TSH, c_line);
    draw_filled_round_rect(fb, TW - 10, at, 3, len, 1, TSH, COLOR_HM_TEXT2);
  }
  bar_put();
}

#define TILE_X    ((BW - TILE_W) / 2)
#define TILE_Y    75
#define TILE_STEP 173
#define RING_PAD  8
#define RING_T    4
#define TC_X      40
#define TC_Y      14
#define TC_W      240
#define TC_H      40
#define CAR_Y0    (TILE_Y - RING_PAD)
#define CAR_Y1    (TILE_Y + TILE_H + RING_PAD)
#define SLOP      8
#define SWIPE     36

static const Btn main_btns[3] = {{STR_ST_SEARCH, ASSET_ICON_SEARCH_16, 0},
                                 {STR_ST_GO, UI_NO_ASSET, 0},
                                 {STR_ST_OPTIONS, ASSET_ICON_WRENCH_16, 0}};

static int m_sel;
static AnimVal m_scroll;
static u32 m_title_t0;

static Color mix(Color a, Color b, int t) {
  Color c;
  c.r = (u8)((a.r * (256 - t) + b.r * t) >> 8);
  c.g = (u8)((a.g * (256 - t) + b.g * t) >> 8);
  c.b = (u8)((a.b * (256 - t) + b.b * t) >> 8);
  return c;
}

static void tile(volatile u8 *fb, const Section *s, int x, int y) {
  Color top;
  if (s->art) {
    asset_rgba(fb, x, y, BSH, TILE_W, TILE_H, s->art);
    return;
  }
  top = mix(s->color, COLOR_WHITE, 70);
  draw_gradient_round_rect(fb, x, y, TILE_W, TILE_H, TILE_R, BSH, top,
                           s->color);
  if (ui_tw(&ui_title, s->title) > TILE_W - 16) {
    /* two lines, broken at the space nearest the middle */
    char first[64];
    int n = (int)strlen(s->title), cut = -1, th = ui_th(&ui_title);
    for (int i = 0; i < n && i < (int)sizeof(first) - 1; i++)
      if (s->title[i] == ' ' &&
          (cut < 0 || iabs(2 * i - n) < iabs(2 * cut - n)))
        cut = i;
    if (cut > 0) {
      memcpy(first, s->title, (size_t)cut);
      first[cut] = 0;
      ui_text_mid_fit(fb, x + TILE_W / 2, y + TILE_H / 2 - th, BSH, first,
                      TILE_W - 16, COLOR_WHITE, s->color, &ui_title);
      ui_text_mid_fit(fb, x + TILE_W / 2, y + TILE_H / 2, BSH,
                      s->title + cut + 1, TILE_W - 16, COLOR_WHITE, s->color,
                      &ui_title);
      return;
    }
  }
  ui_text_mid_fit(fb, x + TILE_W / 2, y + (TILE_H - ui_th(&ui_title)) / 2, BSH,
                  s->title, TILE_W - 16, COLOR_WHITE, s->color, &ui_title);
}

static int title_alpha(void) {
  int ms = anim_ms(m_title_t0);
  return ms >= 160 ? 256 : ms * 256 / 160;
}

static void paint_main(void) {
  volatile u8 *fb = VRAM_BOT_A;
  int sc = anim_px(&m_scroll);

  draw_filled_rect(fb, 0, 0, BW, BSH, BSH, c_bg);
  for (int i = 0; i < nsecs; i++) {
    int x = TILE_X + i * TILE_STEP - sc;
    if (x + TILE_W > 0 && x < BW)
      tile(fb, &secs[i], x, TILE_Y);
  }
  if (nsecs)
    draw_round_ring(fb, TILE_X - RING_PAD, TILE_Y - RING_PAD,
                    TILE_W + 2 * RING_PAD, TILE_H + 2 * RING_PAD,
                    TILE_R + RING_PAD, RING_T, BSH, c_ring);
  else
    ui_text_mid(fb, BW / 2, TILE_Y + 40, BSH, L(STR_ST_EMPTY),
                COLOR_HM_TEXT2, c_bg, &ui_font);

  draw_filled_round_rect(fb, TC_X, TC_Y, TC_W, TC_H, 9, BSH, c_card);
  if (nsecs)
    ui_text_mid_fit(fb, BW / 2, TC_Y + (TC_H - ui_th(&ui_bold)) / 2, BSH,
                    secs[m_sel].title, TC_W - 24,
                    mix(c_card, COLOR_WHITE, title_alpha()), c_card,
                    &ui_bold);
  buttons(main_btns, 3);
}

static void draw_main(void) {
  bar_draw(0);
  top_home(L(STR_ST_WELCOME), 0);
  screen_present_top();
  paint_main();
  screen_present_bottom();
}

#define ROW_X    10
#define ROW_W    (BW - 20)
#define ROW_H    30
#define ROW_STEP 34
#define ROW_Y0   42
#define ROW_RING 2
#define VISIBLE  5
#define LIST_TOP 38
#define LIST_END (BB_Y - 2)

typedef struct {
  const char *title;
  int items[MAX_ITEMS];
  int n, sel;
  AnimList al;
} List;

static List *cur_list;

static const Btn list_btns[3] = {{STR_BACK, UI_NO_ASSET, 1},
                                 {STR_ST_GO, UI_NO_ASSET, 0},
                                 {STR_ST_SEARCH, ASSET_ICON_SEARCH_16, 0}};

static void row_tag(int it, char *out, Color *c) {
  *c = COLOR_HM_TEXT2;
  if (IS_NEWS(it)) {
    put_str(out, news_at(it)->date);
  } else if (apps[it].state == A_INSTALLED) {
    put_str(out, L(STR_ST_INSTALLED));
  } else if (apps[it].state == A_UPDATE) {
    put_str(out, L(STR_ST_UPDATE_TAG));
    *c = c_ring;
  } else {
    size_text(out, apps[it].size);
  }
}

static void list_row(volatile u8 *fb, int y, int it) {
  char tag[24];
  Color tc;
  int tw;
  draw_filled_round_rect(fb, ROW_X, y, ROW_W, ROW_H, 8, BSH, c_btn);
  item_icon(fb, ROW_X + 4, y + 3, 24, BSH, it);
  row_tag(it, tag, &tc);
  tw = ui_tw(&ui_small, tag);
  ui_text_fit(fb, ROW_X + 36, y + (ROW_H - ui_th(&ui_font)) / 2, BSH,
              item_title(it), ROW_W - 36 - 20 - tw, COLOR_WHITE, c_btn,
              &ui_font);
  ui_text(fb, ROW_X + ROW_W - 10 - tw, y + (ROW_H - ui_th(&ui_small)) / 2,
          BSH, tag, tc, c_btn, &ui_small);
}

static void paint_list(void) {
  volatile u8 *fb = VRAM_BOT_A;
  List *l = cur_list;
  int scroll = anim_px(&l->al.scroll);
  char count[24];
  char *p;

  draw_filled_rect(fb, 0, 0, BW, BSH, BSH, c_bg);
  for (int i = 0; i < l->n; i++) {
    int y = ROW_Y0 + i * ROW_STEP - scroll;
    if (y + ROW_H > LIST_TOP && y < LIST_END)
      list_row(fb, y, l->items[i]);
  }
  draw_round_ring(fb, ROW_X - ROW_RING,
                  ROW_Y0 + anim_px(&l->al.ring) - scroll - ROW_RING,
                  ROW_W + 2 * ROW_RING, ROW_H + 2 * ROW_RING, 10, ROW_RING + 1,
                  BSH, c_ring);
  draw_filled_rect(fb, 0, 0, BW, LIST_TOP, BSH, c_bg);

  p = put_u32(count, (u32)(l->sel + 1));
  p = put_str(p, " / ");
  put_u32(p, (u32)l->n);
  draw_filled_round_rect(fb, 8, 6, BW - 16, 28, 9, BSH, c_card);
  ui_text(fb, BW - 18 - ui_tw(&ui_small, count),
          6 + (28 - ui_th(&ui_small)) / 2, BSH, count, COLOR_HM_TEXT2, c_card,
          &ui_small);
  ui_text_fit(fb, 18, 6 + (28 - ui_th(&ui_bold)) / 2, BSH, l->title,
              BW - 60 - ui_tw(&ui_small, count), COLOR_WHITE, c_card,
              &ui_bold);
  buttons(list_btns, 3);
}

static void fade_top_page(int it, int dir) {
  anim_xfade_begin(ANIM_TOP);
  page_top(it, 0);
  anim_xfade_area(ANIM_TOP, 0, BAR_H, TW, TSH - BAR_H);
  anim_xfade_start(ANIM_TOP, dir);
  screen_present_top();
}

static int pg_item;

/* Back, and for an app the download: their count. */
static int page_btns(int it, Btn *b) {
  b[0] = (Btn){STR_BACK, UI_NO_ASSET, 1};
  if (IS_NEWS(it))
    return 1;
  b[1] = (Btn){STR_ST_DOWNLOAD, UI_NO_ASSET, 0};
  if (apps[it].state == A_UPDATE)
    b[1].label = STR_ST_UPDATE;
  else if (apps[it].state == A_INSTALLED)
    b[1].label = STR_ST_AGAIN;
  return 2;
}

static void paint_page(void) {
  volatile u8 *fb = VRAM_BOT_A;
  int it = pg_item, ty = 24, sy = 50;
  draw_filled_rect(fb, 0, 0, BW, BSH, BSH, c_bg);
  draw_filled_round_rect(fb, 8, 8, BW - 16, 196, 12, BSH, c_card);
  item_icon(fb, 22, 22, 48, BSH, it);
  if (!IS_NEWS(it) && apps[it].c) {
    ty = 14;
    ui_text(fb, 82, ty + ui_th(&ui_title), BSH, "C SDK", c_ring, c_card,
            &ui_small);
    sy = ty + ui_th(&ui_title) + ui_th(&ui_small) + 2;
  }
  ui_text_fit(fb, 82, ty, BSH, item_title(it), BW - 104, COLOR_WHITE, c_card,
              &ui_title);
  ui_text_fit(fb, 82, sy, BSH, item_side(it), BW - 104, COLOR_HM_TEXT2,
              c_card, &ui_font);
  draw_filled_rect(fb, 22, 84, BW - 44, 1, BSH, c_line);

  if (!IS_NEWS(it)) {
    static const StringId label[3] = {STR_ST_VERSION, STR_ST_SIZE,
                                      STR_ST_STATUS};
    const App *a = &apps[it];
    char size[24];
    const char *val[3];
    Color vc[3] = {COLOR_WHITE, COLOR_WHITE, COLOR_WHITE};
    size_text(size, a->size);
    val[0] = a->version[0] ? a->version : "-";
    val[1] = size;
    val[2] = L(a->state == A_INSTALLED ? STR_ST_INSTALLED
               : a->state == A_UPDATE  ? STR_ST_UPD_AVAIL
                                       : STR_ST_NOT_INST);
    if (a->state == A_UPDATE)
      vc[2] = c_ring;
    for (int i = 0; i < 3; i++) {
      int y = 96 + i * 24;
      ui_text(fb, 22, y, BSH, L(label[i]), COLOR_HM_TEXT2, c_card, &ui_font);
      ui_text(fb, BW - 22 - ui_tw(&ui_font, val[i]), y, BSH, val[i], vc[i],
              c_card, &ui_font);
    }
  }
  ui_text_mid(fb, BW / 2, 180, BSH, L(STR_ST_SCROLL),
              COLOR_HM_TEXT2, c_card, &ui_small);

  {
    Btn b[2];
    buttons(b, page_btns(it, b));
  }
}

static char why[64];
static u32 piece = PIECE_MIN;

/* bytes [from, from + n) of `path`, with the console's token. The body stays
 * in the core's buffer until the next request. */
static int get_piece(const char *path, u32 from, u32 n, const char *if_none,
                     HttpReply *r, void (*tick)(u32 ms)) {
  char extra[96], *p = put_str(extra, "Range: bytes=");
  u32 t0 = timer_ticks(), us;
  int st;
  p = put_u32(p, from);
  *p++ = '-';
  p = put_u32(p, from + n - 1u);
  p = put_str(p, "\r\n");
  if (if_none && if_none[0] && strlen(if_none) < 40) {
    p = put_str(p, "If-None-Match: ");
    p = put_str(p, if_none);
    put_str(p, "\r\n");
  }
  st = http_call("GET", path, account_token(), extra, 0, 0, 0, r, tick);
  us = timer_us_since(t0);
  if (!st) {
    piece = piece / 2u < PIECE_MIN ? PIECE_MIN : piece / 2u;
  } else if (r->body && r->len && us) {
    u32 next = (u32)((u64)r->len * PIECE_MS * 1000u / us) & ~0xFFFu;
    if (next > piece * 4u)
      next = piece * 4u;
    piece = next < PIECE_MIN ? PIECE_MIN : next > PIECE_MAX ? PIECE_MAX : next;
  }
  return st;
}

/* The server no longer takes this console's token: it was unlinked on the
 * website. Forgetting it unmounts the card, so it is mounted again. */
static void token_revoked(void) {
  account_forget();
  mounted = f_mount(&fs, "", 1) == FR_OK;
}

/* The joined network, joined first when the console is not on one. Joining
 * mounts and unmounts the card itself, which would leave aShop's mount gone,
 * so the card is let go for it and mounted again after. Files open on the
 * card stop working across a join. */
static int online(void (*tick)(u32 ms)) {
  static char ssid[33];
  int ok;
  if (http_online())
    return 1;
  if (mounted)
    f_mount(NULL, "", 0);
  ok = wifi_net_join(ssid, sizeof(ssid), why, sizeof(why), tick);
  mounted = f_mount(&fs, "", 1) == FR_OK;
  return ok;
}

enum { DL_RUN, DL_DONE, DL_FAILED, DL_CANCELLED };

static struct {
  App *app;
  int open, state, cancel, tries;
  u32 done, total, phase, t0;
  const char *title, *line1, *line2;
  char dst[ST_PATH], tmp[ST_PATH + 6], path[96];
  char old[ST_PATH]; /* the copy in the other folder, removed once installed */
  Sha256 sha;
} dl;

static FIL dl_file;

static void dl_close(int remove) {
  if (!dl.open)
    return;
  f_close(&dl_file);
  if (remove)
    f_unlink(dl.tmp);
  dl.open = 0;
}

/* <file>.part again after a join, to carry on at dl.done. Gone when it is
 * not dl.done bytes long. */
static int dl_reopen(void) {
  if (!mounted ||
      f_open(&dl_file, dl.tmp, FA_WRITE | FA_OPEN_APPEND) != FR_OK) {
    f_unlink(dl.tmp);
    return 0;
  }
  dl.open = 1;
  if (f_size(&dl_file) != dl.done) {
    dl_close(1);
    return 0;
  }
  return 1;
}

static void dl_fail(const char *line1, const char *line2) {
  dl_close(1);
  dl.state = DL_FAILED;
  dl.title = L(STR_ST_DL_FAILED);
  dl.line1 = line1;
  dl.line2 = line2 ? line2 : "";
}

/* The download goes to <file>.part, which the Home Menu ignores, and is
 * renamed over any old copy only once its SHA-256 matches the catalogue's,
 * so a cancelled or failed download leaves the old app as it was. */
static void dl_start(App *a) {
  dl.app = a;
  dl.open = 0;
  dl.done = 0;
  dl.state = DL_RUN;
  dl.cancel = 0;
  dl.tries = 0;
  dl.phase = 0;
  dl.t0 = timer_ticks();
  dl.total = a->size;
  if (!mounted) {
    dl_fail(L(STR_ST_E_NOSD), 0);
    return;
  }
  if (!account_token()) {
    dl_fail(L(STR_AC_REVOKED), L(STR_AC_REVOKED2));
    return;
  }
  put_str(put_str(put_str(dl.path, "/v1/store/files/"), a->sha), ".bin");
  f_mkdir(ST_APPS);
  if (a->c)
    f_mkdir(ST_APPS_C);
  path2(dl.dst, app_dir(a), a->file);
  put_str(put_str(dl.tmp, dl.dst), ".part");
  dl.old[0] = 0;
  if (app_on_card(a) == 2)
    path2(dl.old, a->c ? ST_APPS : ST_APPS_C, a->file);
  if (f_open(&dl_file, dl.tmp, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) {
    dl_fail(L(STR_FD_E_WRITE), 0);
    return;
  }
  dl.open = 1;
  sha256_init(&dl.sha);
}

static void dl_finish(void) {
  App *a = dl.app;
  u8 d[32];
  char got[65];
  sha256_final(&dl.sha, d);
  for (int i = 0; i < 32; i++) {
    got[2 * i] = "0123456789abcdef"[d[i] >> 4];
    got[2 * i + 1] = "0123456789abcdef"[d[i] & 15];
  }
  got[64] = 0;
  dl_close(0);
  if (!same(got, a->sha)) {
    f_unlink(dl.tmp);
    dl_fail(L(STR_ST_E_DAMAGED), 0);
    return;
  }
  f_unlink(dl.dst);
  if (f_rename(dl.tmp, dl.dst) != FR_OK) {
    f_unlink(dl.tmp);
    dl_fail(L(STR_ST_E_INSTALL), 0);
    return;
  }
  if (dl.old[0])
    f_unlink(dl.old);
  dl.state = DL_DONE;
  installs++;
  dl.title = L(STR_ST_INSTALLED);
  dl.line1 = a->name;
  dl.line2 = L(STR_ST_ON_HOME);
  record_set(a->file, a->version);
  states_update();
  status_update();
}

static void dl_tick(u32 ms);

/* One piece: a request and its reply, about PIECE_MS. A piece that does not
 * come is asked for again, TRIES times. */
static void dl_step(void) {
  HttpReply r;
  u32 n = dl.total - dl.done < piece ? dl.total - dl.done : piece;
  UINT bw;
  int st;
  if (!http_online()) {
    int ok;
    dl_close(0); /* see online() */
    ok = online(dl_tick);
    if (!dl_reopen()) {
      dl_fail(L(STR_FD_E_WRITE), 0);
      return;
    }
    if (!ok) {
      dl_fail(L(STR_AC_NO_NET), why);
      return;
    }
  }
  st = get_piece(dl.path, dl.done, n, 0, &r, dl_tick);
  if (st == 401 || st == 403) {
    dl_close(1);
    if (st == 401)
      token_revoked();
    dl_fail(L(STR_AC_REVOKED), L(STR_AC_REVOKED2));
    return;
  }
  if (!r.body || !((st == 206 && r.from == dl.done && r.len == n) ||
                   (st == 200 && !dl.done && r.len == dl.total))) {
    http_why(&r, why);
    if ((st >= 400 && st < 500) || (st == 206 && r.total != dl.total) ||
        ++dl.tries >= TRIES)
      dl_fail(L(st ? STR_ST_E_SERVER : STR_ST_E_NET), why);
    return;
  }
  dl.tries = 0;
  if (f_write(&dl_file, r.body, r.len, &bw) != FR_OK || bw != r.len) {
    dl_fail(L(STR_ST_E_FULL), 0);
    return;
  }
  sha256_update(&dl.sha, r.body, r.len);
  dl.done += r.len;
  if (dl.done >= dl.total)
    dl_finish();
}

#define DC_X 50
#define DC_Y 79
#define DC_W 300
#define DC_H 70
#define PB_X 40
#define PB_Y 191
#define PB_W 320
#define PB_H 12

static void dl_top(void) {
  volatile u8 *fb = VRAM_TOP_LA;
  char line[64], a[16], b[16];
  u32 d = dl.done, t = dl.total, fill;

  draw_filled_rect(fb, 0, BAR_H, TW, TSH - BAR_H, TSH, c_bg);
  checker_floor(fb, dl.phase);
  shade_round(fb, TSH, PB_X + 2, PB_Y + 4, PB_W - 4, PB_H, 6, COLOR_BLACK, 90);
  draw_gradient_round_rect(fb, PB_X, PB_Y, PB_W, PB_H, 6, TSH, c_track_top,
                           c_track_bot);
  draw_filled_rect(fb, PB_X + 6, PB_Y, PB_W - 12, 1, TSH, c_track_edge);
  while (t > 0xFFFFFu) {
    d >>= 1;
    t >>= 1;
  }
  fill = t ? d * (u32)PB_W / t : 0;
  if (fill > (u32)PB_W)
    fill = PB_W;
  if (fill >= (u32)PB_H)
    draw_gradient_round_rect(fb, PB_X, PB_Y, (int)fill, PB_H, 6, TSH,
                             c_fill_top, c_fill_bot);

  draw_filled_round_rect(fb, DC_X, DC_Y, DC_W, DC_H, 9, TSH, COLOR_BLACK);
  app_icon(fb, DC_X + 11, DC_Y + (DC_H - 48) / 2, 48, TSH, dl.app);
  ui_text_fit(fb, DC_X + 70, DC_Y + 15, TSH, dl.app->name, DC_W - 85,
              COLOR_WHITE, COLOR_BLACK, &ui_bold);
  if (dl.state == DL_RUN) {
    char *p;
    fmt_size(a, dl.done);
    fmt_size(b, dl.total);
    p = put_str(line, L(STR_ST_DL_PREFIX));
    p = put_str(p, a);
    p = put_str(p, " / ");
    p = put_str(p, b);
    put_str(p, ")");
  } else {
    put_str(line, dl.title);
  }
  ui_text_fit(fb, DC_X + 70, DC_Y + 37, TSH, line, DC_W - 85, COLOR_HM_TEXT2,
              COLOR_BLACK, &ui_font);
  bar_put();
}

static int dl_pct(void) {
  u32 d = dl.done, t = dl.total;
  while (t > 0xFFFFFu) {
    d >>= 1;
    t >>= 1;
  }
  return t ? (int)(d * 100u / t) : 0;
}

static void paint_dl(void) {
  static const Btn cancel[1] = {{STR_CANCEL, UI_NO_ASSET, 0}};
  static const Btn ok[1] = {{STR_OK, UI_NO_ASSET, 0}};
  volatile u8 *fb = VRAM_BOT_A;
  char pct[8];
  int run = dl.state == DL_RUN, fill;

  draw_filled_rect(fb, 0, 0, BW, BSH, BSH, c_bg);
  draw_filled_round_rect(fb, 8, 8, BW - 16, 196, 12, BSH, c_card);
  ui_text_mid_fit(fb, BW / 2, 30, BSH, run ? L(STR_ST_DOWNLOADING) : dl.title,
                  BW - 48, dl.state == DL_FAILED ? c_ring : COLOR_WHITE,
                  c_card, &ui_title);
  if (run) {
    ui_text_mid_fit(fb, BW / 2, 60, BSH, dl.app->name, BW - 48,
                    COLOR_HM_TEXT2, c_card, &ui_font);
    put_str(put_u32(pct, (u32)dl_pct()), "%");
    ui_text_mid(fb, BW / 2, 104, BSH, pct, COLOR_WHITE, c_card, &ui_title);
    fill = dl_pct() * 240 / 100;
    draw_filled_round_rect(fb, 40, 140, 240, 6, 3, BSH, c_track);
    if (fill >= 6)
      draw_filled_round_rect(fb, 40, 140, fill, 6, 3, BSH, c_ring);
    u32 us = timer_us_since(dl.t0);
    if (dl.done && us) {
      char speed[16];
      fmt_size(speed, (u32)((u64)dl.done * 1000000u / us));
      put_str(speed + strlen(speed), "/s");
      ui_text_mid(fb, BW / 2, 158, BSH, speed, COLOR_HM_TEXT2, c_card,
                  &ui_font);
    }
  } else {
    ui_text_mid_fit(fb, BW / 2, 80, BSH, dl.line1, BW - 48, COLOR_HM_TEXT2,
                    c_card, &ui_font);
    ui_text_mid_fit(fb, BW / 2, 100, BSH, dl.line2, BW - 48, COLOR_HM_TEXT2,
                    c_card, &ui_font);
  }
  buttons(run ? cancel : ok, 1);
}

/* The floor moves with time, so frames drawn during a piece and between
 * pieces agree. */
static void dl_phase(void) {
  dl.phase = (timer_us_since(dl.t0) / 1000u * 50u) & 0x1FFFFu;
}

/* While a piece comes in the core is busy and cannot present (see
 * wifi_direct_on), so the top screen's frames are drawn in its backbuffer and
 * copied to the framebuffer on show by the CPU. B is read straight from the
 * pad: touch is the core's too. */
static void dl_tick(u32 ms) {
  volatile u8 *keep = g_fb_top;
  const u32 *s = (const u32 *)VRAM_TOP_BACK;
  volatile u32 *d = (volatile u32 *)VRAM_TOP_PHYS;
  (void)ms;
  if (get_keys() & BUTTON_B)
    dl.cancel = 1;
  dl_phase();
  g_blit_src = 0;
  g_fb_top = VRAM_TOP_BACK;
  dl_top();
  g_fb_top = keep;
  for (u32 i = 0; i < TOP_FB_SIZE / 4u; i++)
    d[i] = s[i];
}

static void download_screen(App *a) {
  int last_pct = -1, tx, ty;

  anim_transition(ANIM_PUSH, ANIM_BOTH);
  dl_start(a);
  bar_draw(dl.state == DL_RUN);
  dl_top();
  screen_present_top();
  paint_dl();
  screen_present_bottom();
  touch_arm();

  while (dl.state == DL_RUN) {
    u32 k = get_keys_down();
    int p;
    if ((k & BUTTON_B) || dl.cancel || (tapped(&tx, &ty) && ty >= BB_Y - 4)) {
      flash(paint_dl, 0);
      dl_close(1);
      dl.state = DL_CANCELLED;
      break;
    }
    dl_step();
    p = dl_pct();
    dl_phase();
    dl_top();
    if (p != last_pct || dl.state != DL_RUN) {
      last_pct = p;
      paint_dl();
      anim_present(ANIM_BOTH);
    } else {
      anim_present(ANIM_TOP);
    }
    clock_poll(1);
    ui_idle();
  }

  bar_draw(0);
  if (dl.state != DL_CANCELLED) {
    dl_top();
    screen_present_top();
    paint_dl();
    screen_present_bottom();
    while (1) {
      u32 k = get_keys_down();
      if ((k & (BUTTON_A | BUTTON_B)) ||
          (tapped(&tx, &ty) && ty >= BB_Y - 4)) {
        flash(paint_dl, 0);
        break;
      }
      clock_poll(0);
      ui_idle();
    }
  }
  settle();
  anim_transition(ANIM_POP, ANIM_BOTH);
}

/* Returns the page's scroll as it was left, so the list can fade it back. */
static int page_screen(int it, int push_top) {
  AnimVal sc = {0, 0, 0, 0};
  u32 frame_at = 0;
  int target = 0, drawn = 0, max = page_max(it), n = IS_NEWS(it) ? 1 : 2;
  int tx, ty;

  pg_item = it;
  anim_transition(ANIM_PUSH, push_top ? ANIM_BOTH : ANIM_BOT);
  if (push_top) {
    page_top(it, 0);
    screen_present_top();
  }
  paint_page();
  screen_present_bottom();
  touch_arm();

  while (1) {
    u32 k = get_keys_down();
    int b = -1, ms, cx, cy;
    if (tapped(&tx, &ty)) {
      Btn pb[2];
      page_btns(it, pb);
      b = btn_at(pb, n, tx, ty);
    }
    if ((k & (BUTTON_B | BUTTON_START)) || b == 0) {
      flash(paint_page, 0);
      break;
    }
    if (n == 2 && ((k & BUTTON_A) || b == 1)) {
      flash(paint_page, 1);
      download_screen(&apps[it]);
      page_top(it, anim_px(&sc));
      screen_present_top();
      paint_page();
      screen_present_bottom();
      touch_arm();
      continue;
    }
    if (k & BUTTON_DUP)
      target -= 3 * line_h();
    if (k & BUTTON_DDOWN)
      target += 3 * line_h();
    ms = anim_frame(&frame_at);
    if (ms && cpad_read(&cx, &cy, 0, 0) && iabs(cy) > 400)
      target -= (cy > 0 ? cy - 400 : cy + 400) * ms / 1600;
    if (target < 0)
      target = 0;
    if (target > max)
      target = max;
    anim_to(&sc, target);
    if (ms)
      anim_step(&sc, ms);
    if (anim_px(&sc) != drawn) {
      drawn = anim_px(&sc);
      page_top(it, drawn);
      anim_present(ANIM_TOP);
    }
    clock_poll(0);
    ui_idle();
  }
  settle();
  anim_transition(ANIM_POP, push_top ? ANIM_BOTH : ANIM_BOT);
  return anim_px(&sc);
}

static void search_screen(void);

static void list_screen(List *l) {
  List *outer = cur_list;
  u32 frame_at = 0;
  int tx, ty;

  l->sel = 0;
  l->al.count = l->n;
  l->al.visible = VISIBLE;
  l->al.step = ROW_STEP;
  anim_list_jump(&l->al, 0);
  cur_list = l;

  anim_transition(ANIM_PUSH, ANIM_BOTH);
  page_top(l->items[0], 0);
  screen_present_top();
  paint_list();
  screen_present_bottom();
  touch_arm();

  while (1) {
    u32 k = get_keys_down();
    int prev = l->sel, open = 0, b = -1, ms;

    if ((k & BUTTON_DUP) && l->sel > 0)
      l->sel--;
    if ((k & BUTTON_DDOWN) && l->sel < l->n - 1)
      l->sel++;
    if (k & BUTTON_DLEFT)
      l->sel = l->sel > VISIBLE ? l->sel - VISIBLE : 0;
    if (k & BUTTON_DRIGHT)
      l->sel = l->sel + VISIBLE < l->n ? l->sel + VISIBLE : l->n - 1;

    if (tapped(&tx, &ty)) {
      int top = anim_list_top(&l->al, l->sel);
      b = btn_at(list_btns, 3, tx, ty);
      for (int r = 0; b < 0 && r < VISIBLE && top + r < l->n; r++) {
        int y = ROW_Y0 + r * ROW_STEP;
        if (touch_in(tx, ty, ROW_X, y - ROW_RING, ROW_W,
                     ROW_H + 2 * ROW_RING)) {
          if (top + r == l->sel)
            open = 1;
          l->sel = top + r;
          break;
        }
      }
    }

    if (l->sel != prev) {
      anim_list_to(&l->al, l->sel);
      if (!anim_list_moving(&l->al)) {
        paint_list();
        screen_present_bottom();
      }
      fade_top_page(l->items[l->sel], l->sel > prev ? 1 : -1);
    }

    if ((k & (BUTTON_B | BUTTON_START)) || b == 0) {
      flash(paint_list, 0);
      break;
    }
    if ((k & BUTTON_A) || open || b == 1) {
      int sc;
      flash(paint_list, 1);
      sc = page_screen(l->items[l->sel], 0);
      cur_list = l;
      paint_list();
      screen_present_bottom();
      if (sc)
        fade_top_page(l->items[l->sel], 0);
      touch_arm();
    } else if ((k & BUTTON_Y) || b == 2) {
      flash(paint_list, 2);
      search_screen();
      cur_list = l;
      page_top(l->items[l->sel], 0);
      screen_present_top();
      paint_list();
      screen_present_bottom();
      touch_arm();
    }

    ms = anim_frame(&frame_at);
    if (ms) {
      int show = 0;
      if (anim_list_step(&l->al, ms)) {
        paint_list();
        show |= ANIM_BOT;
      }
      if (anim_xfade_frame(ANIM_TOP))
        show |= ANIM_TOP;
      anim_present(show);
    }
    clock_poll(0);
    ui_idle();
  }
  settle();
  cur_list = outer;
  anim_transition(ANIM_POP, ANIM_BOTH);
}

/* A list of what section `s` holds; 0 when it holds nothing. */
static int section_list(int s, List *l) {
  const Section *sec = &secs[s];
  l->title = sec->title;
  l->n = 0;
  if (sec->kind == K_NEWS) {
    for (int i = 0; i < nnews && l->n < MAX_ITEMS; i++)
      l->items[l->n++] = ~i;
  } else {
    for (int i = 0; i < napps && l->n < MAX_ITEMS; i++)
      if (sec->kind == K_UPDATES ? apps[i].state == A_UPDATE
                                 : has_word(apps[i].in, sec->id))
        l->items[l->n++] = i;
  }
  return l->n;
}

static void open_section(int s) {
  static List l;
  if (section_list(s, &l))
    list_screen(&l);
  else if (secs[s].kind == K_UPDATES)
    message(L(STR_ST_NO_UPDATES), L(STR_ST_ALL_CURRENT));
  else
    message(L(STR_ST_NOTHING), L(STR_ST_SOON));
}

static void updates_list(void) {
  static List l;
  l.title = L(STR_ST_UPDATES_TITLE);
  l.n = 0;
  for (int i = 0; i < napps; i++)
    if (apps[i].state == A_UPDATE)
      l.items[l.n++] = i;
  if (l.n)
    list_screen(&l);
  else
    message(L(STR_ST_NO_UPDATES), L(STR_ST_ALL_CURRENT));
}

static void search_screen(void) {
  char q[24], title[40];
  List l;
  int ok;

  q[0] = 0;
  anim_transition(ANIM_PUSH, ANIM_BOTH);
  top_home(L(STR_ST_SEARCH_TITLE), L(STR_ST_KB_HINT));
  screen_present_top();
  ok = keyboard_edit(q, sizeof(q), KB_FILENAME, L(STR_ST_APP_NAME), c_ring);
  settle();
  if (!ok || !q[0]) {
    anim_transition(ANIM_POP, ANIM_BOTH);
    return;
  }

  l.n = 0;
  for (int i = 0; i < napps; i++)
    if (has_ci(apps[i].name, q) || has_ci(apps[i].dev, q))
      l.items[l.n++] = i;
  if (!l.n) {
    draw_filled_rect(VRAM_BOT_A, 0, 0, BW, BSH, BSH, c_bg);
    message(L(STR_ST_NO_RESULTS), q);
    anim_transition(ANIM_POP, ANIM_BOTH);
    return;
  }
  put_str(put_str(title, L(STR_ST_SEARCH_PREFIX)), q);
  l.title = title;
  list_screen(&l);
}

#define OP_N     3
#define OP_W     236
#define OP_H     (52 + OP_N * 38)
#define OP_X     ((BW - OP_W) / 2)
#define OP_Y     ((BB_Y - OP_H) / 2)
#define OP_BX    (OP_X + 14)
#define OP_BW    (OP_W - 28)
#define OP_BH    32
#define OP_BY(i) (OP_Y + 44 + (i) * 38)

static const StringId op_label[OP_N] = {STR_ST_CHECK, STR_ST_RELOAD,
                                        STR_ST_ABOUT};

static void options_draw(int item) {
  static const Color scrim = {0x00, 0x00, 0x00};
  volatile u8 *fb = VRAM_BOT_A;
  paint_main();
  draw_filled_rect_alpha(fb, 0, 0, BW, BSH, BSH, scrim, 150);
  anim_popup_behind();
  draw_filled_round_rect(fb, OP_X, OP_Y, OP_W, OP_H, 14, BSH, c_card);
  ui_text_mid(fb, BW / 2, OP_Y + 14, BSH, L(STR_ST_OPTIONS), COLOR_WHITE,
              c_card, &ui_bold);
  for (int i = 0; i < OP_N; i++) {
    draw_filled_round_rect(fb, OP_BX, OP_BY(i), OP_BW, OP_BH, 8, BSH, c_btn);
    ui_text_mid(fb, BW / 2, OP_BY(i) + (OP_BH - ui_th(&ui_font)) / 2, BSH,
                L(op_label[i]), COLOR_WHITE, c_btn, &ui_font);
  }
  draw_round_ring(fb, OP_BX - 2, OP_BY(item) - 2, OP_BW + 4, OP_BH + 4, 10, 3,
                  BSH, c_ring);
  screen_present_bottom();
}

static int options_menu(void) {
  int item = 0, pick = -1, tx, ty;
  anim_popup(OP_X, OP_Y, OP_W, OP_H);
  options_draw(item);
  touch_arm();
  while (pick < 0) {
    u32 k = get_keys_down();
    int was = item;
    if ((k & BUTTON_DUP) && item > 0)
      item--;
    if ((k & BUTTON_DDOWN) && item < OP_N - 1)
      item++;
    if (k & BUTTON_A)
      pick = item;
    if (k & (BUTTON_B | BUTTON_X))
      break;
    if (tapped(&tx, &ty)) {
      if (!touch_in(tx, ty, OP_X, OP_Y, OP_W, OP_H))
        break;
      for (int i = 0; i < OP_N; i++)
        if (touch_in(tx, ty, OP_BX, OP_BY(i), OP_BW, OP_BH))
          pick = item = i;
    }
    if (item != was || pick >= 0)
      options_draw(item);
    ui_idle();
  }
  if (pick >= 0)
    wait_ms(90);
  settle();
  anim_transition(ANIM_FADE, ANIM_BOT);
  return pick;
}

static int first_section(void) {
  for (int i = 0; i < nsecs; i++)
    if (same_ci(secs[i].id, "featured"))
      return i;
  return 0;
}

/* The screens while aShop talks to the server: the welcome screen with the
 * step on its card, and a spinner on the bottom, which is all that is drawn
 * while the core works. */
#define SPIN_Y 116

static void spin(u32 ms) {
  static const s8 dx[8] = {0, 14, 20, 14, 0, -14, -20, -14};
  static const s8 dy[8] = {-20, -14, 0, 14, 20, 14, 0, -14};
  volatile u8 *fb = VRAM_BOT_A;
  int lit = (int)(ms / 100u) % 8;
  draw_filled_rect(fb, BW / 2 - 26, SPIN_Y - 26, 52, 52, BSH, c_bg);
  for (int i = 0; i < 8; i++) {
    int age = (lit - i + 8) % 8;
    Color c = age == 0 ? c_ring : age < 3 ? COLOR_HM_TEXT2 : c_btn_lit;
    draw_filled_round_rect(fb, BW / 2 + dx[i] - 3, SPIN_Y + dy[i] - 3, 7, 7, 3,
                           BSH, c);
  }
  screen_present_bottom();
}

static void busy(const char *step) {
  volatile u8 *fb = VRAM_BOT_A;
  top_home(step, 0);
  screen_present_top();
  draw_filled_rect(fb, 0, 0, BW, BSH, BSH, c_bg);
  draw_filled_round_rect(fb, TC_X, TC_Y, TC_W, TC_H, 9, BSH, c_card);
  ui_text_mid(fb, BW / 2, TC_Y + (TC_H - ui_th(&ui_bold)) / 2, BSH, "aShop",
              COLOR_WHITE, c_card, &ui_bold);
  ui_text_mid_fit(fb, BW / 2, 200, BSH, L(STR_AC_PLEASE_WAIT), BW - 24,
                  COLOR_HM_TEXT2, c_bg, &ui_small);
  spin(0);
}

enum { GOT_OK, GOT_SAME, GOT_DENIED, GOT_FAIL };

/* A whole body into dst, a piece at a time. Every piece must be of the
 * version the first was (its ETag); when it changed meanwhile, it starts
 * again. With `have`, the ETag of a copy already kept, an unchanged body is
 * GOT_SAME and is not sent. */
static int fetch(const char *path, char *dst, u32 max, u32 *len,
                 const char *have, char *tag, HttpReply *r) {
  u32 got = 0;
  int tries = 0;
  for (;;) {
    int st = get_piece(path, got, piece, got ? 0 : have, r, spin), bad;
    if (st == 304 && !got)
      return GOT_SAME;
    if (st == 401 || st == 403)
      return GOT_DENIED;
    if (st == 200 && !got && r->body) {
      if (r->len > max)
        return GOT_FAIL;
      memcpy(dst, r->body, r->len);
      *len = r->len;
      put_str(tag, r->etag);
      return GOT_OK;
    }
    bad = st != 206 || !r->body || !r->len || !r->total || r->total > max ||
          r->from != got;
    if (!bad && got && !same(r->etag, tag)) {
      got = 0;
      bad = 1;
    }
    if (bad) {
      if ((st >= 400 && st < 500) || (st == 206 && r->total > max) ||
          ++tries > TRIES)
        return GOT_FAIL;
      continue;
    }
    if (!got)
      put_str(tag, r->etag);
    memcpy(dst + got, r->body, r->len);
    got += r->len;
    if (got >= r->total) {
      *len = r->total;
      return GOT_OK;
    }
  }
}

static u8 icon_buf[ICON_MAX];

static int icon_kept(const App *a) {
  static FILINFO fno;
  char name[32], path[ST_PATH];
  icon_name(name, a);
  path2(path, ST_DIR, name);
  return f_stat(path, &fno) == FR_OK;
}

/* The icons the card does not have yet. Each is named by its content, so a
 * new icon is a new file. */
static void icons_fetch(void) {
  char name[32], path[ST_PATH], line[64], tag[24];
  char where[48];
  int need = 0, done = 0;
  HttpReply r;
  if (!mounted)
    return;
  for (int i = 0; i < napps; i++)
    need += apps[i].icon[0] && !icon_kept(&apps[i]);
  if (!need)
    return;
  f_mkdir(ST_ICONS);
  for (int i = 0; i < napps; i++) {
    u32 len;
    char *p;
    if (!apps[i].icon[0] || icon_kept(&apps[i]))
      continue;
    p = put_str(put_str(line, L(STR_ST_ICONS)), " (");
    p = put_u32(p, (u32)++done);
    *p++ = '/';
    put_str(put_u32(p, (u32)need), ")");
    busy(line);
    icon_name(name, &apps[i]);
    path2(path, ST_DIR, name);
    put_str(put_str(put_str(where, "/v1/store/icons/"), apps[i].icon), ".png");
    if (fetch(where, (char *)icon_buf, ICON_MAX, &len, 0, tag, &r) == GOT_OK)
      write_file(path, icon_buf, len);
    else if (!r.status)
      break; /* the network is gone: the rest would fail too */
  }
}

enum { SYNC_OK, SYNC_UNLINKED, SYNC_NONE };

/* The catalogue from aShop, else the copy on the card (cat_from says which),
 * then the icons it names. SYNC_NONE when there is neither, the reason in
 * `why`. */
static int sync(void) {
  static char tag[24];
  HttpReply r;
  u32 len = 0, card = card_catalog();
  int got = GOT_FAIL;
  why[0] = 0;
  r.status = 0;
  busy(L(STR_ST_CONNECTING));
  if (online(spin)) {
    /* c=1: this OS runs apps in C, which older ones are not offered */
    got = fetch("/v1/store/catalog?c=1", cat, CAT_MAX, &len,
                card ? cat_tag : 0, tag, &r);
    if (got == GOT_FAIL)
      http_why(&r, why);
  }
  if (got == GOT_DENIED) {
    if (r.status == 401)
      token_revoked();
    return SYNC_UNLINKED;
  }
  if (got == GOT_OK && parse(len)) {
    put_str(cat_tag, tag);
    card_save(len);
    cat_from = CAT_SERVER;
  } else if (card && (len = card_catalog()) && parse(len)) {
    cat_from = got == GOT_SAME ? CAT_SERVER : CAT_CARD;
  } else {
    if (!why[0])
      put_str(why, L(STR_AC_E_REPLY));
    nsecs = napps = nnews = 0;
    cat_from = CAT_NONE;
    return SYNC_NONE;
  }
  if (cat_from == CAT_SERVER)
    icons_fetch();
  return SYNC_OK;
}

static const Btn need_btns[2] = {{STR_BACK, UI_NO_ASSET, 1},
                                 {STR_AC_LINK, UI_NO_ASSET, 0}};

static void paint_need(void) {
  volatile u8 *fb = VRAM_BOT_A;
  draw_filled_rect(fb, 0, 0, BW, BSH, BSH, c_bg);
  draw_filled_round_rect(fb, 8, 8, BW - 16, 196, 12, BSH, c_card);
  ui_icon(fb, (BW - 48) / 2, 20, 48, BSH, ASSET_ICON_USER_48, 0, COLOR_WHITE);
  ui_text_mid_fit(fb, BW / 2, 78, BSH, L(STR_ST_NEED_TITLE), BW - 40,
                  COLOR_WHITE, c_card, &ui_bold);
  for (int i = 0; i < 3; i++)
    ui_text_mid_fit(fb, BW / 2, 108 + i * 20, BSH,
                    L((StringId)(STR_ST_NEED_L1 + i)), BW - 40,
                    COLOR_HM_TEXT2, c_card, &ui_font);
  ui_text_mid(fb, BW / 2, 176, BSH, ACCOUNT_SITE, COLOR_WHITE, c_card,
              &ui_small);
  buttons(need_btns, 2);
}

/* aShop without an account says so and offers to link one (Settings >
 * Aurora Account's screen). 1 when the console is linked on return. */
static int need_account(const char *owner) {
  int tx, ty;
  anim_transition(ANIM_PUSH, ANIM_BOTH);
  for (;;) {
    int b;
    bar_draw(0);
    top_home(L(STR_ST_WELCOME), 0);
    screen_present_top();
    paint_need();
    screen_present_bottom();
    touch_arm();
    for (;;) {
      u32 k = get_keys_down();
      b = (k & (BUTTON_B | BUTTON_START)) ? 0 : (k & BUTTON_A) ? 1 : -1;
      if (b < 0 && tapped(&tx, &ty))
        b = btn_at(need_btns, 2, tx, ty);
      if (b >= 0)
        break;
      ui_idle();
    }
    flash(paint_need, b);
    settle();
    if (b == 0) {
      anim_transition(ANIM_POP, ANIM_BOTH);
      return 0;
    }
    anim_transition(ANIM_PUSH, ANIM_BOTH);
    account_screen(owner);
    anim_transition(ANIM_POP, ANIM_BOTH);
    if (account_linked())
      return 1;
  }
}

/* What stopped aShop, on a card like a download's result, with OK. */
static const char *nt_title, *nt_line;

static void paint_notice(void) {
  static const Btn ok[1] = {{STR_OK, UI_NO_ASSET, 0}};
  volatile u8 *fb = VRAM_BOT_A;
  draw_filled_rect(fb, 0, 0, BW, BSH, BSH, c_bg);
  draw_filled_round_rect(fb, 8, 8, BW - 16, 196, 12, BSH, c_card);
  ui_icon(fb, (BW - 48) / 2, 24, 48, BSH, ASSET_ICON_GLOBE_48, 0, COLOR_WHITE);
  ui_text_mid_fit(fb, BW / 2, 86, BSH, nt_title, BW - 48, c_ring, c_card,
                  &ui_bold);
  ui_text_mid_fit(fb, BW / 2, 116, BSH, nt_line, BW - 48, COLOR_HM_TEXT2,
                  c_card, &ui_font);
  buttons(ok, 1);
}

static void notice(const char *title, const char *line) {
  int tx, ty;
  nt_title = title;
  nt_line = line;
  anim_transition(ANIM_FADE, ANIM_BOTH);
  top_home(title, 0);
  screen_present_top();
  paint_notice();
  screen_present_bottom();
  touch_arm();
  for (;;) {
    u32 k = get_keys_down();
    if ((k & (BUTTON_A | BUTTON_B)) || (tapped(&tx, &ty) && ty >= BB_Y - 4)) {
      flash(paint_notice, 0);
      break;
    }
    ui_idle();
  }
  settle();
}

/* Options: the catalogue again from aShop. 0 when aShop has to close, the
 * console having been unlinked. */
static int refresh(void) {
  int res;
  anim_transition(ANIM_FADE, ANIM_BOTH);
  res = sync();
  if (res == SYNC_UNLINKED) {
    notice(L(STR_AC_REVOKED), L(STR_AC_REVOKED2));
    return 0;
  }
  if (res == SYNC_NONE)
    notice(L(STR_ST_NO_REACH), why);
  records_load();
  states_update();
  status_update();
  art_all();
  m_sel = first_section();
  anim_jump(&m_scroll, m_sel * TILE_STEP);
  anim_transition(ANIM_FADE, ANIM_BOTH);
  draw_main();
  return 1;
}

/* 0 when aShop has to close. */
static int main_options(void) {
  int pick;
  flash(paint_main, 2);
  pick = options_menu();
  if (pick == 0) {
    if (!refresh())
      return 0;
    updates_list();
  } else if (pick == 1) {
    if (!refresh())
      return 0;
    message(L(STR_ST_RELOADED),
            L(cat_from == CAT_SERVER ? STR_ST_FROM_SERVER : STR_ST_FROM_CARD));
  } else if (pick == 2) {
    about_build();
    page_screen(ITEM_ABOUT, 1);
  }
  return 1;
}

static void main_screen(void) {
  u32 frame_at = 0;
  int down = 0, drag = 0, x0 = 0, y0 = 0, lx = 0, sc0 = 0, drawn, drawn_sel;

  m_sel = first_section();
  m_scroll = (AnimVal){0, 0, 0, 0};
  anim_jump(&m_scroll, m_sel * TILE_STEP);
  m_title_t0 = 0;
  draw_main();
  if (cat_from == CAT_CARD) {
    message(L(STR_ST_NO_REACH), L(STR_ST_SAVED));
    paint_main();
    screen_present_bottom();
  }
  drawn = anim_px(&m_scroll);
  drawn_sel = m_sel;
  touch_arm();

  while (1) {
    u32 k = get_keys_down();
    int prev = m_sel, act = -1, x = 0, y = 0, ms;
    int now = touch_read(&x, &y, 0, 0);
    touch_tap(0, 0);

    if (touch_wait) {
      touch_wait = now;
    } else if (now && !down) {
      down = 1;
      drag = 0;
      x0 = lx = x;
      y0 = y;
      sc0 = anim_px(&m_scroll);
    } else if (now) {
      lx = x;
      if (!drag && y0 >= CAR_Y0 && y0 < CAR_Y1 && iabs(x - x0) > SLOP)
        drag = 1;
      if (drag)
        anim_jump(&m_scroll, sc0 - (x - x0));
    } else if (down) {
      down = 0;
      if (drag) {
        int dx = lx - x0, at = sc0 - dx, s;
        drag = 0;
        s = (at + (at >= 0 ? TILE_STEP / 2 : -TILE_STEP / 2)) / TILE_STEP;
        if (s == m_sel && iabs(dx) > SWIPE)
          s += dx < 0 ? 1 : -1;
        m_sel = s < 0 ? 0 : s >= nsecs ? nsecs - 1 : s;
        anim_to(&m_scroll, m_sel * TILE_STEP);
      } else if (y0 >= BB_Y - 4) {
        act = btn_at(main_btns, 3, x0, y0);
      } else if (y0 >= CAR_Y0 && y0 < CAR_Y1) {
        if (x0 < TILE_X - RING_PAD)
          m_sel = m_sel > 0 ? m_sel - 1 : 0;
        else if (x0 >= TILE_X + TILE_W + RING_PAD)
          m_sel = m_sel < nsecs - 1 ? m_sel + 1 : m_sel;
        else
          act = 1;
      }
    }

    if ((k & (BUTTON_DLEFT | BUTTON_L)) && m_sel > 0)
      m_sel--;
    if ((k & (BUTTON_DRIGHT | BUTTON_R)) && m_sel < nsecs - 1)
      m_sel++;
    if (k & BUTTON_A)
      act = 1;
    if (k & BUTTON_Y)
      act = 0;
    if (k & BUTTON_X)
      act = 2;
    if (k & (BUTTON_B | BUTTON_START))
      break;

    if (m_sel != prev) {
      anim_to(&m_scroll, m_sel * TILE_STEP);
      m_title_t0 = anim_now();
    }

    if (act >= 0) {
      down = drag = 0;
      if (act == 1 && nsecs) {
        flash(paint_main, 1);
        open_section(m_sel);
      } else if (act == 0) {
        flash(paint_main, 0);
        search_screen();
      } else if (act == 2 && !main_options()) {
        break;
      }
      draw_main();
      touch_arm();
    }

    ms = anim_frame(&frame_at);
    if (ms) {
      anim_step(&m_scroll, ms);
      if (anim_px(&m_scroll) != drawn || m_sel != drawn_sel ||
          anim_ms(m_title_t0) < 200) {
        drawn = anim_px(&m_scroll);
        drawn_sel = m_sel;
        paint_main();
        anim_present(ANIM_BOT);
      }
    }
    clock_poll(0);
    ui_idle();
  }
  settle();
}

int store_screen(const char *owner) {
  int res;
  installs = 0;
  cat_from = CAT_NONE;
  nsecs = napps = nnews = 0;
  status_update();
  account_load();
  /* The bag first, for the screens before the catalogue. Account screens
   * mount the card themselves, so it is not left mounted for them. */
  mounted = f_mount(&fs, "", 1) == FR_OK;
  arena_at = (u8 *)STORE_ARENA_ADDR;
  bag = art_load("bag.png", BAG_W, BAG_H, 0);
  if (!account_linked()) {
    f_mount(NULL, "", 0);
    mounted = 0;
    if (!need_account(owner))
      return 0;
    mounted = f_mount(&fs, "", 1) == FR_OK;
  } else {
    anim_transition(ANIM_PUSH, ANIM_BOTH);
  }
  bar_draw(0);
  res = sync();
  if (res == SYNC_OK) {
    records_load();
    states_update();
    status_update();
    art_all();
    anim_transition(ANIM_FADE, ANIM_BOTH);
    main_screen();
  } else if (res == SYNC_UNLINKED) {
    notice(L(STR_AC_REVOKED), L(STR_AC_REVOKED2));
  } else {
    notice(L(STR_ST_NO_REACH), why);
  }
  wifi_direct_off();
  if (mounted)
    f_mount(NULL, "", 0);
  mounted = 0;
  wrapped = 0;
  anim_transition(ANIM_POP, ANIM_BOTH);
  return installs;
}
