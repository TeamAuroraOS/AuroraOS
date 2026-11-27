/* The AuroraOS updater (docs/update.md). Settings > System Update downloads it
 * to /Aurora/Update/Update.bin and starts it. It asks the update server for
 * the newest release, downloads each file that differs from the card's,
 * checks it against the server's SHA-256, moves the files it replaces into
 * /Aurora/Update/backup, puts the new ones in place and starts the new
 * AuroraOS. That OS deletes this file when it starts.
 *
 * It is downloaded fresh for every update, so changes to how an update
 * installs go here, not in the OS. Build: make updater (output/Update.bin),
 * attached to a GitHub release as Update.bin. */
#include <aurora_app.h>
#include <bearssl_hash.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "update.h"

/* include/loader.h: the HOME return descriptor [magic, size] and the copy of
 * the OS the return stub restores. The new OS is put there, so leaving the
 * app starts it. */
#define RETURN_DESC_ADDR  0x25008000u
#define RETURN_MAGIC      0x52544E31u /* "RTN1" */
#define OS_SNAPSHOT_ADDR  0x26000000u
#define OS_SNAPSHOT_MAX   0x00100000u /* src/os/os.ld keeps the OS under 1 MB */
#define OS_LOAD_ADDR      0x22000000u

#define ITEMS_MAX   8
#define PATH_MAX_   200
#define LOW_BATTERY 10 /* below this, off the charger, nothing is swapped */

enum { K_ASSETS, K_OTHER, K_OS, K_FIRM }; /* the order they are swapped in */

typedef struct {
  char name[32];
  int kind;
  char dest[PATH_MAX_];
  char sha256[65];
  uint32_t size;
  char url[256];
  bool needed; /* differs from the card's copy */
  bool ready;  /* downloaded and checked */
  bool moved;  /* the card's copy is in the backup */
  bool placed;
} Item;

static Item items[ITEMS_MAX];
static int item_count;

static char plan_current[48], plan_build[16], plan_from[64];
static char plan_firm[PATH_MAX_];
static char to_name[64];
static char note[160];

/* The screens. */

enum { S_BATTERY, S_NETWORK, S_ASK, S_DOWNLOAD, S_INSTALL, S_START, S_COUNT };
static const char *const step_names[S_COUNT] = {
    "Check the battery", "Join the network",  "Ask the update server",
    "Download the files", "Install the files", "Start the new AuroraOS",
};
static int step;
static char action[96];
static char detail[96];
static uint32_t bytes_total, bytes_done;
static int fetching = -1; /* the item downloading */
static uint32_t accent;

static void say(const char *fmt, ...) AURORA_PRINTF(1, 2);
static void say(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(detail, sizeof(detail), fmt, ap);
  va_end(ap);
}

static void draw_top(void) {
  gfx_gradient(SCREEN_TOP, 0, 0, SCREEN_TOP_WIDTH, SCREEN_HEIGHT,
               RGB(34, 36, 50), RGB(14, 15, 20));
  gfx_print(SCREEN_TOP, 20, 16, FONT_TITLE, COLOR_WHITE, "AuroraOS Update");
  gfx_print(SCREEN_TOP, 20, 44, FONT_REGULAR, COLOR_GRAY, "%s  ->  %s",
            plan_from[0] ? plan_from : "AuroraOS",
            to_name[0] ? to_name : "the newest release");
  for (int i = 0; i < S_COUNT; i++) {
    int y = 80 + i * 22;
    uint32_t c = i < step ? accent : i == step ? COLOR_WHITE : RGB(90, 92, 104);
    if (i < step)
      gfx_print(SCREEN_TOP, 24, y, FONT_BOLD, c, "OK");
    else if (i == step)
      gfx_circle(SCREEN_TOP, 32, y + 7, 4, c);
    else
      gfx_circle_outline(SCREEN_TOP, 32, y + 7, 4, c);
    gfx_print(SCREEN_TOP, 52, y, i == step ? FONT_BOLD : FONT_REGULAR, c, "%s",
              step_names[i]);
  }
}

static const char *item_state(const Item *it, int index) {
  if (it->placed)
    return "installed";
  if (!it->needed)
    return "already up to date";
  if (it->ready)
    return "ready";
  if (index == fetching)
    return "downloading";
  return "waiting";
}

