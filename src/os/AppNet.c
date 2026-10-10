/* SPDX-License-Identifier: GPL-2.0 */
/* LICENSE: GPL-2.0, part of the Wi-Fi driver. See docs/wifi.md "License and
 * credits".
 *
 * The network for apps; see include/appnet.h. The app has replaced the OS,
 * so this posts the core's Wi-Fi commands itself, as WiFi9.c does for the
 * OS, and joins as wifi_net_join() does. HTTP/1.0 with Connection: close
 * suits the core's TCP client, which reads until the server closes or the
 * Content-Length is in. */
#include "appnet.h"
#include "audio.h"
#include "ff.h"
#include "gpu.h"
#include "loader.h"
#include "wifi.h"

extern void os_cache_sync(void);

#define JOIN_MS     180000u
#define DNS_MS      3000u
#define PING_MS     2000u
#define REDIRECTS   5
#define DNS_CACHE   8
#define PIECE_FIRST (64u * 1024u)
#define PIECE_MIN   (16u * 1024u)
#define PIECE_MAX   (512u * 1024u)
#define PIECE_MS    3000u

static AudioCtrl *const ctrl = (AudioCtrl *)AUDIO_CTRL_ADDR;
static WifiNetIo *const io = (WifiNetIo *)WIFI_NET_ADDR;

static char g_err[112];
static int g_busy;
/* The last failure was the network's (no answer, a dropped link), not the
 * request's: worth trying again. */
static int g_transient;

static u32 len_of(const char *s) {
  u32 n = 0;
  while (s[n])
    n++;
  return n;
}

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static int prefix_ci(const char *s, const char *p) {
  while (*p)
    if (lower(*s++) != *p++)
      return 0;
  return 1;
}

static int same_ci(const char *a, const char *b) {
  while (*a && lower(*a) == lower(*b)) {
    a++;
    b++;
  }
  return *a == *b;
}

static int hex_val(int c) {
  return c >= '0' && c <= '9'   ? c - '0'
         : c >= 'a' && c <= 'f' ? c - 'a' + 10
         : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                : -1;
}

/* Output that stops at the end of its buffer and remembers it did. */
typedef struct {
  char *p, *e;
  int over;
} Out;

static void out_str(Out *o, const char *s) {
  while (*s) {
    if (o->p >= o->e) {
      o->over = 1;
      return;
    }
    *o->p++ = *s++;
  }
}

static void out_num(Out *o, u32 v) {
  char t[12];
  int n = 0;
  do {
    t[n++] = (char)('0' + v % 10u);
    v /= 10u;
  } while (v);
  while (n) {
    char c[2] = {t[--n], 0};
    out_str(o, c);
  }
}

static void err(const char *a, const char *b, const char *c) {
  Out o = {g_err, g_err + sizeof(g_err) - 1, 0};
  out_str(&o, a);
  if (b)
    out_str(&o, b);
  if (c)
    out_str(&o, c);
  *o.p = 0;
}

const char *appnet_error(void) { return g_err; }

int appnet_ready(void) {
  const volatile u32 *desc = (const volatile u32 *)AURORA_RETURN_DESC_ADDR;
  os_cache_sync();
  if (desc[0] != AURORA_RETURN_READY_MAGIC || ctrl->magic != AUDIO_MAGIC) {
    err("the network needs the app started from the Home Menu", 0, 0);
    return 0;
  }
  if (ctrl->version < APPNET_CORE_VERSION) {
    err("the network needs a newer AuroraOS", 0, 0);
    return 0;
  }
  return 1;
}

/* The core acknowledges a command once it has finished it. */
static int core_done(void) {
  os_cache_sync();
  return ctrl->ack_seq == ctrl->cmd_seq;
}

int appnet_busy(void) {
  if (g_busy && core_done())
    g_busy = 0;
  return g_busy;
}

