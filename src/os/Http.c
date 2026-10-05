/* HTTP/1.0 to the account server's console host, as its API.md asks. See
 * include/http.h. */
#include "http.h"
#include "account.h"
#include "lang.h"
#include "wifi.h"

#define HTTP_TIMEOUT_MS 15000u

static char req[WIFI_HTTP_REQ_MAX];
static u32 host_ip; /* ACCOUNT_HOST, once looked up */

static char *put(char *d, const char *s) {
  while ((*d = *s)) {
    d++;
    s++;
  }
  return d;
}

static char *put_num(char *d, u32 v) {
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

static u32 len_of(const char *s) {
  u32 n = 0;
  while (s[n])
    n++;
  return n;
}

/* "Beta v0.1.3" -> "0.1.3", for the User-Agent. */
static char *put_version(char *p) {
  const char *v = AURORA_VERSION;
  while (*v && !(*v >= '0' && *v <= '9'))
    v++;
  while ((*v >= '0' && *v <= '9') || *v == '.')
    *p++ = *v++;
  *p = 0;
  return p;
}

/* Host even with 1.0, a User-Agent, Connection: close, and a form body with
 * its length. Returns the length; 0 when it does not fit. */
static u32 build(const char *method, const char *path, const char *token,
                 const char *extra, const char *form) {
  u32 need = len_of(method) + len_of(path) + 200u +
             (token ? len_of(token) + 26u : 0) + (extra ? len_of(extra) : 0) +
             (form ? len_of(form) + 72u : 0);
  if (need > sizeof(req))
    return 0;
  char *p = put(req, method);
  p = put(p, " ");
  p = put(p, path);
  p = put(p, " HTTP/1.0\r\nHost: " ACCOUNT_HOST "\r\nUser-Agent: AuroraOS/");
  p = put_version(p);
  p = put(p, "\r\nConnection: close\r\n");
  if (token) {
    p = put(p, "Authorization: Bearer ");
    p = put(p, token);
    p = put(p, "\r\n");
  }
  if (extra)
    p = put(p, extra);
  if (form) {
    p = put(p, "Content-Type: application/x-www-form-urlencoded\r\n"
               "Content-Length: ");
    p = put_num(p, len_of(form));
    p = put(p, "\r\n");
  }
  p = put(p, "\r\n");
  if (form)
    p = put(p, form);
  return (u32)(p - req);
}

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

/* The value of header `name` (lower case, with its colon) in b[0..end), or
 * 0. Header names are case-insensitive; Cloudflare adds many, all skipped. */
static const char *header(const char *b, u32 end, const char *name) {
  for (u32 i = 1; i < end; i++) {
    if (b[i - 1] != '\n')
      continue;
    u32 k = 0;
    while (name[k] && i + k < end && lower(b[i + k]) == name[k])
      k++;
    if (name[k])
      continue;
    const char *v = b + i + k;
    while (*v == ' ' || *v == '\t')
      v++;
    return v;
  }
  return 0;
}

static const char *read_num(const char *s, u32 *v) {
  *v = 0;
  while (*s >= '0' && *s <= '9' && *v < 0x10000000u)
    *v = *v * 10u + (u32)(*s++ - '0');
  return s;
}

/* The status line, the headers aShop and the account code read, and where
 * the body is. */
static void parse(char *b, u32 n, HttpReply *r) {
  u32 end = 0, i = 5;
  b[n] = 0;
  if (n < 12u || b[0] != 'H' || b[1] != 'T' || b[2] != 'T' || b[3] != 'P' ||
      b[4] != '/')
    return;
  while (i < n && b[i] != ' ')
    i++;
  if (i + 4u > n || b[i + 1] < '1' || b[i + 1] > '5' || b[i + 2] < '0' ||
      b[i + 2] > '9' || b[i + 3] < '0' || b[i + 3] > '9')
    return;
  r->status = (b[i + 1] - '0') * 100 + (b[i + 2] - '0') * 10 + (b[i + 3] - '0');
  for (i = 0; i + 3u < n; i++)
    if (b[i] == '\r' && b[i + 1] == '\n' && b[i + 2] == '\r' &&
        b[i + 3] == '\n') {
      end = i + 4u;
      break;
    }
  if (!end)
    return;
  r->body = b + end;
  r->len = n - end;
  const char *v = header(b, end, "content-length:");
  if (v) {
    u32 cl;
    read_num(v, &cl);
    if (cl < r->len)
      r->len = cl;
  }
  /* "bytes 6144-12287/28616" */
  v = header(b, end, "content-range:");
  if (v && lower(v[0]) == 'b' && lower(v[4]) == 's' && v[5] == ' ') {
    u32 last;
    v = read_num(v + 6, &r->from);
    if (*v == '-') {
      v = read_num(v + 1, &last);
      if (*v == '/' && last >= r->from)
        read_num(v + 1, &r->total);
    }
  }
  v = header(b, end, "etag:");
  if (v) {
    u32 k = 0;
    while (v[k] && v[k] != '\r' && v[k] != '\n' && k + 1 < sizeof(r->etag)) {
      r->etag[k] = v[k];
      k++;
    }
    r->etag[k] = 0;
  }
  r->body[r->len] = 0;
}

int http_online(void) {
  WifiNetResult nr;
  if (wifi_online())
    return 1;
  /* The core may still hold a join the OS forgot, after an app. */
  if (wifi_net(WIFI_NETOP_STATUS, 0, 0, 0, 1000u, &nr, 0) == WIFI_NETS_OK)
    return 1;
  host_ip = 0;
  return 0;
}

int http_call(const char *method, const char *path, const char *token,
              const char *extra, const char *form, char *buf, u32 max,
              HttpReply *r, void (*tick)(u32 ms)) {
  WifiNetResult nr;
  u32 got = 0;
  r->net = WIFI_NETS_OK;
  r->stage = 0;
  r->status = 0;
  r->body = 0;
  r->len = r->from = r->total = 0;
  r->etag[0] = 0;
  if (!host_ip) {
    r->net = wifi_net(WIFI_NETOP_DNS, 0, 0, ACCOUNT_HOST, 3000u, &nr, tick);
    if (r->net != WIFI_NETS_OK)
      return 0;
    host_ip = nr.ip;
  }
  u32 len = build(method, path, token, extra, form);
  if (!len) {
    r->net = WIFI_NETS_TOOBIG;
    return 0;
  }
  r->net = wifi_http(host_ip, 80, req, len, buf, max, &got, &r->stage,
                     HTTP_TIMEOUT_MS, tick);
  for (u32 i = 0; i < len; i++) /* the token was in it */
    ((volatile char *)req)[i] = 0;
  if (r->net == WIFI_NETS_OK)
    parse(buf, got, r);
  else if (r->stage <= WIFI_HTTP_CONNECT)
    host_ip = 0; /* Cloudflare's addresses can change: look it up again */
  return r->status;
}

void http_why(const HttpReply *r, char *out) {
  static const StringId net[] = {
      [WIFI_NETS_NOLINK] = STR_AC_E_NOLINK,
      [WIFI_NETS_TIMEOUT] = STR_AC_E_TIMEOUT,
      [WIFI_NETS_NONAME] = STR_AC_E_DNS,
      [WIFI_NETS_DNSFAIL] = STR_AC_E_DNS,
      [WIFI_NETS_BADNAME] = STR_AC_E_DNS,
      [WIFI_NETS_NOHOST] = STR_AC_E_ROUTER,
      [WIFI_NETS_ICMPERR] = STR_AC_E_TIMEOUT,
      [WIFI_NETS_NOSEND] = STR_AC_E_CHIP,
      [WIFI_NETS_BUSY] = STR_AC_E_CHIP,
      [WIFI_NETS_REFUSED] = STR_AC_E_REFUSED,
      [WIFI_NETS_TOOBIG] = STR_AC_E_REPLY,
  };
  if (r->status) {
    put_num(put(out, "HTTP "), (u32)r->status);
    return;
  }
  if (r->net == WIFI_NETS_OK) {
    put(out, L(STR_AC_E_REPLY));
    return;
  }
  put(out, L(r->net < sizeof(net) / sizeof(net[0]) && net[r->net]
                 ? net[r->net]
                 : STR_AC_E_TIMEOUT));
}
