/* SPDX-License-Identifier: GPL-2.0 */
/* LICENSE: GPL-2.0, part of the Wi-Fi driver. See docs/wifi.md "License and
 * credits". */
#ifndef AURORA_APPNET_H
#define AURORA_APPNET_H

#include "aurora.h"

/* The network for apps (src/os/AppNet.c), linked by the C SDK and the Auric
 * runtime: joining the network saved in Settings > Wi-Fi, DNS, ping, one TCP
 * exchange, and HTTP over it. Every call posts to the ARM11 core and waits
 * until it is done; a failure leaves a one-line reason in appnet_error(). */

/* The first core that keeps replies at WIFI_HTTP_RESP. */
#define APPNET_CORE_VERSION 115u
/* A request's bound; the core allows at most 20 s. */
#define APPNET_TIMEOUT_MS 15000u
#define APPNET_URL_MAX 1024u

/* The runtime that links AppNet.c defines these three. */
/* A millisecond clock. */
u32 appnet_ms(void);
/* The SD card mounted as FatFs's default drive, and left mounted; 1 when it
 * is. */
int appnet_mount(void);
/* Called about every 100 ms while the core works on a call, with the
 * milliseconds waited. The core is busy meanwhile: nothing may be posted to
 * it (presents, sounds). */
void appnet_waiting(u32 ms);

/* Launched from the Home Menu, with a core that runs these calls; else 0 and
 * the reason. */
int appnet_ready(void);
/* 1 while a call the core has not finished is outstanding. */
int appnet_busy(void);
/* Waits for it, without calling appnet_waiting(); before leaving the app. */
void appnet_finish(void);
const char *appnet_error(void);

/* Joins the saved network unless the core is already on one: about 25 s. */
int appnet_join(void);
/* Asks the core whether the link is up. */
int appnet_online(void);
/* The console's address while joined, else 0. a.b.c.d is a << 24 | ... | d. */
u32 appnet_ip(void);
/* "a.b.c.d", at most 16 bytes. */
void appnet_ip_text(u32 ip, char *out);
/* A host name or "a.b.c.d"; names are kept for later calls. */
int appnet_resolve(const char *host, u32 *ip);
int appnet_ping(u32 ip, u32 *rtt_us);

/* `data` (at most WIFI_HTTP_REQ_MAX bytes) goes to ip:port over TCP, and what
 * comes back until the server closes the connection stays at WIFI_HTTP_RESP
 * until the next call: *got bytes, with a 0 byte after them. */
int appnet_exchange(u32 ip, u32 port, const void *data, u32 len, u32 *got);

typedef struct {
  int status;     /* e.g. 200; 0 when no HTTP reply came */
  char *head;     /* the status line and headers, "\r\n" between them */
  char *body;     /* with a 0 byte after it */
  u32 len;
  u32 from, total; /* a 206's Content-Range: its first byte, the file size */
} AppNetReply;

/* One HTTP/1.0 request to an http:// URL, redirects followed (up to five).
 * `headers` (may be 0) are extra lines; a body gets its Content-Length. The
 * reply is read in place at WIFI_HTTP_RESP and stays there until the next
 * call. Returns the status, or 0 when no reply came. A redirect that cannot
 * be followed (to https://, or a sixth) is returned as it is, with the reason
 * in appnet_error(). */
int appnet_http(const char *method, const char *url, const char *headers,
                const void *body, u32 blen, AppNetReply *r);
/* Header `name` (no colon, any case) of a reply, copied; 0 if it has none. */
int appnet_header(const AppNetReply *r, const char *name, char *out, u32 max);
/* Percent-encodes `s` for a URL query or a form; returns the length. */
u32 appnet_url_encode(char *out, u32 max, const char *s);
/* GETs `url` into the file `path` (a FatFs path), in Range pieces when the
 * server sends them, through `path`.part; `headers` (may be 0) go with each
 * piece. `progress` (may be 0) runs after each piece; `total` is 0 while
 * unknown. */
int appnet_download(const char *url, const char *headers, const char *path,
                    void (*progress)(u32 done, u32 total));

#endif
