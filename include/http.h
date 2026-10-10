#ifndef AURORA_HTTP_H
#define AURORA_HTTP_H

#include "aurora.h"

/* HTTP/1.0 exchanges with the account server's console host (ACCOUNT_HOST,
 * account.h) over the joined Wi-Fi network: one WIFI_NETOP_HTTP each, so a
 * reply has to fit the core's buffer (WIFI_HTTP_RESP_MAX), headers included.
 * Larger bodies are fetched in byte ranges. See docs/account.md and
 * docs/store.md. */

typedef struct {
  u32 net;       /* WIFI_NETS_* */
  u32 stage;     /* WIFI_HTTP_* */
  int status;    /* HTTP status, 0 when no reply was read */
  char *body;    /* in the caller's buffer, NUL-terminated; 0 without one */
  u32 len;       /* body bytes (Content-Length when that is shorter) */
  u32 from;      /* Content-Range: the first byte and the whole length; */
  u32 total;     /* total is 0 without the header */
  char etag[24]; /* "" without one */
} HttpReply;

/* 1 while the console is on a network the core keeps up. When it is not,
 * the next request looks the host up again. */
int http_online(void);

/* One request: `method` `path` with the Authorization of `token` when it is
 * not 0, the `extra` header lines (each ending in \r\n) and a form body. The
 * reply goes into `buf` (`max` bytes and one more for the NUL), or with `buf`
 * 0 stays in the core's buffer until the next request. Looks the
 * host up first when needed. Returns the HTTP status, 0 when no reply came
 * (r->net says why). `tick` is called while the core works, with the screens
 * drawn directly (wifi_direct_on). */
int http_call(const char *method, const char *path, const char *token,
              const char *extra, const char *form, char *buf, u32 max,
              HttpReply *r, void (*tick)(u32 ms));

/* A short reason for a failed exchange: "HTTP 404", or what stopped the
 * network. */
void http_why(const HttpReply *r, char *out);

#endif