void appnet_finish(void) {
  u32 t0 = appnet_ms();
  while (g_busy && !core_done() && appnet_ms() - t0 < JOIN_MS)
    ;
  g_busy = 0;
}

/* The block holds one command, and a GPU present or a sound may still be in
 * it: the operands are written only once the core is idle. */
static int claim(void) {
  u32 t0;
  gpu_wait_idle();
  t0 = appnet_ms();
  while (!core_done())
    if (appnet_ms() - t0 > 3000u) {
      err("the ARM11 core is busy", 0, 0);
      return 0;
    }
  return 1;
}

/* The operands reach memory before the counter that announces them. */
static void send(u32 cmd, u32 arg0) {
  ctrl->cmd = cmd;
  ctrl->arg0 = arg0;
  os_cache_sync();
  ctrl->cmd_seq = ctrl->cmd_seq + 1;
  os_cache_sync();
  g_busy = 1;
}

static int wait(u32 max_ms) {
  u32 t0 = appnet_ms(), shown = 0;
  for (;;) {
    u32 ms;
    if (core_done()) {
      g_busy = 0;
      return 1;
    }
    ms = appnet_ms() - t0;
    if (ms >= max_ms)
      return 0;
    if (ms / 100u != shown) {
      shown = ms / 100u;
      appnet_waiting(ms);
    }
  }
}

static void net_err(u32 st, const char *host) {
  g_transient = 0;
  switch (st) {
    case WIFI_NETS_NOLINK:
      err("not connected to a network", 0, 0);
      g_transient = 1;
      break;
    case WIFI_NETS_TIMEOUT:
      err(host ? host : "the network", " did not answer in time", 0);
      g_transient = 1;
      break;
    case WIFI_NETS_NONAME:
      err("no host is called ", host, 0);
      break;
    case WIFI_NETS_DNSFAIL:
      err("the name ", host, " could not be looked up");
      g_transient = 1;
      break;
    case WIFI_NETS_BADNAME:
      err(host, " is not a valid host name", 0);
      break;
    case WIFI_NETS_NOHOST:
      err(host ? host : "the host", " cannot be reached on this network", 0);
      break;
    case WIFI_NETS_ICMPERR:
      err(host ? host : "the host", " is unreachable", 0);
      break;
    case WIFI_NETS_NOSEND:
      err("the Wi-Fi chip could not send", 0, 0);
      g_transient = 1;
      break;
    case WIFI_NETS_REFUSED:
      err(host ? host : "the server", " refused the connection", 0);
      break;
    case WIFI_NETS_TOOBIG:
      err("the reply is larger than 1020 KB", 0, 0);
      break;
    default:
      err("the Wi-Fi chip did not finish in time", 0, 0);
      g_transient = 1;
      break;
  }
}

static u32 net_op(u32 op, u32 ip, u32 seq, const char *name, u32 timeout_ms) {
  u32 n = 0;
  if (!claim())
    return WIFI_NETS_BUSY;
  io->ip = ip;
  io->seq = seq;
  io->timeout_ms = timeout_ms;
  io->status = WIFI_NETS_NOLINK;
  for (; name && name[n] && n < sizeof(io->name) - 1u; n++)
    io->name[n] = name[n];
  io->name[n] = '\0';
  send(AUDIO_CMD_WIFI_NET, op);
  if (!wait(3u * timeout_ms + 15000u))
    return WIFI_NETS_BUSY;
  return io->status;
}

int appnet_online(void) {
  if (!appnet_ready())
    return 0;
  if (net_op(WIFI_NETOP_STATUS, 0, 0, 0, 1000u) == WIFI_NETS_OK)
    return 1;
  err("not connected to a network", 0, 0);
  return 0;
}

u32 appnet_ip(void) {
  const WifiShared *w = (const WifiShared *)WIFI_SHARED_ADDR;
  os_cache_sync();
  return w->session == 1u ? w->ip : 0;
}

