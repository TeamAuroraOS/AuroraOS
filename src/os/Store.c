/* aShop: a carousel of sections on the bottom screen, lists of apps and news,
 * app pages, and downloads into SD:/Aurora/Apps. The catalogue is
 * SD:/Aurora/Store/catalog.txt when the card has one, else the demo built in
 * below; banners and the welcome art are PNGs beside it, drawn as plain tiles
 * when missing. There is no download transport yet: an app is copied from
 * SD:/Aurora/Store/Packages when its package is there, and otherwise the
 * download only runs its progress. See docs/store.md. */

#include "store.h"
#include "anim.h"
#include "assets.h"
#include "container.h"
#include "ff.h"
#include "image.h"
#include "keyboard.h"
#include "lang.h"
#include "power.h"
#include "statusbar.h"
#include "timer.h"
#include "touch.h"
#include "ui.h"
#include <string.h>

#define TW    TOP_SCREEN_WIDTH
#define TSH   TOP_SCREEN_HEIGHT
#define BW    BOT_SCREEN_WIDTH
#define BSH   BOT_SCREEN_HEIGHT
#define BAR_H STATUS_APP_HEIGHT

#define ST_DIR      "0:/Aurora/Store"
#define ST_CATALOG  ST_DIR "/catalog.txt"
#define ST_RECORDS  ST_DIR "/installed.txt"
#define ST_PACKAGES ST_DIR "/Packages"
#define ST_APPS     "0:/Aurora/Apps"
#define ST_PATH     160

#define MAX_SECS  8
#define MAX_APPS  48
#define MAX_NEWS  16
#define MAX_ITEMS (MAX_APPS + MAX_NEWS)
#define CAT_MAX   (32 * 1024)
#define POOL_MAX  (24 * 1024)
#define FILE_MAX  64
#define VER_MAX   16
#define LINES_MAX 200 /* of a page's text, wrapped */

/* Downloads without a network: the bytes per millisecond a simulated link
 * moves, and the shortest a download may take, so its screen can be seen. */
#define DEMO_RATE   3072u
#define DEMO_MIN_MS 2500u
#define CHUNK       (16 * 1024)

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
  u32 size;
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

