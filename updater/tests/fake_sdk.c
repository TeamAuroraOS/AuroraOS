/* The C SDK as updater/main.c uses it, for the host test (run.py): the screens
 * and buttons do nothing but log, the SD card is the folder UT_ROOT, and the
 * network is an update server made of the folder UT_SERVER (manifest.json and
 * files/<sha256>). Environment knobs:
 *   UT_BATTERY, UT_CHARGING  the battery
 *   UT_STATUS                the HTTP status of the manifest (200)
 *   UT_CORRUPT               a sha256 whose download arrives damaged
 *   UT_FAIL_RENAME           a card path whose renames (as the target) fail
 *   UT_FAIL_RENAME_ONCE      the same, the first time only
 *   UT_LOG                   where gfx_print's text goes */
#include <aurora_app.h>

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#undef fopen
#undef rename
#undef remove
#undef rmdir

static const char *env(const char *name, const char *def) {
  const char *v = getenv(name);
  return v && v[0] ? v : def;
}

/* "/Aurora/x" -> "<UT_ROOT>/Aurora/x". */
static const char *host_path(const char *p) {
  static char bufs[4][1024];
  static int n;
  char *b = bufs[n++ % 4];
  snprintf(b, sizeof(bufs[0]), "%s%s%s", env("UT_ROOT", "."),
           p[0] == '/' ? "" : "/", p);
  return b;
}

static void log_line(const char *s) {
  FILE *f = fopen(env("UT_LOG", "ut.log"), "a");
  if (f) {
    fprintf(f, "%s\n", s);
    fclose(f);
  }
}

FILE *host_fopen(const char *path, const char *mode) {
  return fopen(host_path(path), mode);
}

int host_rename(const char *from, const char *to) {
  static int refused_once;
  const char *fail = getenv("UT_FAIL_RENAME");
  const char *once = getenv("UT_FAIL_RENAME_ONCE");
  if ((fail && !strcmp(fail, to)) ||
      (once && !strcmp(once, to) && !refused_once++)) {
    log_line("[rename refused]");
    return -1;
  }
  char a[1024];
  snprintf(a, sizeof(a), "%s", host_path(from));
  /* As app_fs_rename: a file in the way is replaced. */
  return rename(a, host_path(to));
}

int host_remove(const char *path) { return remove(host_path(path)); }

int host_rmdir(const char *path) { return rmdir(host_path(path)); }

/* The screens and buttons. */