void appnet_ip_text(u32 ip, char *out) {
  Out o = {out, out + 15, 0};
  for (int i = 3; i >= 0; i--) {
    out_num(&o, (ip >> (8 * i)) & 0xFFu);
    if (i)
      out_str(&o, ".");
  }
  *o.p = 0;
}

/* `line` starting with `key`: the rest of it into `out`. */
static void take(const char *line, const char *key, char *out, u32 size) {
  u32 k = 0, n = 0;
  for (; key[k]; k++)
    if (line[k] != key[k])
      return;
  while (line[k + n] && n + 1 < size) {
    out[n] = line[k + n];
    n++;
  }
  out[n] = 0;
}

static void wipe(volatile char *p, u32 n) {
  while (n--)
    *p++ = 0;
}

int appnet_join(void) {
  static FIL f;
  static char buf[256], ssid[33], pass[65];
  UINT br = 0;
  int done;
  g_err[0] = 0;
  if (!appnet_ready())
    return 0;
  if (net_op(WIFI_NETOP_STATUS, 0, 0, 0, 1000u) == WIFI_NETS_OK)
    return 1;
  if (!appnet_mount()) {
    err("the SD card could not be read", 0, 0);
    return 0;
  }
  ssid[0] = pass[0] = 0;
  if (f_open(&f, WIFI_NET_FILE, FA_READ) == FR_OK) {
    if (f_read(&f, buf, sizeof(buf) - 1u, &br) != FR_OK)
      br = 0;
    f_close(&f);
  }
  buf[br] = 0;
  for (char *l = buf; *l;) {
    char *e = l;
    while (*e && *e != '\r' && *e != '\n')
      e++;
    int more = *e != 0;
    *e = 0;
    take(l, "ssid=", ssid, sizeof(ssid));
    take(l, "password=", pass, sizeof(pass));
    l = more ? e + 1 : e;
  }
  wipe(buf, sizeof(buf));
  if (!ssid[0]) {
    err("no network saved: pick one in Settings > Wi-Fi", 0, 0);
    return 0;
  }
  if (!wifi_fw_stage(WIFI_FW_TYPE4)) {
    wipe(pass, sizeof(pass));
    err("no Wi-Fi firmware in /Aurora/wifi (Settings > Wi-Fi copies it)", 0,
        0);
    return 0;
  }
  if (!claim()) {
    wipe(pass, sizeof(pass));
    return 0;
  }
  wifi_request(ssid, pass);
  wipe(pass, sizeof(pass));
  send(AUDIO_CMD_WIFI_BOOT, WIFI_OPT_CONNECT | WIFI_OPT_STAY);
  done = wait(JOIN_MS);
  wifi_request_clear();
  if (!done) {
    err("the Wi-Fi chip did not finish in time", 0, 0);
    return 0;
  }
  if (((const WifiShared *)WIFI_SHARED_ADDR)->session == 1u)
    return 1;
  wifi_join_why((const WifiShared *)WIFI_SHARED_ADDR, ssid, g_err,
                sizeof(g_err));
  return 0;
}

/* "a.b.c.d" exactly. */
static int parse_ip(const char *s, u32 *ip) {
  u32 v = 0;
  for (int i = 0; i < 4; i++) {
    u32 b = 0, d = 0;
    while (*s >= '0' && *s <= '9' && d < 4) {
      b = b * 10u + (u32)(*s++ - '0');
      d++;
    }
    if (!d || b > 255u || *s != (i < 3 ? '.' : 0))
      return 0;
    if (i < 3)
      s++;
    v = v << 8 | b;
  }
  *ip = v;
  return 1;
}

static struct {
  char name[64];
  u32 ip;
} g_dns[DNS_CACHE];
static u32 g_dns_next;

/* A server that did not answer may have moved. */
static void dns_forget(u32 ip) {
  for (u32 i = 0; i < DNS_CACHE; i++)
    if (g_dns[i].ip == ip)
      g_dns[i].name[0] = 0;
}