static void draw_bottom(void) {
  const int bx = 20, bw = SCREEN_BOTTOM_WIDTH - 40;
  gfx_gradient(SCREEN_BOTTOM, 0, 0, SCREEN_BOTTOM_WIDTH, SCREEN_HEIGHT,
               RGB(24, 26, 34), RGB(12, 13, 18));
  gfx_print(SCREEN_BOTTOM, bx, 14, FONT_BOLD, COLOR_WHITE, "%s", action);
  gfx_print(SCREEN_BOTTOM, bx, 36, FONT_SMALL, COLOR_GRAY, "%s", detail);
  if (bytes_total) {
    gfx_round_rect(SCREEN_BOTTOM, bx, 60, bw, 10, 5, RGB(50, 52, 64));
    int fill = (int)((uint64_t)bw * bytes_done / bytes_total);
    if (fill > bw)
      fill = bw;
    if (fill >= 10)
      gfx_round_rect(SCREEN_BOTTOM, bx, 60, fill, 10, 5, accent);
  }
  for (int i = 0; i < item_count; i++) {
    const Item *it = &items[i];
    int y = 86 + i * 20;
    gfx_print(SCREEN_BOTTOM, bx, y, FONT_REGULAR, COLOR_WHITE, "%s", it->name);
    const char *s = item_state(it, i);
    int sx = SCREEN_BOTTOM_WIDTH - bx - gfx_text_width(FONT_SMALL, s);
    gfx_print(SCREEN_BOTTOM, sx, y + 2, FONT_SMALL,
              it->placed || it->ready ? accent : COLOR_GRAY, "%s", s);
  }
  bool cancel = step <= S_DOWNLOAD;
  const char *foot = cancel ? "HOME: cancel" : "Keep the console on.";
  int fx = (SCREEN_BOTTOM_WIDTH - gfx_text_width(FONT_SMALL, foot)) / 2;
  gfx_print(SCREEN_BOTTOM, fx, 214, FONT_SMALL,
            cancel ? COLOR_GRAY : COLOR_ORANGE, "%s", foot);
}

static void show(void) {
  draw_top();
  draw_bottom();
  gfx_present();
}

static void at(int s, const char *what) {
  step = s;
  snprintf(action, sizeof(action), "%s", what);
  detail[0] = 0;
  show();
}

static void waiting(uint32_t ms) {
  say("Waiting for the server... %u s", (unsigned)(ms / 1000u));
  show();
}

/* What a run left: read by the OS at its next start. */
static void write_result(bool ok, const char *why) {
  char buf[512];
  int n = snprintf(buf, sizeof(buf), "ok=%d\r\nto=%s\r\n", ok ? 1 : 0, to_name);
  if (why && why[0])
    n += snprintf(buf + n, sizeof(buf) - (size_t)n, "why=%s\r\n", why);
  if (ok && note[0])
    snprintf(buf + n, sizeof(buf) - (size_t)n, "note=%s\r\n", note);
  fs_write_file(UPDATE_RESULT, buf, strlen(buf));
}

/* Shown until A or B, then back to the AuroraOS that started us, which
 * nothing has changed. */
static void stop(bool error, const char *title, const char *why)
    __attribute__((noreturn));
static void stop(bool error, const char *title, const char *why) {
  draw_top();
  gfx_gradient(SCREEN_BOTTOM, 0, 0, SCREEN_BOTTOM_WIDTH, SCREEN_HEIGHT,
               error ? RGB(40, 20, 24) : RGB(24, 26, 34),
               error ? RGB(16, 10, 12) : RGB(12, 13, 18));
  gfx_print(SCREEN_BOTTOM, 20, 20, FONT_BOLD,
            error ? RGB(255, 110, 100) : COLOR_WHITE, "%s", title);
  /* The reason, wrapped to the screen. */
  char line[64];
  int y = 50, n = 0;
  for (const char *s = why; *s && y < 170;) {
    const char *e = s, *cut = 0;
    n = 0;
    while (*e && n < (int)sizeof(line) - 1) {
      line[n] = *e;
      line[n + 1] = 0;
      if (gfx_text_width(FONT_REGULAR, line) > SCREEN_BOTTOM_WIDTH - 40)
        break;
      if (*e == ' ')
        cut = e;
      e++;
      n++;
    }
    if (*e && cut)
      e = cut;
    gfx_print(SCREEN_BOTTOM, 20, y, FONT_REGULAR, COLOR_WHITE, "%.*s",
              (int)(e - s), s);
    y += 20;
    s = *e == ' ' ? e + 1 : e;
  }
  gfx_print(SCREEN_BOTTOM, 20, 180, FONT_SMALL, COLOR_GRAY,
            "Your AuroraOS was not changed.");
  gfx_print(SCREEN_BOTTOM, 20, 210, FONT_SMALL, COLOR_GRAY,
            "A or B: back to AuroraOS");
  gfx_present();
  hid_wait(KEY_A | KEY_B);
  exit(0);
}

