/* The network: src/os/AppNet.c behind the net_ and http_ functions. A call
 * keeps the ARM11 core busy until it is done, and the core is what presents
 * frames and plays sounds; so while one runs nothing is posted to it, and a
 * net_on_wait() handler's frames are copied to framebuffer A by the CPU. */
#include "app_internal.h"
#include "appnet.h"
#include "crash.h"
#include "wifi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void (*g_wait_fn)(uint32_t ms);
static void (*g_progress)(size_t done, size_t total);
static bool g_in_call;
static bool g_home; /* HOME was pressed during the call */
static const char *g_err; /* the SDK's own reason, over AppNet's */

u32 appnet_ms(void) { return sys_millis(); }

int appnet_mount(void) { return app_fs_mount(); }

void appnet_waiting(u32 ms) {
  crash_poll_arm11();
  if (!g_home && app_from_home() && app_home_pressed())
    g_home = true;
  if (g_wait_fn)
    g_wait_fn(ms);
}

bool app_net_busy(void) { return g_in_call && appnet_busy(); }

void app_net_leave(void) {
  if (g_in_call)
    appnet_finish();
}

static bool begin(void) {
  if (g_in_call) {
    g_err = "a network call is already running";
    return false;
  }
  g_err = NULL;
  g_home = false;
  g_in_call = true;
  if (g_wait_fn)
    app_gfx_direct(true);
  return true;
}

/* HOME waits until the core is done, then leaves as it would have. */
static void end(void) {
  app_gfx_direct(false);
  g_in_call = false;
  if (g_home)
    exit(0);
}

const char *net_error(void) { return g_err ? g_err : appnet_error(); }

void net_on_wait(void (*fn)(uint32_t ms)) { g_wait_fn = fn; }

bool net_available(void) { return appnet_ready() != 0; }

bool net_connect(void) {
  bool ok;
  if (!begin())
    return false;
  ok = appnet_join() != 0;
  end();
  return ok;
}

bool net_connected(void) {
  bool ok;
  if (!begin())
    return false;
  ok = appnet_online() != 0;
  end();
  return ok;
}

const char *net_address(void) {
  static char text[16];
  u32 ip = appnet_ip();
  text[0] = 0;
  if (ip)
    appnet_ip_text(ip, text);
  return text;
}

bool net_resolve(const char *host, char ip_text[16]) {
  u32 ip = 0;
  bool ok;
  ip_text[0] = 0;
  if (!begin())
    return false;
  ok = appnet_resolve(host, &ip) != 0;
  end();
  if (ok)
    appnet_ip_text(ip, ip_text);
  return ok;
}

int net_ping(const char *host) {
  u32 ip = 0, rtt = 0;
  bool ok;
  if (!begin())
    return -1;
  ok = appnet_resolve(host, &ip) && appnet_ping(ip, &rtt);
  end();
  return ok ? (int)((rtt + 500u) / 1000u) : -1;
}

int net_tcp_exchange(const char *host, int port, const void *data,
                     size_t length, void *reply, size_t max) {
  u32 ip = 0, got = 0;
  bool ok;
  if (!begin())
    return -1;
  ok = appnet_resolve(host, &ip) &&
       appnet_exchange(ip, port > 0 ? (u32)port : 0u, data, (u32)length,
                       &got);
  if (ok && reply) {
    if (got > max)
      got = (u32)max;
    memcpy(reply, (const void *)WIFI_HTTP_RESP, got);
  }
  end();
  return ok ? (int)got : -1;
}

/* One block: the struct, the headers and the body, so http_free() is free(). */
static HttpResponse *keep(const AppNetReply *a) {
  size_t hl = strlen(a->head), bl = a->len;
  HttpResponse *r = malloc(sizeof(*r) + hl + bl + 2u);
  if (!r) {
    g_err = "not enough memory for the reply";
    return NULL;
  }
  r->status = a->status;
  r->headers = (char *)(r + 1);
  memcpy(r->headers, a->head, hl + 1u);
  r->body = r->headers + hl + 1u;
  memcpy(r->body, a->body, bl);
  r->body[bl] = 0;
  r->length = bl;
  return r;
}

HttpResponse *http_request(const char *method, const char *url,
                           const char *headers, const void *data,
                           size_t length) {
  AppNetReply a;
  HttpResponse *r = NULL;
  if (!begin())
    return NULL;
  if (appnet_http(method ? method : "GET", url, headers, data, (u32)length,
                  &a))
    r = keep(&a);
  end();
  return r;
}

HttpResponse *http_get(const char *url) {
  return http_request("GET", url, NULL, NULL, 0);
}

HttpResponse *http_post(const char *url, const char *content_type,
                        const void *data, size_t length) {
  char head[256];
  snprintf(head, sizeof(head), "Content-Type: %s\r\n",
           content_type ? content_type : "application/x-www-form-urlencoded");
  return http_request("POST", url, head, data, length);
}

bool http_header(const HttpResponse *r, const char *name, char *value,
                 size_t max) {
  AppNetReply a;
  if (!r || !max)
    return false;
  a.head = r->headers;
  return appnet_header(&a, name, value, (u32)max) != 0;
}

void http_free(HttpResponse *r) { free(r); }

size_t http_url_encode(char *out, size_t max, const char *text) {
  return appnet_url_encode(out, (u32)max, text);
}

static void progress(u32 done, u32 total) {
  if (g_progress)
    g_progress(done, total);
}

bool http_download(const char *url, const char *path,
                   void (*fn)(size_t done, size_t total)) {
  char fpath[FS_NAME_MAX + 8];
  bool ok;
  if (!app_fs_path(path, fpath, sizeof(fpath))) {
    g_err = "the file name is not valid";
    return false;
  }
  if (!begin())
    return false;
  g_progress = fn;
  ok = appnet_download(url, NULL, fpath, progress) != 0;
  g_progress = NULL;
  end();
  return ok;
}