static const char demo_catalog[] =
    "# The aShop demo. SD:/Aurora/Store/catalog.txt replaces it; the format\n"
    "# is in docs/store.md.\n"
    "\n"
    "[section news]\n"
    "title=News\n"
    "banner=news.png\n"
    "kind=news\n"
    "color=2F8FE0\n"
    "\n"
    "[section featured]\n"
    "title=Featured Software\n"
    "banner=featured.png\n"
    "color=2EC48A\n"
    "\n"
    "[section apps]\n"
    "title=Apps\n"
    "banner=apps.png\n"
    "color=9B3FE0\n"
    "\n"
    "[section games]\n"
    "title=Games\n"
    "banner=games.png\n"
    "color=E8702A\n"
    "\n"
    "[section new3ds]\n"
    "title=Better on New 3DS\n"
    "banner=new3ds.png\n"
    "color=3AB57E\n"
    "\n"
    "[section updates]\n"
    "title=Download updates\n"
    "banner=updates.png\n"
    "kind=updates\n"
    "color=1E5E45\n"
    "\n"
    "[app tetris]\n"
    "name=Tetris\n"
    "dev=Aurora\n"
    "version=1.1\n"
    "size=28616\n"
    "file=Tetris.bin\n"
    "in=featured games\n"
    "text=The falling-block classic, written in Auric, Aurora's own app "
    "language.\n"
    "text=\n"
    "text=Left and Right move, Down drops softly, Up drops at once, and A or B "
    "turns the piece. START starts again after a game over, and HOME goes back "
    "to the Home Menu.\n"
    "text=\n"
    "text=Sounds play from SD:/Aurora/Apps/TETRIS when you put them there.\n"
    "\n"
    "[app rainbow]\n"
    "name=Rainbow\n"
    "dev=Aurora\n"
    "version=1.0\n"
    "size=1344\n"
    "file=Rainbow.bin\n"
    "in=apps\n"
    "text=The Auric sample app: seven bands of colour across the top screen.\n"
    "text=\n"
    "text=It is the smallest complete Aurora app, with its own Home Menu icon, "
    "and the place to start if you want to make one.\n"
    "\n"
    "[app appname]\n"
    "name=App name thing\n"
    "dev=aShop demo\n"
    "version=0.9\n"
    "size=4M\n"
    "in=featured apps\n"
    "text=App name thing's description goes here. The description can be as "
    "long as you like, and later you will even be able to add pictures to "
    "this page!\n"
    "text=\n"
    "text=Up and Down on the D-pad, or the Circle Pad, scroll a long page like "
    "this one.\n"
    "\n"
    "[app application]\n"
    "name=Application Name\n"
    "dev=aShop demo\n"
    "version=2.0\n"
    "size=100M\n"
    "in=featured new3ds\n"
    "text=A large download, to watch the download screen at work. It takes "
    "about half a minute, and B or Cancel stops it.\n"
    "\n"
    "[app notes]\n"
    "name=Pixel Notes\n"
    "dev=aShop demo\n"
    "version=1.3\n"
    "size=512K\n"
    "in=apps\n"
    "text=Jot notes and lists on the touchscreen and keep them on the SD "
    "card.\n"
    "\n"
    "[app painter]\n"
    "name=Sky Painter\n"
    "dev=aShop demo\n"
    "version=1.0\n"
    "size=6M\n"
    "in=apps new3ds\n"
    "text=Paint with the stylus on the bottom screen and watch the picture "
    "take shape on the top one.\n"
    "\n"
    "[app synth]\n"
    "name=Pocket Synth\n"
    "dev=aShop demo\n"
    "version=0.4\n"
    "size=3M\n"
    "in=apps featured\n"
    "text=A small synthesizer: notes on the touchscreen, tone and echo on the "
    "buttons.\n"
    "\n"
    "[app garden]\n"
    "name=Block Garden\n"
    "dev=aShop demo\n"
    "version=1.2\n"
    "size=12M\n"
    "in=games new3ds\n"
    "text=Grow a garden one block at a time. Plant, water and wait.\n"
    "\n"
    "[app courier]\n"
    "name=Star Courier\n"
    "dev=aShop demo\n"
    "version=1.0\n"
    "size=24M\n"
    "in=games new3ds\n"
    "text=Fly parcels between the stars before the clock runs out. Made for "
    "the 3D screen.\n"
    "\n"
    "[app chess]\n"
    "name=Chess Club\n"
    "dev=aShop demo\n"
    "version=2.1\n"
    "size=2M\n"
    "in=games\n"
    "text=Chess against the console, or pass it to a friend.\n"
    "\n"
    "[news welcome]\n"
    "title=Welcome to aShop\n"
    "date=Oct 2026\n"
    "text=aShop is where apps for Aurora will be found and downloaded.\n"
    "text=\n"
    "text=This is a demo. The catalogue is built in, and a download copies "
    "the app from /Aurora/Store/Packages on the SD card when it is there; "
    "otherwise only the progress runs. Downloaded apps go to /Aurora/Apps and "
    "appear on the Home Menu.\n"
    "\n"
    "[news auric]\n"
    "title=Make your own apps\n"
    "date=Oct 2026\n"
    "text=Aurora apps are written in Auric, a small language that compiles to "
    "an app the Home Menu can launch.\n"
    "text=\n"
    "text=From auric-lang, run: python -m compiler.aurc build app.aur --icon "
    "app.icon -o APP.BIN, then copy APP.BIN to /Aurora/Apps.\n"
    "\n"
    "[news shots]\n"
    "title=Tip: screenshots\n"
    "date=Oct 2026\n"
    "text=Press L and R together anywhere to save both screens as one picture "
    "in /Aurora/Screenshots.\n";

static char cat[CAT_MAX + 1];
static char pool[POOL_MAX];
static int pool_used;
static Section secs[MAX_SECS];
static App apps[MAX_APPS];
static News news[MAX_NEWS];
static int nsecs, napps, nnews;
static int cat_on_card;