int appnet_resolve(const char *host, u32 *ip) {
  u32 st, n = len_of(host);
  if (parse_ip(host, ip))
    return 1;
  for (u32 i = 0; i < DNS_CACHE; i++)
    if (g_dns[i].name[0] && same_ci(g_dns[i].name, host)) {
      *ip = g_dns[i].ip;
      return 1;
    }
  if (!appnet_ready())
    return 0;
  st = net_op(WIFI_NETOP_DNS, 0, 0, host, DNS_MS);
  if (st != WIFI_NETS_OK) {
    net_err(st, host);
    return 0;
  }
  *ip = io->ip;
  if (n < sizeof(g_dns[0].name)) {
    u32 k = g_dns_next++ % DNS_CACHE;
    for (u32 i = 0; i <= n; i++)
      g_dns[k].name[i] = host[i];
    g_dns[k].ip = *ip;
  }
  return 1;
}

int appnet_ping(u32 ip, u32 *rtt_us) {
  static u32 seq;
  u32 st;
  if (!appnet_ready())
    return 0;
  st = net_op(WIFI_NETOP_PING, ip, ++seq, 0, PING_MS);
  if (st != WIFI_NETS_OK) {
    net_err(st, 0);
    return 0;
  }
  *rtt_us = io->rtt_us;
  return 1;
}

static u32 exchange(u32 ip, u32 port, const void *data, u32 len, u32 *got,
                    u32 *stage) {
  volatile char *q = (volatile char *)WIFI_HTTP_REQ;
  const char *d = (const char *)data;
  u32 n;
  *got = 0;
  *stage = 0;
  if (!claim())
    return WIFI_NETS_BUSY;
  for (u32 i = 0; i < len; i++)
    q[i] = d[i];
  io->ip = ip;
  io->port = port;
  io->req_len = len;
  io->resp_len = 0;
  io->stage = 0;
  io->timeout_ms = APPNET_TIMEOUT_MS;
  io->status = WIFI_NETS_NOLINK;
  send(AUDIO_CMD_WIFI_NET, WIFI_NETOP_HTTP);
  if (!wait(3u * APPNET_TIMEOUT_MS + 15000u))
    return WIFI_NETS_BUSY;
  n = io->resp_len;
  if (n > WIFI_HTTP_RESP_MAX - 1u)
    n = WIFI_HTTP_RESP_MAX - 1u;
  ((char *)WIFI_HTTP_RESP)[n] = 0;
  *got = n;
  *stage = io->stage;
  return io->status;
}

int appnet_exchange(u32 ip, u32 port, const void *data, u32 len, u32 *got) {
  u32 st, stage;
  char host[16];
  *got = 0;
  g_err[0] = 0;
  if (!appnet_ready())
    return 0;
  if (!port || port > 0xFFFFu) {
    err("the port is not valid", 0, 0);
    return 0;
  }
  if (!len || len > WIFI_HTTP_REQ_MAX) {
    err(len ? "the data to send is longer than 2 KB" : "nothing to send", 0,
        0);
    return 0;
  }
  st = exchange(ip, port, data, len, got, &stage);
  if (st != WIFI_NETS_OK) {
    appnet_ip_text(ip, host);
    net_err(st, host);
    *got = 0;
    return 0;
  }
  return 1;
}

typedef struct {
  char host[256];
  u32 port;
  char path[APPNET_URL_MAX];
} Url;

static const char hexd[] = "0123456789ABCDEF";

/* A path as it goes on the request line: spaces, controls and bytes above
 * 0x7E percent-encoded, a fragment dropped. */
static int path_put(Out *o, const char *s) {
  for (; *s && *s != '#'; s++) {
    unsigned char c = (unsigned char)*s;
    if (c <= 0x20 || c >= 0x7F) {
      char t[4] = {'%', hexd[c >> 4], hexd[c & 15], 0};
      out_str(o, t);
    } else {
      char t[2] = {(char)c, 0};
      out_str(o, t);
    }
  }
  return !o->over;
}

