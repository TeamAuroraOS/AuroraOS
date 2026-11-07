/* SPDX-License-Identifier: GPL-2.0 */
/* LICENSE: GPL-2.0, part of the Wi-Fi driver. See docs/wifi.md "License and
 * credits".
 *
 * The network for apps; see include/appnet.h. The app has replaced the OS,
 * so this posts the core's Wi-Fi commands itself, as WiFi9.c does for the
 * OS, and joins as wifi_net_join() does. HTTP/1.0 with Connection: close
 * suits the core's TCP client, which reads until the server closes or the
 * Content-Length is in, and Tls.c, which does the same over TLS. The OS
 * links it too, for the Terminal's curl. */
#include "appnet.h"
#include "audio.h"
#include "ff.h"
#include "gpu.h"
#include "tls.h"
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

static int st_is_open(void);

static char g_err[112];
static int g_busy;
/* The last failure was the network's (no answer, a dropped link), not the
 * request's: worth trying again. */
static int g_transient;
static int g_tls_err; /* the last WIFI_NETS_TLS's reason */
static u32 g_last_st;
static int g_tcp_held; /* appnet_tcp_open's caller has the rings */

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
  os_cache_sync();
  if (!appnet_launched() || ctrl->magic != AUDIO_MAGIC) {
    g_last_st = WIFI_NETS_NOLINK;
    err("the network needs the app started from the Home Menu", 0, 0);
    return 0;
  }
  if (ctrl->version < APPNET_CORE_VERSION) {
    g_last_st = WIFI_NETS_NOLINK;
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

/* What the core runs without ending a background TCP connection. */
static int beside_tcp(u32 op) {
  return op == WIFI_NETOP_STATUS ||
         (ctrl->version >= AUDIO_UDP_VERSION &&
          (op == WIFI_NETOP_DNS || op == WIFI_NETOP_UDP_BIND ||
           op == WIFI_NETOP_UDP_UNBIND));
}

/* The block holds one command, and a GPU present or a sound may still be in
 * it: the operands are written only once the core is idle. */
static int claim_op(u32 op) {
  u32 t0;
  if (g_tcp_held && !beside_tcp(op)) {
    g_last_st = WIFI_NETS_BUSY;
    err("a socket holds the network connection: close it first", 0, 0);
    return 0;
  }
  gpu_wait_idle();
  t0 = appnet_ms();
  while (!core_done())
    if (appnet_ms() - t0 > 3000u) {
      g_last_st = WIFI_NETS_BUSY;
      err("the ARM11 core is busy", 0, 0);
      return 0;
    }
  return 1;
}

static int claim(void) { return claim_op(0); }

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
  g_last_st = st;
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
    case WIFI_NETS_TLS:
      err(host ? host : "the server", ": ", tls_error_text(g_tls_err));
      break;
    case WIFI_NETS_BUSY:
      if (g_tcp_held) {
        err("a socket holds the network connection: close it first", 0, 0);
        break;
      }
      /* fall through */
    default:
      err("the Wi-Fi chip did not finish in time", 0, 0);
      g_transient = 1;
      break;
  }
}