static char rec_file[MAX_APPS][FILE_MAX];
static char rec_ver[MAX_APPS][VER_MAX];
static int nrec;

static FATFS fs;
static int mounted;
static int installs;
static u8 *bag;
static u8 *arena_at;

static char status_text[40];
static int bar_lit = -1;

/* The lines of the text last wrapped for a page, kept while it is shown. */
static u16 ln_off[LINES_MAX];
static u8 ln_len[LINES_MAX];
static int nlines;
static const char *wrapped;

static char lower(char c) {
  return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
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

/* Text lines join into one string, a line break between them. A record's lines
 * are the newest thing in the pool while it is read, so each adds on. */
static void text_add(const char **field, const char *v) {
  int n = (int)strlen(v);
  if (pool_used + n + 2 > POOL_MAX)
    return;
  if (*field) {
    if (*field + strlen(*field) + 1 != pool + pool_used)
      return;
    pool[pool_used - 1] = '\n';
  } else {
    *field = pool + pool_used;
  }
  memcpy(pool + pool_used, v, (size_t)n);
  pool_used += n;
  pool[pool_used++] = 0;
}

static const char *pool_join(const char *a, const char *b) {
  int na = (int)strlen(a), nb = (int)strlen(b);
  char *s = pool + pool_used;
  if (pool_used + na + nb + 1 > POOL_MAX)
    return "";
  memcpy(s, a, (size_t)na);
  memcpy(s + na, b, (size_t)nb);
  s[na + nb] = 0;
  pool_used += na + nb + 1;
  return s;
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

/* "28616", "512K" or "100M". */
static u32 parse_size(const char *s) {
  u32 v = 0;
  while (*s >= '0' && *s <= '9')
    v = v * 10u + (u32)(*s++ - '0');
  if (lower(*s) == 'k')
    v <<= 10;
  else if (lower(*s) == 'm')
    v <<= 20;
  return v;
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

enum { R_NONE, R_SECTION, R_APP, R_NEWS };

/* A file name alone: the catalogue must not reach outside its folders. */
static int plain_name(const char *v) {
  if (!v[0] || v[0] == '.')
    return 0;
  for (; *v; v++)
    if (*v == '/' || *v == '\\' || *v == ':')
      return 0;
  return 1;
}

static void assign(int kind, int i, const char *k, const char *v) {
  if (kind == R_SECTION) {
    Section *s = &secs[i];
    if (same_ci(k, "title"))
      s->title = v;
    else if (same_ci(k, "banner") && plain_name(v))
      s->banner = v;
    else if (same_ci(k, "kind"))
      s->kind = same_ci(v, "news") ? K_NEWS
                : same_ci(v, "updates") ? K_UPDATES : K_APPS;
    else if (same_ci(k, "color"))
      s->color = parse_color(v, s->color);
  } else if (kind == R_APP) {
    App *a = &apps[i];
    if (same_ci(k, "name"))
      a->name = v;
    else if (same_ci(k, "dev"))
      a->dev = v;
    else if (same_ci(k, "version"))
      a->version = v;
    else if (same_ci(k, "size"))
      a->size = parse_size(v);
    else if (same_ci(k, "file") && plain_name(v) && strlen(v) < FILE_MAX - 6)
      a->file = v;
    else if (same_ci(k, "icon") && plain_name(v))
      a->icon = v;
    else if (same_ci(k, "in"))
      a->in = v;
    else if (same_ci(k, "text"))
      text_add(&a->text, v);
  } else if (kind == R_NEWS) {
    News *n = &news[i];
    if (same_ci(k, "title"))
      n->title = v;
    else if (same_ci(k, "date"))
      n->date = v;
    else if (same_ci(k, "text"))
      text_add(&n->text, v);
  }
}

static void parse(char *s) {
  static const Color dflt = {0x2E, 0xC4, 0x8A};
  int kind = R_NONE, idx = 0;

  while (*s) {
    char *line = s, *eq;
    while (*s && *s != '\n')
      s++;
    if (*s)
      *s++ = 0;
    line = trim(line);
    if (!*line || *line == '#')
      continue;

    if (*line == '[') {
      char *type = line + 1, *id = type, *end;
      while (*id && *id != ' ' && *id != ']')
        id++;
      if (*id == ' ')
        *id++ = 0;
      end = strchr(id, ']');
      if (end)
        *end = 0;
      if (*type == ']')
        *type = 0;
      id = trim(id);
      kind = R_NONE;
      if (same_ci(type, "section") && nsecs < MAX_SECS) {
        kind = R_SECTION;
        idx = nsecs++;
        secs[idx] = (Section){id, id, "", K_APPS, dflt, 0};
      } else if (same_ci(type, "app") && napps < MAX_APPS) {
        kind = R_APP;
        idx = napps++;
        apps[idx] = (App){id, id, "", "", 0, "", "", 0, 0, A_NEW, 0, 0};
      } else if (same_ci(type, "news") && nnews < MAX_NEWS) {
        kind = R_NEWS;
        idx = nnews++;
        news[idx] = (News){id, id, "", 0};
      }
      continue;
    }

    eq = strchr(line, '=');
    if (!eq || kind == R_NONE)
      continue;
    *eq = 0;
    assign(kind, idx, trim(line), trim(eq + 1));
  }

  for (int i = 0; i < napps; i++) {
    if (!apps[i].file)
      apps[i].file = pool_join(apps[i].id, ".bin");
    if (!apps[i].text)
      apps[i].text = "";
  }
  for (int i = 0; i < nnews; i++)
    if (!news[i].text)
      news[i].text = "";
}

static void catalog_load(void) {
  static FIL f;
  UINT br = 0;
  int n = 0;

  nsecs = napps = nnews = 0;
  pool_used = 0;
  cat_on_card = 0;
  if (mounted && f_open(&f, ST_CATALOG, FA_READ) == FR_OK) {
    if (f_read(&f, cat, CAT_MAX, &br) == FR_OK && br > 0) {
      n = (int)br;
      cat_on_card = 1;
    }
    f_close(&f);
  }
  if (!cat_on_card) {
    n = (int)sizeof(demo_catalog) - 1;
    if (n > CAT_MAX)
      n = CAT_MAX;
    memcpy(cat, demo_catalog, (size_t)n);
  }
  cat[n] = 0;
  parse(cat);
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

static int app_on_card(const App *a) {
  static FILINFO fno;
  char path[ST_PATH];
  if (!mounted)
    return 0;
  path2(path, ST_APPS, a->file);
  return f_stat(path, &fno) == FR_OK;
}

/* An app on the card that aShop did not record, or recorded at another
 * version, has an update. */
static void states_update(void) {
  for (int i = 0; i < napps; i++) {
    App *a = &apps[i];
    const char *rec = record_of(a->file);
    if (!app_on_card(a))
      a->state = A_NEW;
    else if (!a->version[0] || (rec && same_ci(rec, a->version)))
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
  if (n) {
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

static void store_load(void) {
  arena_at = (u8 *)STORE_ARENA_ADDR;
  wrapped = 0;
  catalog_load();
  records_load();
  states_update();
  status_update();
  bag = art_load("bag.png", BAG_W, BAG_H, 0);
  for (int i = 0; i < nsecs; i++)
    secs[i].art = art_load(secs[i].banner, TILE_W, TILE_H, TILE_R);
  for (int i = 0; i < napps; i++) {
    apps[i].icon_lg = art_load(apps[i].icon, 48, 48, 10);
    apps[i].icon_sm = apps[i].icon_lg ? art_load(apps[i].icon, 24, 24, 5) : 0;
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
  static char text[640];
  char *p = text;
  p = put_str(p, L(STR_ST_AB_INTRO));
  p = put_str(p, "\n\n");
  p = put_str(p, L(STR_ST_AB_CAT));
  p = put_str(p, L(cat_on_card ? STR_ST_AB_CARD : STR_ST_AB_DEMO));
  p = put_str(p, L(STR_ST_AB_WITH));
  p = put_u32(p, (u32)napps);
  p = put_str(p, L(napps == 1 ? STR_ST_AB_APP : STR_ST_AB_APPS));
  p = put_u32(p, (u32)nnews);
  p = put_str(p, L(nnews == 1 ? STR_ST_AB_NEWS1 : STR_ST_AB_NEWSN));
  p = put_str(p, "\n\n");
  put_str(p, L(STR_ST_AB_SERVER));
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
  ui_text_fit(fb, PG_X + 15, y0 + (PG_CARD_H - ui_th(&ui_bold)) / 2, TSH,
              item_title(it), PG_W - 30 - sw, COLOR_WHITE, COLOR_BLACK,
              &ui_bold);

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
  int it = pg_item;
  draw_filled_rect(fb, 0, 0, BW, BSH, BSH, c_bg);
  draw_filled_round_rect(fb, 8, 8, BW - 16, 196, 12, BSH, c_card);
  item_icon(fb, 22, 22, 48, BSH, it);
  ui_text_fit(fb, 82, 24, BSH, item_title(it), BW - 104, COLOR_WHITE, c_card,
              &ui_title);
  ui_text_fit(fb, 82, 50, BSH, item_side(it), BW - 104, COLOR_HM_TEXT2,
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

enum { DL_RUN, DL_DONE, DL_FAILED, DL_CANCELLED };

static struct {
  App *app;
  int real, existed, state;
  u32 done, total, t0, phase;
  const char *title, *line1, *line2;
  char dst[ST_PATH], tmp[ST_PATH + 6];
} dl;

static FIL dl_src, dl_dst;
static u8 dl_buf[CHUNK];

static void dl_close(int remove) {
  if (!dl.real)
    return;
  f_close(&dl_src);
  f_close(&dl_dst);
  if (remove)
    f_unlink(dl.tmp);
  dl.real = 0;
}

static void dl_fail(const char *why) {
  dl_close(1);
  dl.state = DL_FAILED;
  dl.title = L(STR_ST_DL_FAILED);
  dl.line1 = why;
  dl.line2 = "";
}

static void dl_start(App *a) {
  char src[ST_PATH];
  aos_header_t h;

  dl.app = a;
  dl.real = 0;
  dl.done = 0;
  dl.state = DL_RUN;
  dl.phase = 0;
  dl.existed = app_on_card(a);
  dl.total = a->size ? a->size : (1u << 20);
  dl.t0 = timer_ticks();
  if (!mounted) {
    dl_fail(L(STR_ST_E_NOSD));
    return;
  }
  path2(src, ST_PACKAGES, a->file);
  if (f_open(&dl_src, src, FA_READ) != FR_OK)
    return;
  if (aurora_parse_header(&dl_src, &h) != AURORA_OK ||
      f_lseek(&dl_src, 0) != FR_OK) {
    f_close(&dl_src);
    dl_fail(L(STR_ST_E_NOTAPP));
    return;
  }
  dl.total = f_size(&dl_src);
  f_mkdir(ST_APPS);
  path2(dl.dst, ST_APPS, a->file);
  put_str(put_str(dl.tmp, dl.dst), ".part");
  if (f_open(&dl_dst, dl.tmp, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) {
    f_close(&dl_src);
    dl_fail(L(STR_FD_E_WRITE));
    return;
  }
  dl.real = 1;
}

static void dl_finish(void) {
  App *a = dl.app;
  int real = dl.real;
  dl.state = DL_DONE;
  if (real) {
    dl_close(0);
    f_unlink(dl.dst);
    if (f_rename(dl.tmp, dl.dst) != FR_OK) {
      f_unlink(dl.tmp);
      dl_fail(L(STR_ST_E_INSTALL));
      return;
    }
    installs++;
    dl.title = L(STR_ST_INSTALLED);
    dl.line1 = a->name;
    dl.line2 = L(STR_ST_ON_HOME);
  } else if (dl.existed) {
    dl.title = L(STR_ST_UP_TO_DATE);
    dl.line1 = L(STR_ST_DEMO1);
    dl.line2 = L(STR_ST_DEMO2);
  } else {
    dl.title = L(STR_ST_DL_DONE);
    dl.line1 = L(STR_ST_DEMO3);
    dl.line2 = L(STR_ST_DEMO4);
  }
  if (real || dl.existed)
    record_set(a->file, a->version);
  states_update();
  status_update();
}

static void dl_step(void) {
  u32 ms = timer_us_since(dl.t0) / 1000u, dur = dl.total / DEMO_RATE, want;
  if (dur < DEMO_MIN_MS)
    dur = DEMO_MIN_MS;
  if (ms >= dur) {
    want = dl.total;
  } else {
    /* total * ms / dur in 32 bits: the elapsed share in 1/4096ths. */
    u32 f = ms * 4096u / dur;
    want = (dl.total >> 12) * f + (((dl.total & 4095u) * f) >> 12);
  }

  for (int n = 0; dl.real && dl.done < want && n < 4; n++) {
    UINT len = want - dl.done > CHUNK ? CHUNK : want - dl.done, br, bw;
    if (f_read(&dl_src, dl_buf, len, &br) != FR_OK || br != len) {
      dl_fail(L(STR_ST_E_READ));
      return;
    }
    if (f_write(&dl_dst, dl_buf, len, &bw) != FR_OK || bw != len) {
      dl_fail(L(STR_ST_E_FULL));
      return;
    }
    dl.done += len;
  }
  if (!dl.real)
    dl.done = want;
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
  } else {
    ui_text_mid_fit(fb, BW / 2, 80, BSH, dl.line1, BW - 48, COLOR_HM_TEXT2,
                    c_card, &ui_font);
    ui_text_mid_fit(fb, BW / 2, 100, BSH, dl.line2, BW - 48, COLOR_HM_TEXT2,
                    c_card, &ui_font);
  }
  buttons(run ? cancel : ok, 1);
}

static void download_screen(App *a) {
  u32 frame_at = 0;
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
    int ms;
    if ((k & BUTTON_B) || (tapped(&tx, &ty) && ty >= BB_Y - 4)) {
      flash(paint_dl, 0);
      dl_close(1);
      dl.state = DL_CANCELLED;
      break;
    }
    dl_step();
    ms = anim_frame(&frame_at);
    if (ms) {
      int show = ANIM_TOP, p = dl_pct();
      dl.phase = (dl.phase + (u32)ms * 50u) & 0x1FFFFu;
      dl_top();
      if (p != last_pct || dl.state != DL_RUN) {
        last_pct = p;
        paint_dl();
        show |= ANIM_BOT;
      }
      anim_present(show);
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

static void main_options(void) {
  int pick;
  flash(paint_main, 2);
  pick = options_menu();
  if (pick == 0) {
    records_load();
    states_update();
    status_update();
    bar_draw(0);
    paint_main();
    updates_list();
  } else if (pick == 1) {
    store_load();
    m_sel = first_section();
    anim_jump(&m_scroll, m_sel * TILE_STEP);
    paint_main();
    message(L(STR_ST_RELOADED),
            L(cat_on_card ? STR_ST_FROM_SD : STR_ST_BUILTIN));
  } else if (pick == 2) {
    about_build();
    page_screen(ITEM_ABOUT, 1);
  }
}

static void main_screen(void) {
  u32 frame_at = 0;
  int down = 0, drag = 0, x0 = 0, y0 = 0, lx = 0, sc0 = 0, drawn, drawn_sel;

  m_sel = first_section();
  m_scroll = (AnimVal){0, 0, 0, 0};
  anim_jump(&m_scroll, m_sel * TILE_STEP);
  m_title_t0 = 0;
  draw_main();
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
      } else if (act == 2) {
        main_options();
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

int store_screen(void) {
  installs = 0;
  mounted = f_mount(&fs, "", 1) == FR_OK;
  anim_transition(ANIM_PUSH, ANIM_BOTH);
  store_load();
  main_screen();
  if (mounted)
    f_mount(NULL, "", 0);
  mounted = 0;
  wrapped = 0;
  anim_transition(ANIM_POP, ANIM_BOTH);
  return installs;
}