static void fail(const char *fmt, ...) AURORA_PRINTF(1, 2)
    __attribute__((noreturn));
static void fail(const char *fmt, ...) {
  char why[160];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(why, sizeof(why), fmt, ap);
  va_end(ap);
  stop(true, "The update could not finish", why);
}

/* Files. */

static bool sha256_file(const char *path, char hex[65], uint32_t *size) {
  static unsigned char buf[16384];
  br_sha256_context c;
  unsigned char d[32];
  size_t n;
  FILE *f = fopen(path, "rb");
  if (!f)
    return false;
  br_sha256_init(&c);
  *size = 0;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
    br_sha256_update(&c, buf, n);
    *size += (uint32_t)n;
  }
  bool ok = !ferror(f);
  fclose(f);
  br_sha256_out(&c, d);
  for (int i = 0; i < 32; i++)
    snprintf(hex + 2 * i, 3, "%02x", d[i]);
  return ok;
}

static bool same_file(const char *path, const Item *it) {
  char hex[65];
  uint32_t size;
  return fs_exists(path) && sha256_file(path, hex, &size) &&
         size == it->size && !strcmp(hex, it->sha256);
}

/* A path this updater may write: on the card, absolute, no "..". */
static bool safe_path(const char *p) {
  size_t n = strlen(p);
  return p[0] == '/' && n > 1 && n < PATH_MAX_ && !strstr(p, "..") &&
         !strchr(p, ':') && !strchr(p, '\\');
}