static int url_parse(const char *url, Url *u) {
  const char *s = url;
  u32 n = 0;
  Out o;
  while (*s == ' ')
    s++;
  if (prefix_ci(s, "https://")) {
    err("https:// needs TLS, which AuroraOS does not have yet: use http://", 0,
        0);
    return 0;
  }
  if (prefix_ci(s, "http://")) {
    s += 7;
  } else {
    for (const char *t = s; *t && *t != '/'; t++)
      if (t[0] == ':' && t[1] == '/' && t[2] == '/') {
        err("only http:// addresses are supported", 0, 0);
        return 0;
      }
  }
  while (*s && *s != ':' && *s != '/' && *s != '?' && *s != '#') {
    if (*s == '@' || n + 1 >= sizeof(u->host)) {
      err(*s == '@' ? "user names in addresses are not supported"
                    : "the host name is too long",
          0, 0);
      return 0;
    }
    u->host[n++] = *s++;
  }
  u->host[n] = 0;
  if (!n) {
    err("the address has no host name", 0, 0);
    return 0;
  }
  u->port = 80;
  if (*s == ':') {
    u32 p = 0, d = 0;
    for (s++; *s >= '0' && *s <= '9' && p <= 0xFFFFu; s++, d++)
      p = p * 10u + (u32)(*s - '0');
    if (!d || !p || p > 0xFFFFu || (*s && *s != '/' && *s != '?' && *s != '#')) {
      err("the port in the address is not valid", 0, 0);
      return 0;
    }
    u->port = p;
  }
  o.p = u->path;
  o.e = u->path + sizeof(u->path) - 1u;
  o.over = 0;
  if (*s != '/')
    out_str(&o, "/");
  if (!path_put(&o, s)) {
    err("the address is too long", 0, 0);
    return 0;
  }
  *o.p = 0;
  return 1;
}

/* A Location that is a path: from the root, or beside the current one. */
static int url_relative(Url *u, const char *loc) {
  static char base[APPNET_URL_MAX];
  u32 cut = 0;
  Out o;
  for (u32 i = 0; u->path[i] && u->path[i] != '?'; i++)
    if (u->path[i] == '/')
      cut = i + 1u;
  for (u32 i = 0; i < cut; i++)
    base[i] = u->path[i];
  base[cut] = 0;
  o.p = u->path;
  o.e = u->path + sizeof(u->path) - 1u;
  o.over = 0;
  if (loc[0] != '/')
    out_str(&o, base);
  if (!path_put(&o, loc)) {
    err("the redirect address is too long", 0, 0);
    return 0;
  }
  *o.p = 0;
  return 1;
}

/* Header `name` (lower case, with its colon) at a line start in `h`: where
 * its value starts. */
static const char *find_header(const char *h, const char *name) {
  for (const char *l = h; *l;) {
    if (prefix_ci(l, name)) {
      l += len_of(name);
      while (*l == ' ' || *l == '\t')
        l++;
      return l;
    }
    while (*l && *l != '\n')
      l++;
    if (*l)
      l++;
  }
  return 0;
}

static int has_header(const char *headers, const char *name) {
  return headers && find_header(headers, name);
}

static char g_req[WIFI_HTTP_REQ_MAX];

