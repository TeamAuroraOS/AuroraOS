/* The network built-ins, over src/os/AppNet.c (GPL-2.0, part of the Wi-Fi
 * driver). The last reply stays in the core's buffer, where http_text() and
 * the JSON lookups read it in place; the strings those make are copied into a
 * pool emptied by the next request, so a string from a reply lasts until
 * then. */
#include "aurora.h"
#include "appnet.h"
#include "ff.h"
#include "json.h"
#include "timer.h"
#include "auric_runtime.h"
#include "auric_internal.h"

#define AUR_STRS        (64u * 1024u)
#define AUR_JSON_TOKENS 16384u
#define AUR_PARAMS      1024u
#define AUR_HEADERS     1024u

/* WiFiJoin.c seeds the join's nonce from the timer. */
uint32_t timer_ticks(void) { return aur_ticks(); }

u32 appnet_ms(void) { return aur_ticks() / AUR_TICKS_PER_MS; }

int appnet_mount(void) { return aur_mount(); }

static int aur_home_later; /* HOME pressed while the core was busy */

void appnet_waiting(u32 ms) {
  (void)ms;
  if (!aur_home_later && aur_from_home() && aur_home_pressed())
    aur_home_later = 1;
}

/* After a network call: a HOME press during it leaves now. */
static void aur_net_after(void) {
  if (aur_home_later)
    aur_leave();
}

static u32 aur_len(const char *s) {
  u32 n = 0;
  while (s[n])
    n++;
  return n;
}

static int aur_lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static char aur_strs[AUR_STRS];
static u32 aur_strs_used;

static char *aur_str_alloc(u32 n) {
  char *s;
  if (n > AUR_STRS - aur_strs_used)
    return 0;
  s = aur_strs + aur_strs_used;
  aur_strs_used += n;
  return s;
}

static const char *aur_str_copy(const char *s, u32 n) {
  char *d = aur_str_alloc(n + 1u);
  if (!d)
    return "";
  for (u32 i = 0; i < n; i++)
    d[i] = s[i];
  d[n] = 0;
  return d;
}

/* The request being put together, and the last reply. */
static char aur_params[AUR_PARAMS], aur_headers[AUR_HEADERS];
static u32 aur_params_len, aur_headers_len;
static int aur_req_over;
static const char *aur_err; /* the runtime's own reason, over AppNet's */

static AppNetReply aur_reply;
static int aur_json_state; /* 0 not parsed yet, 1 JSON, -1 not */
static Json aur_json;
static JsonTok aur_tok[AUR_JSON_TOKENS];
static int aur_json_idx;

static void aur_add(char *buf, u32 cap, u32 *len, const char *s, u32 n) {
  if (*len + n >= cap) {
    aur_req_over = 1;
    return;
  }
  for (u32 i = 0; i < n; i++)
    buf[*len + i] = s[i];
  *len += n;
  buf[*len] = 0;
}

static void aur_add_encoded(const char *s) {
  for (; *s; s++) {
    char c[2] = {*s, 0}, t[4];
    aur_add(aur_params, AUR_PARAMS, &aur_params_len, t,
            appnet_url_encode(t, sizeof(t), c));
  }
}

void aur_http_param(const char *name, const char *value) {
  aur_check_home();
  if (aur_params_len)
    aur_add(aur_params, AUR_PARAMS, &aur_params_len, "&", 1);
  aur_add_encoded(name);
  aur_add(aur_params, AUR_PARAMS, &aur_params_len, "=", 1);
  aur_add_encoded(value);
}

void aur_http_param_int(const char *name, int value) {
  char t[12];
  int i = sizeof(t) - 1;
  unsigned mag = value < 0 ? 0u - (unsigned)value : (unsigned)value;
  t[i] = 0;
  do {
    t[--i] = (char)('0' + mag % 10u);
    mag /= 10u;
  } while (mag);
  if (value < 0)
    t[--i] = '-';
  aur_http_param(name, t + i);
}

void aur_http_header(const char *name, const char *value) {
  aur_check_home();
  aur_add(aur_headers, AUR_HEADERS, &aur_headers_len, name, aur_len(name));
  aur_add(aur_headers, AUR_HEADERS, &aur_headers_len, ": ", 2);
  aur_add(aur_headers, AUR_HEADERS, &aur_headers_len, value, aur_len(value));
  aur_add(aur_headers, AUR_HEADERS, &aur_headers_len, "\r\n", 2);
}

static int aur_has_header(const char *name) {
  for (const char *l = aur_headers; *l;) {
    u32 k = 0;
    while (name[k] && aur_lower(l[k]) == name[k])
      k++;
    if (!name[k] && l[k] == ':')
      return 1;
    while (*l && *l != '\n')
      l++;
    if (*l)
      l++;
  }
  return 0;
}

static void aur_reply_reset(void) {
  aur_reply.status = 0;
  aur_reply.head = aur_reply.body = (char *)"";
  aur_reply.len = aur_reply.from = aur_reply.total = 0;
  aur_json_state = 0;
  aur_strs_used = 0;
  aur_err = 0;
}