int gfx_print(Screen s, int x, int y, GfxFont font, uint32_t color,
              const char *fmt, ...) {
  char b[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(b, sizeof(b), fmt, ap);
  va_end(ap);
  (void)s, (void)x, (void)y, (void)font, (void)color;
  log_line(b);
  return (int)strlen(b) * 7;
}

int gfx_text_width(GfxFont font, const char *text) {
  (void)font;
  return (int)strlen(text) * 7;
}

void gfx_gradient(Screen s, int x, int y, int w, int h, uint32_t top,
                  uint32_t bottom) {}
void gfx_round_rect(Screen s, int x, int y, int w, int h, int r,
                    uint32_t color) {}
void gfx_circle(Screen s, int cx, int cy, int r, uint32_t color) {}
void gfx_circle_outline(Screen s, int cx, int cy, int r, uint32_t color) {}
void gfx_present(void) {}

uint32_t hid_wait(uint32_t keys) { return keys & KEY_A ? KEY_A : keys; }

bool app_from_home(void) { return true; }
uint32_t sys_accent(void) { return 0x64E8C8u; }
int sys_battery(void) { return atoi(env("UT_BATTERY", "80")); }
bool sys_charging(void) { return atoi(env("UT_CHARGING", "0")) != 0; }

void sys_power_off(void) {
  log_line("[power off]");
  exit(4);
}

/* The card. */

bool fs_exists(const char *path) {
  struct stat st;
  return !stat(host_path(path), &st);
}

bool fs_mkdir(const char *path) {
  char b[1024];
  snprintf(b, sizeof(b), "%s", host_path(path));
  for (char *p = b + strlen(env("UT_ROOT", ".")) + 1; *p; p++)
    if (*p == '/') {
      *p = 0;
      mkdir(b, 0777);
      *p = '/';
    }
  mkdir(b, 0777);
  struct stat st;
  return !stat(b, &st) && S_ISDIR(st.st_mode);
}

void *fs_read_file(const char *path, size_t *size) {
  FILE *f = fopen(host_path(path), "rb");
  if (!f)
    return NULL;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *b = malloc((size_t)n + 1);
  if (fread(b, 1, (size_t)n, f) != (size_t)n) {
    fclose(f);
    free(b);
    return NULL;
  }
  fclose(f);
  b[n] = 0;
  *size = (size_t)n;
  return b;
}

bool fs_write_file(const char *path, const void *data, size_t size) {
  FILE *f = fopen(host_path(path), "wb");
  if (!f)
    return false;
  bool ok = fwrite(data, 1, size, f) == size;
  return fclose(f) == 0 && ok;
}

struct FsDir {
  DIR *d;
  char path[1024];
};

FsDir *fs_dir_open(const char *path) {
  FsDir *d = calloc(1, sizeof(*d));
  snprintf(d->path, sizeof(d->path), "%s", host_path(path));
  d->d = opendir(d->path);
  if (!d->d) {
    free(d);
    return NULL;
  }
  return d;
}

bool fs_dir_read(FsDir *d, FsEntry *e) {
  struct dirent *de;
  while ((de = readdir(d->d))) {
    if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
      continue;
    char full[2048];
    struct stat st;
    snprintf(full, sizeof(full), "%s/%s", d->path, de->d_name);
    stat(full, &st);
    snprintf(e->name, sizeof(e->name), "%s", de->d_name);
    e->is_dir = S_ISDIR(st.st_mode);
    e->size = e->is_dir ? 0 : (uint32_t)st.st_size;
    return true;
  }
  return false;
}

void fs_dir_close(FsDir *d) {
  closedir(d->d);
  free(d);
}

/* The network. */

static char g_net_error[128] = "";

void net_on_wait(void (*fn)(uint32_t ms)) {
  if (fn)
    fn(1500);
}

bool net_connect(void) { return true; }

const char *net_error(void) { return g_net_error; }

size_t http_url_encode(char *out, size_t max, const char *text) {
  size_t n = 0;
  for (; *text && n + 4 < max; text++) {
    unsigned char c = (unsigned char)*text;
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~')
      out[n++] = (char)c;
    else
      n += (size_t)snprintf(out + n, max - n, "%%%02X", c);
  }
  out[n] = 0;
  return n;
}

HttpResponse *http_get(const char *url) {
  char path[1024], line[1100];
  snprintf(line, sizeof(line), "[GET %s]", url);
  log_line(line);
  snprintf(path, sizeof(path), "%s/manifest.json", env("UT_SERVER", "."));
  FILE *f = fopen(path, "rb");
  if (!f) {
    snprintf(g_net_error, sizeof(g_net_error), "no route to the server");
    return NULL;
  }
  HttpResponse *r = calloc(1, sizeof(*r));
  fseek(f, 0, SEEK_END);
  r->length = (size_t)ftell(f);
  fseek(f, 0, SEEK_SET);
  r->body = malloc(r->length + 1);
  r->length = fread(r->body, 1, r->length, f);
  r->body[r->length] = 0;
  fclose(f);
  r->status = atoi(env("UT_STATUS", "200"));
  r->headers = calloc(1, 1);
  return r;
}

void http_free(HttpResponse *r) {
  if (!r)
    return;
  free(r->body);
  free(r->headers);
  free(r);
}

bool http_download(const char *url, const char *path,
                   void (*progress)(size_t done, size_t total)) {
  const char *sha = strstr(url, "/v1/files/");
  char src[1024], part[1100], line[1200];
  snprintf(line, sizeof(line), "[download %s -> %s]", url, path);
  log_line(line);
  if (!sha) {
    snprintf(g_net_error, sizeof(g_net_error), "HTTP 404");
    return false;
  }
  sha += 10;
  snprintf(src, sizeof(src), "%s/files/%s", env("UT_SERVER", "."), sha);
  FILE *in = fopen(src, "rb");
  if (!in) {
    snprintf(g_net_error, sizeof(g_net_error), "HTTP 404");
    return false;
  }
  snprintf(part, sizeof(part), "%s.part", host_path(path));
  FILE *out = fopen(part, "wb");
  if (!out) {
    fclose(in);
    snprintf(g_net_error, sizeof(g_net_error), "could not write %s", path);
    return false;
  }
  char buf[8192];
  size_t n, done = 0;
  const char *corrupt = getenv("UT_CORRUPT");
  while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
    if (corrupt && !strcmp(corrupt, sha) && done == 0)
      buf[n / 2] ^= 0x5A;
    fwrite(buf, 1, n, out);
    done += n;
  }
  fclose(in);
  fclose(out);
  if (progress)
    progress(done, done);
  remove(host_path(path));
  return rename(part, host_path(path)) == 0;
}