static u32 build(const char *method, const Url *u, const char *headers,
                 const void *body, u32 blen) {
  Out o = {g_req, g_req + sizeof(g_req), 0};
  out_str(&o, method);
  out_str(&o, " ");
  out_str(&o, u->path);
  out_str(&o, " HTTP/1.0\r\nHost: ");
  out_str(&o, u->host);
  if (u->port != 80u) {
    out_str(&o, ":");
    out_num(&o, u->port);
  }
  out_str(&o, "\r\n");
  if (!has_header(headers, "user-agent:")) {
    out_str(&o, "User-Agent: AuroraOS/" AURORA_VERSION_NUM " (app)\r\n");
  }
  if (!has_header(headers, "accept:"))
    out_str(&o, "Accept: */*\r\n");
  if (!has_header(headers, "accept-encoding:"))
    out_str(&o, "Accept-Encoding: identity\r\n");
  out_str(&o, "Connection: close\r\n");
  /* The caller's lines, each ended with CR LF whatever it ended with. */
  for (const char *h = headers; h && *h;) {
    const char *e = h;
    while (*e && *e != '\r' && *e != '\n')
      e++;
    for (const char *c = h; c < e; c++) {
      char t[2] = {*c, 0};
      out_str(&o, t);
    }
    if (e > h)
      out_str(&o, "\r\n");
    while (*e == '\r' || *e == '\n')
      e++;
    h = e;
  }
  if (blen || same_ci(method, "POST") || same_ci(method, "PUT") ||
      same_ci(method, "PATCH")) {
    out_str(&o, "Content-Length: ");
    out_num(&o, blen);
    out_str(&o, "\r\n");
  }
  out_str(&o, "\r\n");
  if (o.over || (u32)(o.e - o.p) < blen) {
    err("the request is longer than 2 KB (address, headers and body)", 0, 0);
    return 0;
  }
  for (u32 i = 0; i < blen; i++)
    *o.p++ = ((const char *)body)[i];
  return (u32)(o.p - g_req);
}

static const char *read_num(const char *s, u32 *v) {
  *v = 0;
  while (*s >= '0' && *s <= '9' && *v < 0x10000000u)
    *v = *v * 10u + (u32)(*s++ - '0');
  return s;
}

/* Chunked transfer coding undone in place; returns the data's length. A
 * chunk the reply cut short keeps what came. */
static u32 dechunk(char *b, u32 n) {
  u32 i = 0, o = 0;
  for (;;) {
    u32 size = 0;
    int digits = 0;
    while (i < n && hex_val(b[i]) >= 0 && size <= n) {
      size = size * 16u + (u32)hex_val(b[i++]);
      digits++;
    }
    if (!digits)
      break;
    while (i < n && b[i] != '\n')
      i++;
    i++;
    if (!size || i >= n)
      break;
    if (size > n - i)
      size = n - i;
    for (u32 k = 0; k < size; k++)
      b[o + k] = b[i + k];
    o += size;
    i += size;
    if (i < n && b[i] == '\r')
      i++;
    if (i < n && b[i] == '\n')
      i++;
  }
  return o;
}

static int parse(char *b, u32 n, AppNetReply *r) {
  u32 i = 5, end = 0, hend = 0, cl;
  const char *v;
  if (n < 12u || !prefix_ci(b, "http/"))
    return 0;
  while (i < n && b[i] != ' ')
    i++;
  if (i + 4u > n || b[i + 1] < '1' || b[i + 1] > '5' || b[i + 2] < '0' ||
      b[i + 2] > '9' || b[i + 3] < '0' || b[i + 3] > '9')
    return 0;
  r->status = (b[i + 1] - '0') * 100 + (b[i + 2] - '0') * 10 + (b[i + 3] - '0');
  for (i = 0; i + 1u < n; i++) {
    if (b[i] != '\n')
      continue;
    if (b[i + 1] == '\n') {
      hend = i;
      end = i + 2u;
      break;
    }
    if (b[i + 1] == '\r' && i + 2u < n && b[i + 2] == '\n') {
      hend = i;
      end = i + 3u;
      break;
    }
  }
  if (!end) {
    r->status = 0;
    return 0;
  }
  if (b[hend - 1] == '\r')
    hend--;
  b[hend] = 0;
  r->head = b;
  r->body = b + end;
  r->len = n - end;
  v = find_header(b, "transfer-encoding:");
  if (v && prefix_ci(v, "chunked")) {
    r->len = dechunk(r->body, r->len);
  } else if ((v = find_header(b, "content-length:")) != 0) {
    read_num(v, &cl);
    if (cl < r->len)
      r->len = cl;
  }
  /* "bytes 6144-12287/28616" */
  v = find_header(b, "content-range:");
  if (v && prefix_ci(v, "bytes ")) {
    u32 last;
    v = read_num(v + 6, &r->from);
    if (*v == '-') {
      v = read_num(v + 1, &last);
      if (*v == '/' && last >= r->from)
        read_num(v + 1, &r->total);
    }
  }
  r->body[r->len] = 0;
  return 1;
}