static void aur_request_reset(void) {
  aur_params[0] = aur_headers[0] = 0;
  aur_params_len = aur_headers_len = 0;
  aur_req_over = 0;
}

/* The URL with the parameters as its query, unless they are the body. */
static const char *aur_url(const char *url, int params) {
  static char full[APPNET_URL_MAX + AUR_PARAMS + 2];
  u32 n = 0, q = 0;
  for (; url[n] && n < APPNET_URL_MAX; n++) {
    full[n] = url[n];
    q |= url[n] == '?';
  }
  if (url[n]) {
    aur_req_over = 1;
    return url;
  }
  if (params && aur_params_len) {
    full[n++] = q ? '&' : '?';
    for (u32 i = 0; i < aur_params_len; i++)
      full[n++] = aur_params[i];
  }
  full[n] = 0;
  return full;
}

static int aur_request(const char *method, const char *url, const char *body) {
  static char heads[AUR_HEADERS + 64];
  u32 blen = body ? aur_len(body) : 0, n = 0;
  int post = body != 0, form = post && !blen && aur_params_len;
  const char *full;
  aur_check_home();
  aur_reply_reset();
  full = aur_url(url, !form);
  if (aur_req_over) {
    aur_err = "the address, parameters or headers are too long";
    aur_request_reset();
    return 0;
  }
  if (form) {
    body = aur_params;
    blen = aur_params_len;
  }
  for (; n < aur_headers_len; n++)
    heads[n] = aur_headers[n];
  if (post && !aur_has_header("content-type")) {
    const char *t = !form && (body[0] == '{' || body[0] == '[')
                        ? "Content-Type: application/json\r\n"
                        : "Content-Type: application/x-www-form-urlencoded\r\n";
    while (*t)
      heads[n++] = *t++;
  }
  heads[n] = 0;
  appnet_http(method, full, heads, body, blen, &aur_reply);
  aur_request_reset();
  aur_net_after();
  return aur_reply.status;
}

int aur_http_get(const char *url) { return aur_request("GET", url, 0); }

int aur_http_post(const char *url, const char *body) {
  return aur_request("POST", url, body ? body : "");
}

int aur_net_connect(void) {
  int ok;
  aur_check_home();
  aur_err = 0;
  ok = appnet_join();
  aur_net_after();
  return ok;
}

int aur_net_online(void) {
  int ok;
  aur_check_home();
  aur_err = 0;
  ok = appnet_online();
  aur_net_after();
  return ok;
}

const char *aur_net_error(void) { return aur_err ? aur_err : appnet_error(); }

const char *aur_net_address(void) {
  static char text[16];
  u32 ip;
  aur_check_home();
  ip = appnet_ip();
  text[0] = 0;
  if (ip)
    appnet_ip_text(ip, text);
  return text;
}

const char *aur_http_text(void) {
  aur_check_home();
  return aur_reply.body;
}

int aur_http_length(void) {
  aur_check_home();
  return (int)aur_reply.len;
}

/* Lines end with "\n" (a "\r" before it is dropped); a last one without it
 * counts too. */
int aur_http_lines(void) {
  const char *b = aur_reply.body;
  u32 n = aur_reply.len, c = 0;
  aur_check_home();
  if (!n)
    return 0;
  for (u32 i = 0; i < n; i++)
    c += b[i] == '\n';
  return (int)(c + (b[n - 1] != '\n'));
}

const char *aur_http_line(int line) {
  const char *b = aur_reply.body;
  u32 n = aur_reply.len, i = 0, e;
  aur_check_home();
  if (line < 0)
    return "";
  for (; line > 0 && i < n; i++)
    if (b[i] == '\n')
      line--;
  if (line > 0 || i >= n)
    return "";
  for (e = i; e < n && b[e] != '\n'; e++)
    ;
  if (e > i && b[e - 1] == '\r')
    e--;
  return aur_str_copy(b + i, e - i);
}