static bool safe_name(const char *s) {
  if (!s[0])
    return false;
  for (; *s; s++)
    if (!((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z') ||
          (*s >= '0' && *s <= '9') || *s == '.' || *s == '-' || *s == '_'))
      return false;
  return true;
}

static void new_path(const Item *it, char *out, size_t max) {
  snprintf(out, max, "%s/%.31s", UPDATE_NEW_DIR, it->name);
}

/* The names Settings > System Update > Restore looks for. */
static void backup_path(const Item *it, char *out, size_t max) {
  const char *name = it->kind == K_OS       ? "AURORAOS.BIN"
                     : it->kind == K_ASSETS ? "assets.pak"
                     : it->kind == K_FIRM   ? "Aurora.firm"
                                            : it->name;
  snprintf(out, max, "%s/%.31s", UPDATE_BACKUP_DIR, name);
}

/* key=value lines. */
static void value_of(const char *buf, const char *key, char *out, size_t max) {
  size_t k = strlen(key);
  out[0] = 0;
  for (const char *l = buf; *l;) {
    if (!strncmp(l, key, k)) {
      size_t n = 0;
      for (l += k; *l && *l != '\r' && *l != '\n' && n + 1 < max; l++)
        out[n++] = *l;
      out[n] = 0;
      return;
    }
    l = strchr(l, '\n');
    if (!l)
      return;
    l++;
  }
}

static void read_plan(void) {
  size_t size;
  char *plan = fs_read_file(UPDATE_PLAN, &size);
  if (!plan)
    fail("Start the update from Settings > System Update.");
  value_of(plan, "current=", plan_current, sizeof(plan_current));
  value_of(plan, "build=", plan_build, sizeof(plan_build));
  value_of(plan, "from=", plan_from, sizeof(plan_from));
  value_of(plan, "firm=", plan_firm, sizeof(plan_firm));
  free(plan);
  if (plan_firm[0] && (!safe_path(plan_firm) || !fs_exists(plan_firm)))
    plan_firm[0] = 0;
}

/* The server. */

static void ask_server(void) {
  static JsonTok tok[192];
  char url[256], enc[96];
  Json j;
  http_url_encode(enc, sizeof(enc), plan_current);
  snprintf(url, sizeof(url), "%s%s?current=%s&build=%s", UPDATE_SERVER,
           UPDATE_LATEST, enc, plan_build[0] ? plan_build : "0");
  HttpResponse *r = http_get(url);
  if (!r)
    fail("The update server did not answer: %s", net_error());
  if (r->status != 200)
    fail("The update server answered HTTP %d.", r->status);
  if (!json_parse(&j, r->body, (uint32_t)r->length, tok, 192) ||
      tok[0].type != JSON_OBJ)
    fail("The update server's reply could not be read.");
  json_text(&j, json_get(&j, 0, "name"), to_name, sizeof(to_name));
  if (!json_bool(&j, json_get(&j, 0, "update"), 0)) {
    char why[128];
    snprintf(why, sizeof(why), "%s is the newest release.",
             to_name[0] ? to_name : plan_from);
    stop(false, "AuroraOS is up to date", why);
  }
  int files = json_get(&j, 0, "files");
  if (files < 0 || tok[files].type != JSON_ARR)
    fail("The update server's reply has no files.");
  item_count = 0;
  bool has_os = false;
  uint32_t count = json_count(&j, files);
  for (uint32_t i = 0; i < count && item_count < ITEMS_MAX; i++) {
    int f = json_at(&j, files, i);
    Item *it = &items[item_count];
    char kind[16], path[PATH_MAX_];
    memset(it, 0, sizeof(*it));
    json_str(&j, json_get(&j, f, "name"), it->name, sizeof(it->name));
    json_str(&j, json_get(&j, f, "kind"), kind, sizeof(kind));
    json_str(&j, json_get(&j, f, "path"), path, sizeof(path));
    json_str(&j, json_get(&j, f, "sha256"), it->sha256, sizeof(it->sha256));
    json_str(&j, json_get(&j, f, "url"), it->url, sizeof(it->url));
    int size = json_int(&j, json_get(&j, f, "size"), 0);
    it->size = size > 0 ? (uint32_t)size : 0;
    it->kind = !strcmp(kind, "os")       ? K_OS
               : !strcmp(kind, "assets") ? K_ASSETS
               : !strcmp(kind, "firm")   ? K_FIRM
                                         : K_OTHER;
    if (!safe_name(it->name) || strlen(it->sha256) != 64 || !it->size ||
        strncmp(it->url, "https://", 8))
      fail("The update server sent a file entry Aurora cannot use.");
    if (it->kind == K_FIRM) {
      if (!plan_firm[0]) {
        snprintf(note, sizeof(note),
                 "The firm was not updated: AuroraOS could not tell where "
                 "it is.");
        continue;
      }
      snprintf(it->dest, sizeof(it->dest), "%s", plan_firm);
    } else {
      if (!safe_path(path))
        fail("The update server sent a place Aurora will not write: %s", path);
      snprintf(it->dest, sizeof(it->dest), "%s", path);
    }
    has_os |= it->kind == K_OS;
    item_count++;
  }
  http_free(r);
  if (!has_os)
    fail("The update server's release has no AURORAOS.BIN.");
  /* Swap order: the OS after what it reads, the firm (which starts it) last. */
  for (int a = 1; a < item_count; a++)
    for (int b = a; b > 0 && items[b - 1].kind > items[b].kind; b--) {
      Item t = items[b - 1];
      items[b - 1] = items[b];
      items[b] = t;
    }
}

static void download_progress(size_t done, size_t total) {
  (void)total;
  uint32_t before = 0;
  for (int i = 0; i < fetching; i++)
    if (items[i].needed)
      before += items[i].size;
  bytes_done = before + (uint32_t)done;
  say("%s: %u of %u KB", items[fetching].name, (unsigned)(done / 1024u),
      (unsigned)(items[fetching].size / 1024u));
  show();
}

static void download_all(void) {
  char path[PATH_MAX_];
  bytes_total = bytes_done = 0;
  for (int i = 0; i < item_count; i++) {
    items[i].needed = !same_file(items[i].dest, &items[i]);
    if (items[i].needed)
      bytes_total += items[i].size;
  }
  if (!bytes_total)
    stop(false, "Nothing to download",
         "Every file on the card is already the newest one.");
  fs_mkdir(UPDATE_NEW_DIR);
  for (int i = 0; i < item_count; i++) {
    Item *it = &items[i];
    if (!it->needed)
      continue;
    new_path(it, path, sizeof(path));
    fetching = i;
    for (int attempt = 0; attempt < 2 && !it->ready; attempt++) {
      remove(path);
      say("%s", it->name);
      show();
      if (!http_download(it->url, path, download_progress)) {
        if (attempt)
          fail("Downloading %s failed: %s", it->name, net_error());
        continue;
      }
      it->ready = same_file(path, it);
    }
    if (!it->ready) {
      remove(path);
      fail("%s did not download correctly. Try again later.", it->name);
    }
  }
  fetching = -1;
  bytes_done = bytes_total;
}

/* Swapping. */

static void clear_backup(void) {
  char path[PATH_MAX_ + FS_NAME_MAX];
  FsEntry e;
  fs_mkdir(UPDATE_BACKUP_DIR);
  FsDir *d = fs_dir_open(UPDATE_BACKUP_DIR);
  if (!d)
    return;
  while (fs_dir_read(d, &e))
    if (!e.is_dir) {
      snprintf(path, sizeof(path), "%s/%s", UPDATE_BACKUP_DIR, e.name);
      remove(path);
    }
  fs_dir_close(d);
}

/* The card's copy into the backup, the new one into its place. */
static bool place(Item *it) {
  char bak[PATH_MAX_], nw[PATH_MAX_];
  backup_path(it, bak, sizeof(bak));
  new_path(it, nw, sizeof(nw));
  if (fs_exists(it->dest)) {
    remove(bak);
    if (rename(it->dest, bak))
      return false;
    it->moved = true;
  }
  if (rename(nw, it->dest)) {
    if (it->moved && !rename(bak, it->dest))
      it->moved = false;
    return false;
  }
  it->placed = true;
  return true;
}

/* place() undone: 1 when the card's copy is back. */
static bool unplace(Item *it) {
  char bak[PATH_MAX_], nw[PATH_MAX_];
  backup_path(it, bak, sizeof(bak));
  new_path(it, nw, sizeof(nw));
  if (it->placed) {
    if (rename(it->dest, nw))
      return false;
    it->placed = false;
  }
  if (it->moved) {
    if (rename(bak, it->dest))
      return false;
    it->moved = false;
  }
  return true;
}

static void install_all(void) {
  char info[PATH_MAX_ + 96];
  int batt = sys_battery();
  if (!sys_charging() && batt >= 0 && batt < LOW_BATTERY)
    fail("The battery ran low (%d%%). Plug in the charger and try again.",
         batt);
  /* No presents from here to the end: nothing may leave the app halfway. */
  clear_backup();
  for (int i = 0; i < item_count; i++) {
    if (!items[i].needed)
      continue;
    if (!place(&items[i])) {
      bool back = true;
      for (int k = i; k >= 0; k--)
        back &= unplace(&items[k]);
      write_result(false, back ? "A file could not be moved on the SD card. "
                                 "Nothing was changed."
                               : "A file could not be moved on the SD card, "
                                 "and some could not be put back.");
      fail(back ? "A file could not be moved on the SD card."
                : "A file could not be moved on the SD card, and some could "
                  "not be put back. See /Aurora/Update/backup.");
    }
  }
  snprintf(info, sizeof(info), "name=%s\r\nfirm=%s\r\n", plan_from, plan_firm);
  fs_write_file(UPDATE_BACKUP_INFO, info, strlen(info));
  rmdir(UPDATE_NEW_DIR);
  remove(UPDATE_PLAN);
  write_result(true, 0);
}

/* The new AuroraOS where the return stub restores the OS from, so leaving
 * the app starts it. False when it cannot be done that way. */
static bool stage_new_os(void) {
  uint32_t h[9];
  volatile uint32_t *desc = (volatile uint32_t *)RETURN_DESC_ADDR;
  if (!app_from_home() || desc[0] != RETURN_MAGIC)
    return false;
  FILE *f = fopen(UPDATE_OS_PATH, "rb");
  if (!f)
    return false;
  bool ok = fread(h, 1, sizeof(h), f) == sizeof(h) && !memcmp(h, "AOS1", 4) &&
            h[3] == OS_LOAD_ADDR && h[4] == OS_LOAD_ADDR && !h[6] && h[2] &&
            h[2] <= OS_SNAPSHOT_MAX && !fseek(f, (long)h[1], SEEK_SET) &&
            fread((void *)OS_SNAPSHOT_ADDR, 1, h[2], f) == h[2];
  fclose(f);
  if (ok)
    desc[1] = h[2];
  return ok;
}

int main(void) {
  accent = sys_accent();
  read_plan();

  at(S_BATTERY, "Checking the battery");
  int batt = sys_battery();
  if (!sys_charging() && batt >= 0 && batt < UPDATE_MIN_BATTERY)
    fail("The battery is at %d%%. Charge it past %d%% or plug in the charger.",
         batt, UPDATE_MIN_BATTERY);

  at(S_NETWORK, "Joining the network");
  net_on_wait(waiting);
  if (!net_connect())
    fail("%s", net_error());

  at(S_ASK, "Asking the update server");
  ask_server();

  at(S_DOWNLOAD, "Downloading");
  download_all();

  at(S_INSTALL, "Installing");
  install_all();

  at(S_START, "Starting the new AuroraOS");
  if (stage_new_os())
    exit(0); /* the return stub starts it */

  /* Installed, but not startable from here: a power cycle starts it. */
  snprintf(action, sizeof(action), "%s is installed", to_name);
  say("Turn the console off and on to start it.");
  show();
  gfx_print(SCREEN_BOTTOM, 20, 190, FONT_SMALL, COLOR_GRAY, "A: power off");
  gfx_present();
  hid_wait(KEY_A);
  sys_power_off();
}