int appnet_header(const AppNetReply *r, const char *name, char *out,
                  u32 max) {
  static char key[64];
  u32 n = 0, k = 0;
  const char *v;
  if (!max)
    return 0;
  out[0] = 0;
  while (name[k] && k + 2u < sizeof(key)) {
    key[k] = (char)lower(name[k]);
    k++;
  }
  key[k++] = ':';
  key[k] = 0;
  if (!r->head || !(v = find_header(r->head, key)))
    return 0;
  while (v[n] && v[n] != '\r' && v[n] != '\n' && n + 1u < max) {
    out[n] = v[n];
    n++;
  }
  while (n && (out[n - 1] == ' ' || out[n - 1] == '\t'))
    n--;
  out[n] = 0;
  return 1;
}

static int redirect(int st) {
  return st == 301 || st == 302 || st == 303 || st == 307 || st == 308;
}

int appnet_http(const char *method, const char *url, const char *headers,
                const void *body, u32 blen, AppNetReply *r) {
  static Url u;
  static char loc[APPNET_URL_MAX];
  u32 ip, len, got, stage, st;
  r->status = 0;
  r->head = r->body = (char *)"";
  r->len = r->from = r->total = 0;
  g_err[0] = 0;
  g_transient = 0;
  if (!appnet_ready() || !url_parse(url, &u))
    return 0;
  for (int hops = 0;; hops++) {
    if (!appnet_resolve(u.host, &ip))
      return 0;
    len = build(method, &u, headers, body, blen);
    if (!len)
      return 0;
    st = exchange(ip, u.port, g_req, len, &got, &stage);
    wipe(g_req, len); /* it may carry a key */
    if (st != WIFI_NETS_OK) {
      if (stage <= WIFI_HTTP_CONNECT)
        dns_forget(ip);
      net_err(st, u.host);
      return 0;
    }
    if (!parse((char *)WIFI_HTTP_RESP, got, r)) {
      err(got ? "the reply is not HTTP" : "the server closed the connection without a reply",
          0, 0);
      g_transient = !got;
      return 0;
    }
    if (!redirect(r->status) || !appnet_header(r, "location", loc, sizeof(loc)))
      return r->status;
    if (hops >= REDIRECTS) {
      err("too many redirects", 0, 0);
      return r->status;
    }
    if (prefix_ci(loc, "https://")) {
      err("redirected to https://, which needs TLS: AuroraOS does not have it "
          "yet", 0, 0);
      return r->status;
    }
    if (loc[0] == '/' && loc[1] == '/') {
      static char abs[APPNET_URL_MAX + 8];
      Out o = {abs, abs + sizeof(abs) - 1u, 0};
      out_str(&o, "http:");
      out_str(&o, loc);
      *o.p = 0;
      if (!url_parse(abs, &u))
        return r->status;
    } else if (prefix_ci(loc, "http://")) {
      if (!url_parse(loc, &u))
        return r->status;
    } else if (!url_relative(&u, loc)) {
      return r->status;
    }
    /* As browsers do: 303 always, and 301 or 302 after a POST, become GET. */
    if (!same_ci(method, "HEAD") &&
        (r->status == 303 || ((r->status == 301 || r->status == 302) &&
                              same_ci(method, "POST")))) {
      method = "GET";
      body = 0;
      blen = 0;
    }
  }
}