int aur_http_save(const char *path) {
  static FIL f;
  UINT bw = 0;
  int ok;
  aur_check_home();
  if (!aur_mount() || f_open(&f, path, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) {
    aur_err = "the file could not be created";
    return 0;
  }
  ok = f_write(&f, aur_reply.body, aur_reply.len, &bw) == FR_OK &&
       bw == aur_reply.len;
  ok = f_close(&f) == FR_OK && ok;
  if (!ok)
    aur_err = "the file could not be written";
  return ok;
}

int aur_http_download(const char *url, const char *path) {
  const char *full;
  int ok;
  aur_check_home();
  aur_reply_reset();
  full = aur_url(url, 1);
  if (aur_req_over) {
    aur_err = "the address, parameters or headers are too long";
    aur_request_reset();
    return 0;
  }
  ok = appnet_download(full, aur_headers_len ? aur_headers : 0, path, 0);
  aur_request_reset();
  aur_net_after();
  return ok;
}

static int aur_same(const char *s, u32 n, const char *word) {
  u32 k = 0;
  while (k < n && word[k] && s[k] == word[k])
    k++;
  return k == n && !word[k];
}

/* json_parse() reads one value from the start; the reply is JSON only if
 * that value is well formed and nothing but spaces follows it. */
static int aur_json_whole(void) {
  const JsonTok *r = &aur_tok[0];
  const char *s = aur_reply.body + r->start;
  u32 n = r->end - r->start, e = r->end + (r->type == JSON_STR);
  if (r->type == JSON_NUM && !(s[0] == '-' || (s[0] >= '0' && s[0] <= '9')))
    return 0;
  if ((r->type == JSON_TRUE && !aur_same(s, n, "true")) ||
      (r->type == JSON_FALSE && !aur_same(s, n, "false")) ||
      (r->type == JSON_NULL && !aur_same(s, n, "null")))
    return 0;
  for (; e < aur_reply.len; e++) {
    char c = aur_reply.body[e];
    if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
      return 0;
  }
  return 1;
}

static int aur_json_ready(void) {
  if (!aur_json_state)
    aur_json_state = aur_reply.len &&
                             json_parse(&aur_json, aur_reply.body,
                                        aur_reply.len, aur_tok,
                                        AUR_JSON_TOKENS) &&
                             aur_json_whole()
                         ? 1
                         : -1;
  return aur_json_state > 0;
}

/* Each part of the path names a member of an object or, as a number, an
 * element of an array; "#" is the number json_index() gave. */
static int aur_json_find(const char *path) {
  char seg[128];
  int t = 0;
  if (!aur_json_ready())
    return -1;
  while (*path) {
    u32 n = 0, idx = 0;
    int num = 1;
    while (*path && *path != '.') {
      if (n + 1u < sizeof(seg))
        seg[n++] = *path;
      path++;
    }
    seg[n] = 0;
    if (*path == '.')
      path++;
    if (n == 1 && seg[0] == '#') {
      u32 v = (u32)aur_json_idx;
      char t2[12];
      int k = 0;
      if (aur_json_idx < 0)
        return -1;
      do {
        t2[k++] = (char)('0' + v % 10u);
        v /= 10u;
      } while (v);
      for (n = 0; k;)
        seg[n++] = t2[--k];
      seg[n] = 0;
    }
    for (u32 i = 0; i < n; i++) {
      if (seg[i] < '0' || seg[i] > '9' || idx > 100000000u)
        num = 0;
      idx = idx * 10u + (u32)(seg[i] - '0');
    }
    if (aur_tok[t].type == JSON_ARR)
      t = num && n ? json_at(&aur_json, t, idx) : -1;
    else if (aur_tok[t].type == JSON_OBJ)
      t = json_get(&aur_json, t, seg);
    else
      t = -1;
    if (t < 0)
      return -1;
  }
  return t;
}

/* A number, or a string holding one; true and false are 1 and 0. */
static double aur_json_number(int t) {
  JsonTok one;
  Json j;
  if (t < 0)
    return 0.0;
  if (aur_tok[t].type == JSON_TRUE)
    return 1.0;
  if (aur_tok[t].type != JSON_STR)
    return json_num(&aur_json, t, 0.0);
  one = aur_tok[t];
  one.type = JSON_NUM;
  j.src = aur_json.src;
  j.tok = &one;
  j.n = j.cap = 1;
  return json_num(&j, 0, 0.0);
}

int aur_json_has(const char *path) {
  aur_check_home();
  return aur_json_find(path) >= 0;
}

int aur_json_int(const char *path) {
  double v;
  aur_check_home();
  v = aur_json_number(aur_json_find(path));
  if (v >= 2147483647.0)
    return 2147483647;
  if (v <= -2147483647.0)
    return -2147483647;
  return (int)(v < 0 ? v - 0.5 : v + 0.5);
}

float aur_json_float(const char *path) {
  aur_check_home();
  return (float)aur_json_number(aur_json_find(path));
}

int aur_json_bool(const char *path) {
  int t;
  aur_check_home();
  t = aur_json_find(path);
  if (t >= 0 && aur_tok[t].type == JSON_NUM)
    return aur_json_number(t) != 0.0;
  return t >= 0 && json_bool(&aur_json, t, 0);
}

/* A string's text (escapes resolved, anything outside ASCII as '?'); for any
 * other value its JSON as written. */
const char *aur_json_string(const char *path) {
  const JsonTok *k;
  int t;
  aur_check_home();
  t = aur_json_find(path);
  if (t < 0)
    return "";
  k = &aur_tok[t];
  if (k->type == JSON_STR) {
    u32 n = k->end - k->start + 1u;
    char *d = aur_str_alloc(n);
    if (!d)
      return "";
    json_str(&aur_json, t, d, n);
    aur_strs_used -= n - (aur_len(d) + 1u); /* escapes made it shorter */
    return d;
  }
  return aur_str_copy(aur_json.src + k->start, k->end - k->start);
}

int aur_json_count(const char *path) {
  int t;
  aur_check_home();
  t = aur_json_find(path);
  return t >= 0 ? (int)json_count(&aur_json, t) : 0;
}

void aur_json_index(int n) {
  aur_check_home();
  aur_json_idx = n;
}