static u32 net_op(u32 op, u32 ip, u32 seq, const char *name, u32 timeout_ms) {
  u32 n = 0;
  if (!claim_op(op))
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

/* With a connection running in the background the core answers STATUS
 * without writing WifiNetIo, which still reports on the connection. */
static int bg_running(void) {
  return st_is_open() || (g_tcp_held && wifi_tcp_state() != WIFI_TCP_CLOSED);
}

int appnet_online(void) {
  if (!appnet_ready())
    return 0;
  if (bg_running())
    return 1;
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
  if (bg_running())
    return 1;
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

static int tls_start(void) {
  send(AUDIO_CMD_WIFI_NET, WIFI_NETOP_TCP);
  return 1;
}

static int tls_done(void) {
  if (!core_done())
    return 0;
  g_busy = 0;
  return 1;
}

/* exchange() over TLS (Tls.c), the reply decrypted into WIFI_HTTP_RESP. */
static u32 exchange_tls(const char *host, u32 ip, u32 port, const void *data,
                        u32 len, int head_only, u32 *got, u32 *stage) {
  static const TlsLink link = {tls_start, tls_done, appnet_waiting,
                               appnet_ms};
  u32 st;
  *got = 0;
  *stage = 0;
  if (!claim())
    return WIFI_NETS_BUSY;
  st = tls_exchange(&link, host, ip, port, data, len, head_only,
                    (char *)WIFI_HTTP_RESP, WIFI_HTTP_RESP_MAX - 1u, got,
                    stage, APPNET_TIMEOUT_MS, &g_tls_err);
  ((char *)WIFI_HTTP_RESP)[*got] = 0;
  return st;
}

typedef struct {
  char host[256];
  u32 port;
  int tls; /* https:// */
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
  u->tls = 0;
  if (prefix_ci(s, "https://")) {
    s += 8;
    u->tls = 1;
  } else if (prefix_ci(s, "http://")) {
    s += 7;
  } else {
    for (const char *t = s; *t && *t != '/'; t++)
      if (t[0] == ':' && t[1] == '/' && t[2] == '/') {
        err("only http:// and https:// addresses are supported", 0, 0);
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
  u->port = u->tls ? 443u : 80u;
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

/* A Location that is a path: from the root, or beside the current one. One that is
 * only a query keeps the whole path. */
static int url_relative(Url *u, const char *loc) {
  static char base[APPNET_URL_MAX];
  u32 cut = 0;
  Out o;
  for (u32 i = 0; u->path[i] && u->path[i] != '?'; i++)
    if (u->path[i] == '/' || loc[0] == '?')
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

/* Where a Location sends a request that went to `u`; 0 with appnet_error() when it
 * cannot be followed. */
static int url_follow(Url *u, const char *loc) {
  if (loc[0] == '/' && loc[1] == '/') {
    static char abs[APPNET_URL_MAX + 8];
    Out o = {abs, abs + sizeof(abs) - 1u, 0};
    out_str(&o, u->tls ? "https:" : "http:");
    out_str(&o, loc);
    *o.p = 0;
    return url_parse(abs, u);
  }
  if (prefix_ci(loc, "http://") || prefix_ci(loc, "https://"))
    return url_parse(loc, u);
  /* Any other scheme: a relative path has no colon in its first part. */
  for (const char *s = loc; *s && *s != '/' && *s != '?' && *s != '#'; s++)
    if (*s == ':') {
      err("the server redirected to an address that is not http:// or https://", 0, 0);
      return 0;
    }
  return url_relative(u, loc);
}

static int redirect(int st) {
  return st == 301 || st == 302 || st == 303 || st == 307 || st == 308;
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

/* The caller's lines, each ended with CR LF whatever it ended with. `secret`: leave
 * out its Authorization and Cookie lines. */
static void out_headers(Out *o, const char *headers, int secret) {
  for (const char *h = headers; h && *h;) {
    const char *e = h;
    while (*e && *e != '\r' && *e != '\n')
      e++;
    if (!secret ||
        (!prefix_ci(h, "authorization:") && !prefix_ci(h, "cookie:"))) {
      for (const char *c = h; c < e; c++) {
        char t[2] = {*c, 0};
        out_str(o, t);
      }
      if (e > h)
        out_str(o, "\r\n");
    }
    while (*e == '\r' || *e == '\n')
      e++;
    h = e;
  }
}

static char g_req[WIFI_HTTP_REQ_MAX];

/* `secret`: leave out the caller's Authorization and Cookie lines. */
static u32 build(const char *method, const Url *u, const char *headers,
                 int secret, const void *body, u32 blen) {
  Out o = {g_req, g_req + sizeof(g_req), 0};
  out_str(&o, method);
  out_str(&o, " ");
  out_str(&o, u->path);
  out_str(&o, " HTTP/1.0\r\nHost: ");
  out_str(&o, u->host);
  if (u->port != (u->tls ? 443u : 80u)) {
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
  out_headers(&o, headers, secret);
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

static const char *read_num(const char *s, u32 *v);

/* A stream's connection. Which host it is on is kept so the next GET can reuse
 * it; a reply fully read leaves it ready for one. */
enum { RX_NONE, RX_HEAD, RX_BODY, RX_DONE, RX_FAILED };

#define STREAM_HEAD_MAX 3072u
#define STREAM_REDIRECTS 5
/* Bounds a stage with no progress at all, not a whole reply. */
#define STREAM_STALL_MS APPNET_TIMEOUT_MS

static struct {
  char host[256];
  u32 ip, port;
  int tls, open;
  int rx, status, closes, redirects;
  int to_close; /* no Content-Length: the body ends with the connection */
  /* Where the caller asked: its keys go nowhere else. */
  char first_host[256];
  u32 first_port;
  int first_tls, moved;
  u32 len, got, tx_len, tx_done, head_len, last_ms;
  char head[STREAM_HEAD_MAX];
  char tx[APPNET_URL_MAX + 1024u];
  char headers[512];
} g_st;

static Url g_st_url;

static int st_tcp_start(void) {
  if (!claim())
    return 0;
  send(AUDIO_CMD_WIFI_NET, WIFI_NETOP_TCP_BG);
  /* It ends as soon as the SYN is out; ARP may take a moment. Not wait(): its
   * hook may draw, and a present would overwrite this command in its slot. */
  for (u32 t0 = appnet_ms(); !core_done();)
    if (appnet_ms() - t0 >= 5000u)
      return 0;
  g_busy = 0;
  return 1;
}

static int st_tcp_done(void) { return wifi_tcp_state() == WIFI_TCP_CLOSED; }

static void st_close(int graceful) {
  if (!g_st.open)
    return;
  if (g_st.tls) {
    tls_stream_close(graceful);
  } else {
    u32 t0 = appnet_ms();
    wifi_tcp_close(graceful ? WIFI_TCP_FIN : WIFI_TCP_ABORT);
    while (!st_tcp_done() && appnet_ms() - t0 < 5000u)
      wifi_tcp_alive();
  }
  g_st.open = 0;
}

static int st_is_open(void) { return g_st.open; }

void appnet_stream_close(void) {
  st_close(1);
  g_st.rx = RX_NONE;
}

static int st_fail(const char *why) {
  if (why)
    err(why, 0, 0);
  st_close(0);
  g_st.rx = RX_FAILED;
  return APPNET_STREAM_FAILED;
}

static int st_open(const Url *u) {
  static const TlsLink link = {st_tcp_start, st_tcp_done, 0, appnet_ms};
  u32 ip, st;
  if (!appnet_ready())
    return 0;
  if (ctrl->version < AUDIO_TCP_BG_VERSION) {
    err("streams need a newer AuroraOS", 0, 0);
    return 0;
  }
  if (g_tcp_held) {
    err("a socket holds the network connection: close it first", 0, 0);
    return 0;
  }
  if (!appnet_resolve(u->host, &ip))
    return 0;
  if (u->tls) {
    st = tls_stream_open(&link, u->host, ip, u->port, APPNET_TIMEOUT_MS,
                         &g_tls_err);
    if (st != WIFI_NETS_OK) {
      if (st == WIFI_NETS_TLS)
        err(u->host, ": ", tls_error_text(g_tls_err));
      else
        err("the ARM11 core is busy", 0, 0);
      return 0;
    }
  } else {
    wifi_tcp_setup(ip, u->port, APPNET_TIMEOUT_MS);
    if (!st_tcp_start()) {
      err("the ARM11 core is busy", 0, 0);
      return 0;
    }
  }
  for (u32 i = 0; i <= len_of(u->host); i++)
    g_st.host[i] = u->host[i];
  g_st.ip = ip;
  g_st.port = u->port;
  g_st.tls = u->tls;
  g_st.open = 1;
  return 1;
}

static int st_send_request(const Url *u, const char *headers, int secret) {
  Out o = {g_st.tx, g_st.tx + sizeof(g_st.tx), 0};
  out_str(&o, "GET ");
  out_str(&o, u->path);
  out_str(&o, " HTTP/1.1\r\nHost: ");
  out_str(&o, u->host);
  if (u->port != (u->tls ? 443u : 80u)) {
    out_str(&o, ":");
    out_num(&o, u->port);
  }
  out_str(&o, "\r\n");
  if (!has_header(headers, "user-agent:"))
    out_str(&o, "User-Agent: AuroraOS/" AURORA_VERSION_NUM " (app)\r\n");
  if (!has_header(headers, "accept:"))
    out_str(&o, "Accept: */*\r\n");
  if (!has_header(headers, "accept-encoding:"))
    out_str(&o, "Accept-Encoding: identity\r\n");
  out_str(&o, "Connection: keep-alive\r\n");
  out_headers(&o, headers, secret);
  out_str(&o, "\r\n");
  if (o.over) {
    err("the request is too long (address and headers)", 0, 0);
    return 0;
  }
  g_st.tx_len = (u32)(o.p - g_st.tx);
  g_st.tx_done = 0;
  g_st.head_len = 0;
  g_st.len = g_st.got = 0;
  g_st.status = 0;
  g_st.closes = 0;
  g_st.to_close = 0;
  g_st.rx = RX_HEAD;
  g_st.last_ms = appnet_ms();
  return 1;
}

static int st_ended(void) {
  if (g_st.tls) {
    int e = 0;
    if (!tls_stream_closed(&e))
      return 0;
    if (e)
      g_tls_err = e;
    return 1;
  }
  return wifi_tcp_state() == WIFI_TCP_CLOSED;
}

/* The request to `u`, on the connection there is if it still suits. */
static int st_request(const Url *u) {
  if (g_st.open && g_st.tls)
    while (tls_stream_pump())
      ;
  /* A reply not read to its end leaves its bytes in the way of the next, and a
   * server may have ended a connection left idle. */
  if (g_st.open && (g_st.rx == RX_HEAD || g_st.rx == RX_BODY || g_st.closes || st_ended() ||
                    !same_ci(g_st.host, u->host) || g_st.port != u->port ||
                    g_st.tls != u->tls))
    st_close(g_st.rx == RX_DONE);
  if (!g_st.open && !st_open(u)) {
    g_st.rx = RX_FAILED;
    return 0;
  }
  /* As in appnet_http: a key meant for one server goes to no other, and never in
   * clear text after asking for TLS. */
  g_st.moved = !same_ci(u->host, g_st.first_host) || u->port != g_st.first_port ||
               u->tls != g_st.first_tls;
  if (!st_send_request(u, g_st.headers[0] ? g_st.headers : 0, g_st.moved)) {
    g_st.rx = RX_FAILED;
    return 0;
  }
  return 1;
}

int appnet_stream_get(const char *url, const char *headers) {
  Url *u = &g_st_url;
  u32 n = 0;
  g_err[0] = 0;
  if (!url_parse(url, u))
    return 0;
  if (headers) {
    n = len_of(headers);
    if (n >= sizeof(g_st.headers)) {
      err("the request's headers are too long", 0, 0);
      return 0;
    }
    for (u32 i = 0; i <= n; i++)
      g_st.headers[i] = headers[i];
  } else {
    g_st.headers[0] = 0;
  }
  g_st.redirects = 0;
  for (u32 i = 0; i <= len_of(u->host); i++)
    g_st.first_host[i] = u->host[i];
  g_st.first_port = u->port;
  g_st.first_tls = u->tls;
  return st_request(u);
}

/* Raw bytes off the connection: TLS's plaintext, or TCP's own. */
static u32 st_recv(void *p, u32 n) {
  return g_st.tls ? tls_stream_read(p, n) : wifi_tcp_read(p, n);
}

static u32 st_xmit(const void *p, u32 n) {
  return g_st.tls ? tls_stream_write(p, n) : wifi_tcp_write(p, n);
}

/* A Content-Length, which read_num would cut short above 268 MB; 0 if it is too big
 * for 32 bits. */
static int st_length(const char *s, u32 *v) {
  *v = 0;
  for (; *s >= '0' && *s <= '9'; s++) {
    u32 d = (u32)(*s - '0');
    if (*v > (0xFFFFFFFFu - d) / 10u)
      return 0;
    *v = *v * 10u + d;
  }
  return 1;
}

/* The status line and headers in, one piece at a time: only as much as the
 * head needs, so body bytes stay in the engine for appnet_stream_read. Returns
 * 1 when the head is complete and parsed. */
static int st_head(void) {
  u32 code;
  for (;;) {
    u32 n = g_st.head_len;
    if (n >= 4u && g_st.head[n - 4] == '\r' && g_st.head[n - 3] == '\n' &&
        g_st.head[n - 2] == '\r' && g_st.head[n - 1] == '\n') {
      const char *s = g_st.head;
      g_st.head[n] = 0;
      if (!prefix_ci(s, "http/")) {
        st_fail("the reply is not HTTP");
        return 0;
      }
      while (*s && *s != ' ')
        s++;
      code = 0;
      if (*s)
        read_num(s + 1, &code);
      if (code < 100u || code >= 200u)
        break;
      /* An interim reply: the real one follows. */
      g_st.head_len = 0;
      continue;
    }
    if (n >= sizeof(g_st.head) - 1u) {
      st_fail("the server's reply headers are too long");
      return 0;
    }
    if (!st_recv(g_st.head + n, 1u))
      return 0;
    g_st.head_len = n + 1u;
    g_st.last_ms = appnet_ms();
  }
  g_st.status = (int)code;
  g_st.len = 0;
  {
    const char *v = find_header(g_st.head, "content-length:");
    int sized = v != 0;
    if (v && !st_length(v, &g_st.len)) {
      st_fail("the reply is over 4 GB, too long for a stream");
      return 0;
    }
    if (find_header(g_st.head, "transfer-encoding:")) {
      st_fail("the server sent a chunked reply, which a stream cannot read");
      return 0;
    }
    v = find_header(g_st.head, "connection:");
    g_st.closes = v ? prefix_ci(v, "close") : prefix_ci(g_st.head, "http/1.0");
    /* Nothing says where the body ends but the connection; these two have none. */
    g_st.to_close = !sized && code != 204u && code != 304u;
    if (g_st.to_close)
      g_st.closes = 1;
  }
  return 1;
}

int appnet_stream_poll(void) {
  int ended;
  if (!g_st.open) {
    if (g_st.rx == RX_FAILED)
      return APPNET_STREAM_FAILED;
    if (g_st.rx == RX_DONE)
      return APPNET_STREAM_DONE;
    return APPNET_STREAM_IDLE;
  }
  if (g_st.tls)
    while (tls_stream_pump())
      ;
  else
    wifi_tcp_alive();
  /* Asked before reading, so all that came ahead of the end is there to read. */
  ended = st_ended();

  if (g_st.rx != RX_NONE && g_st.tx_done < g_st.tx_len) {
    u32 k = st_xmit(g_st.tx + g_st.tx_done, g_st.tx_len - g_st.tx_done);
    if (k) {
      g_st.tx_done += k;
      g_st.last_ms = appnet_ms();
    }
  }
  if (g_st.rx == RX_HEAD && st_head()) {
    const char *v = redirect(g_st.status) ? find_header(g_st.head, "location:") : 0;
    g_st.last_ms = appnet_ms();
    if (v) {
      static char loc[APPNET_URL_MAX];
      u32 n = 0;
      for (; v[n] && v[n] != '\r' && v[n] != '\n'; n++) {
        if (n + 1u >= sizeof(loc))
          return st_fail("the redirect address is too long");
        loc[n] = v[n];
      }
      while (n && (loc[n - 1] == ' ' || loc[n - 1] == '\t'))
        n--;
      loc[n] = 0;
      if (!n || ++g_st.redirects > STREAM_REDIRECTS)
        return st_fail("too many redirects, or one that cannot be followed");
      if (!url_follow(&g_st_url, loc))
        return st_fail(0);
      /* Maybe a new host, and the body of this one is usually empty. */
      st_close(0);
      return st_request(&g_st_url) ? APPNET_STREAM_WAITING : APPNET_STREAM_FAILED;
    }
    g_st.rx = g_st.len || g_st.to_close ? RX_BODY : RX_DONE;
  }
  /* In a body what is still buffered can be read: appnet_stream_read finds the end. */
  if (g_st.rx == RX_HEAD && ended) {
    if (g_st.tls && g_tls_err)
      return st_fail(tls_error_text(g_tls_err));
    return st_fail("the server closed the connection before replying");
  }
  if ((g_st.rx == RX_HEAD || g_st.rx == RX_BODY) &&
      appnet_ms() - g_st.last_ms >= STREAM_STALL_MS)
    return st_fail("the server stopped sending");
  return g_st.rx == RX_HEAD     ? APPNET_STREAM_WAITING
         : g_st.rx == RX_BODY   ? APPNET_STREAM_BODY
         : g_st.rx == RX_DONE   ? APPNET_STREAM_DONE
         : g_st.rx == RX_FAILED ? APPNET_STREAM_FAILED
                                : APPNET_STREAM_IDLE;
}

u32 appnet_stream_read(void *buf, u32 max) {
  u32 n;
  if (g_st.rx != RX_BODY)
    return 0;
  if (!g_st.to_close && max > g_st.len - g_st.got)
    max = g_st.len - g_st.got;
  n = st_recv(buf, max);
  if (!n) {
    if (!st_ended())
      return 0;
    /* Once more, for what came just before the end. */
    n = st_recv(buf, max);
    if (!n) {
      if (g_st.to_close && !(g_st.tls && g_tls_err))
        g_st.rx = RX_DONE;
      else
        st_fail("the connection ended in the middle of a reply");
      return 0;
    }
  }
  g_st.got += n;
  g_st.last_ms = appnet_ms();
  if (!g_st.to_close && g_st.got == g_st.len)
    g_st.rx = RX_DONE;
  return n;
}

int appnet_stream_status(void) { return g_st.status; }

const char *appnet_stream_url(void) {
  static char at[APPNET_URL_MAX + 280];
  Out o = {at, at + sizeof(at) - 1u, 0};
  out_str(&o, g_st_url.tls ? "https://" : "http://");
  out_str(&o, g_st_url.host);
  if (g_st_url.port != (g_st_url.tls ? 443u : 80u)) {
    out_str(&o, ":");
    out_num(&o, g_st_url.port);
  }
  out_str(&o, g_st_url.path);
  *o.p = 0;
  return at;
}

u32 appnet_stream_length(void) { return g_st.len; }

int appnet_stream_moved(void) { return g_st.moved; }

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

int appnet_http(const char *method, const char *url, const char *headers,
                const void *body, u32 blen, AppNetReply *r) {
  static Url u;
  static char loc[APPNET_URL_MAX], first_host[sizeof(u.host)];
  u32 ip, len, got, stage, st, first_port;
  int first_tls;
  r->status = 0;
  r->head = r->body = (char *)"";
  r->len = r->from = r->total = 0;
  g_err[0] = 0;
  g_transient = 0;
  if (!appnet_ready() || !url_parse(url, &u))
    return 0;
  for (u32 i = 0; i < sizeof(u.host); i++)
    first_host[i] = u.host[i];
  first_port = u.port;
  first_tls = u.tls;
  for (int hops = 0;; hops++) {
    if (u.tls && ctrl->version < AUDIO_TCP_VERSION) {
      err("https:// needs a newer AuroraOS (Beta 7 or later)", 0, 0);
      return hops ? r->status : 0;
    }
    if (!appnet_resolve(u.host, &ip))
      return 0;
    /* A key the caller meant for one server goes to no other, and never in
     * clear text after asking for TLS. */
    len = build(method, &u, headers,
                !same_ci(u.host, first_host) || u.port != first_port ||
                    u.tls != first_tls,
                body, blen);
    if (!len)
      return 0;
    st = u.tls ? exchange_tls(u.host, ip, u.port, g_req, len,
                              same_ci(method, "HEAD"), &got, &stage)
               : exchange(ip, u.port, g_req, len, &got, &stage);
    wipe(g_req, len); /* it may carry a key */
    if (st != WIFI_NETS_OK) {
      if (stage <= WIFI_HTTP_CONNECT && st != WIFI_NETS_BUSY)
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
    if (!url_follow(&u, loc))
      return r->status;
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
    if (st == 200 && done == 0) {
      /* The server sent the whole file, unless the connection cut it short. */
      char cl[12];
      u32 want;
      if (appnet_header(&r, "content-length", cl, sizeof(cl))) {
        read_num(cl, &want);
        if (want != r.len && ++tries < 3)
          continue;
        if (want != r.len) {
          err("the download was cut short: ", path, 0);
          break;
        }
      }
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
    tries = 0;
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

int appnet_tcp_open(u32 ip, u32 port, u32 timeout_ms) {
  g_err[0] = 0;
  if (!appnet_ready())
    return 0;
  if (ctrl->version < AUDIO_TCP_BG_VERSION) {
    g_last_st = WIFI_NETS_NOLINK;
    err("sockets need a newer AuroraOS", 0, 0);
    return 0;
  }
  if (g_tcp_held || st_is_open()) {
    g_last_st = WIFI_NETS_BUSY;
    err("the network connection is in use", 0, 0);
    return 0;
  }
  wifi_tcp_setup(ip, port, timeout_ms);
  if (!st_tcp_start()) {
    if (!g_err[0]) {
      g_last_st = WIFI_NETS_BUSY;
      err("the ARM11 core did not take the connection", 0, 0);
    }
    return 0;
  }
  g_tcp_held = 1;
  return 1;
}

u32 appnet_tcp_status(void) {
  os_cache_sync();
  return ctrl->version >= AUDIO_UDP_VERSION ? wifi_tcp_end_status()
                                            : io->status;
}

u32 appnet_core_version(void) {
  os_cache_sync();
  return ctrl->magic == AUDIO_MAGIC ? ctrl->version : 0;
}

/* As st_tcp_start: a short wait, with nothing drawn meanwhile. */
static int udp_op(u32 op) {
  if (!claim_op(op))
    return 0;
  send(AUDIO_CMD_WIFI_NET, op);
  for (u32 t0 = appnet_ms(); !core_done();)
    if (appnet_ms() - t0 >= 5000u) {
      g_last_st = WIFI_NETS_BUSY;
      err("the ARM11 core did not answer", 0, 0);
      return 0;
    }
  g_busy = 0;
  return 1;
}

int appnet_udp_bind(u32 port) {
  u32 st;
  g_err[0] = 0;
  if (!appnet_ready())
    return 0;
  if (ctrl->version < AUDIO_UDP_VERSION) {
    g_last_st = WIFI_NETS_NOLINK;
    err("UDP needs a newer AuroraOS", 0, 0);
    return 0;
  }
  wifi_udp_setup(port);
  if (!udp_op(WIFI_NETOP_UDP_BIND))
    return 0;
  st = io->status;
  if (st == WIFI_NETS_TOOBIG) {
    g_last_st = st;
    err("that UDP port cannot be used", 0, 0);
    return 0;
  }
  if (st != WIFI_NETS_OK) {
    net_err(st, 0);
    return 0;
  }
  return 1;
}

void appnet_udp_unbind(void) {
  if (appnet_ready() && ctrl->version >= AUDIO_UDP_VERSION)
    udp_op(WIFI_NETOP_UDP_UNBIND);
}

void appnet_tcp_release(void) { g_tcp_held = 0; }

u32 appnet_last_status(void) { return g_last_st; }