u32 appnet_url_encode(char *out, u32 max, const char *s) {
  u32 n = 0;
  if (!max)
    return 0;
  for (; *s; s++) {
    unsigned char c = (unsigned char)*s;
    int keep = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
               (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
               c == '~';
    if (n + (keep ? 1u : 3u) >= max)
      break;
    if (keep) {
      out[n++] = (char)c;
    } else {
      out[n++] = '%';
      out[n++] = hexd[c >> 4];
      out[n++] = hexd[c & 15];
    }
  }
  out[n] = 0;
  return n;
}

static int put_file(FIL *f, const char *data, u32 len) {
  UINT bw = 0;
  return f_write(f, data, len, &bw) == FR_OK && bw == len;
}

int appnet_download(const char *url, const char *headers, const char *path,
                    void (*progress)(u32 done, u32 total)) {
  static FIL f;
  static char part[300], range[1100];
  AppNetReply r;
  u32 piece = PIECE_FIRST, done = 0, total = 0;
  int tries = 0, ok = 0, st;
  Out o = {part, part + sizeof(part) - 1u, 0};
  g_err[0] = 0;
  if (!appnet_ready())
    return 0;
  out_str(&o, path);
  out_str(&o, ".part");
  *o.p = 0;
  if (o.over) {
    err("the file name is too long", 0, 0);
    return 0;
  }
  if (!appnet_mount() || f_open(&f, part, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) {
    err("could not create ", path, 0);
    return 0;
  }
  for (;;) {
    u32 t0 = appnet_ms(), ms;
    Out ro = {range, range + sizeof(range) - 1u, 0};
    out_str(&ro, "Range: bytes=");
    out_num(&ro, done);
    out_str(&ro, "-");
    out_num(&ro, done + piece - 1u);
    out_str(&ro, "\r\n");
    if (headers)
      out_str(&ro, headers);
    *ro.p = 0;
    if (ro.over) {
      err("the headers are too long", 0, 0);
      break;
    }
    st = appnet_http("GET", url, range, 0, 0, &r);
    if (!st) {
      if (g_transient && ++tries < 3) {
        piece = piece / 2u < PIECE_MIN ? PIECE_MIN : piece / 2u;
        continue;
      }
      break;
    }
    tries = 0;
    if (st == 200 && done == 0) {
      /* The server sent the whole file. */
      if (!put_file(&f, r.body, r.len)) {
        err("could not write ", path, 0);
        break;
      }
      done = total = r.len;
    } else if (st == 206 && r.from == done && r.len) {
      if (!put_file(&f, r.body, r.len)) {
        err("could not write ", path, 0);
        break;
      }
      done += r.len;
      total = r.total;
      if (!total && r.len < piece)
        total = done;
    } else {
      if (!g_err[0]) {
        Out eo = {g_err, g_err + sizeof(g_err) - 1u, 0};
        out_str(&eo, "the server answered HTTP ");
        out_num(&eo, (u32)st);
        *eo.p = 0;
      }
      break;
    }
    ms = appnet_ms() - t0;
    if (ms) {
      u32 next = (u32)((u64)r.len * PIECE_MS / ms) & ~0xFFFu;
      if (next > piece * 4u)
        next = piece * 4u;
      piece = next < PIECE_MIN ? PIECE_MIN : next > PIECE_MAX ? PIECE_MAX : next;
    }
    if (progress)
      progress(done, total);
    if (total && done >= total) {
      ok = 1;
      break;
    }
  }
  if (f_close(&f) != FR_OK && ok) {
    err("could not write ", path, 0);
    ok = 0;
  }
  if (ok) {
    f_unlink(path);
    if (f_rename(part, path) != FR_OK) {
      err("could not save ", path, 0);
      ok = 0;
    }
  }
  if (!ok)
    f_unlink(part);
  return ok;
}
