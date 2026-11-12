/* SPDX-License-Identifier: GPL-2.0 */
/* LICENSE: GPL-2.0, part of the Wi-Fi driver. See docs/wifi.md "License and
 * credits". */
#ifndef AURORA_APPNET_H
#define AURORA_APPNET_H

#include "aurora.h"

/* The network for apps (src/os/AppNet.c), linked by the C SDK and the Auric
 * runtime, and by the OS for the Terminal: joining the network saved in
 * Settings > Wi-Fi, DNS, ping, one TCP exchange, and HTTP and HTTPS over it.
 * Every call posts to the ARM11 core and waits until it is done; a failure
 * leaves a one-line reason in appnet_error(). */

/* The first core that keeps replies at WIFI_HTTP_RESP. */
#define APPNET_CORE_VERSION 115u
/* A request's bound; the core allows at most 20 s. */
#define APPNET_TIMEOUT_MS 15000u
#define APPNET_URL_MAX 2048u

/* The runtime that links AppNet.c defines these four. */
u32 appnet_ms(void);
/* The SD card mounted as FatFs's default drive, and left mounted; 1 when it
 * is. */
int appnet_mount(void);
/* Called about every 100 ms while the core works on a call, with the
 * milliseconds waited. The core is busy meanwhile: nothing may be posted to
 * it (presents, sounds). */
void appnet_waiting(u32 ms);
/* 1 when a core that runs these calls is up: an app started from the Home
 * Menu, or the OS itself. */
int appnet_launched(void);

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

/* One HTTP/1.0 request to an http:// or https:// URL (https:// over TLS,
 * Tls.c, with a core from Beta 7 on), redirects followed (up to five).
 * `headers` (may be 0) are extra lines; a body gets its Content-Length. The
 * reply is read in place at WIFI_HTTP_RESP and stays there until the next
 * call. Returns the status, or 0 when no reply came. A redirect that cannot
 * be followed (a sixth, or to https:// on an older core) is returned as it
 * is, with the reason in appnet_error(). Authorization and Cookie lines in
 * `headers` go only to the first URL's host, port and scheme. */
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

/* A stream: GETs over one connection that stays open between them, with the
 * core keeping it going in the background (a core from AUDIO_TCP_BG_VERSION on),
 * so the app can present frames and start sounds meanwhile. One at a time; while
 * it is open no other network call works, so names are resolved as it opens and
 * a change of host closes it first. Nothing here waits for the network. */
enum {
  APPNET_STREAM_FAILED = -1, /* appnet_error() says why */
  APPNET_STREAM_IDLE = 0,    /* no request */
  APPNET_STREAM_WAITING,     /* connecting, or the reply has not begun */
  APPNET_STREAM_BODY,        /* status and length known; read the body */
  APPNET_STREAM_DONE,        /* every byte of this reply has been read */
};

/* Asks for `url`, http:// or https://; `headers` (may be 0) are extra lines, each
 * ended with CR LF, at most 511 bytes.
 * Redirects are followed by the next poll, without the Authorization and Cookie
 * lines once the host, port or TLS changes. Only blocks to look up a new host's
 * name or to end a connection to another one. 0 with appnet_error() on failure. */
int appnet_stream_get(const char *url, const char *headers);
/* Pumps the connection; call it often, a request or not. APPNET_STREAM_*. */
int appnet_stream_poll(void);
/* Body bytes of the current reply, at most `max`; 0 when none has arrived. With no
 * Content-Length the reply ends when the server closes; a chunked one fails. 15 s
 * with nothing read counts as the server having stopped. */
u32 appnet_stream_read(void *buf, u32 max);
int appnet_stream_status(void);
/* The address the last request ended up at, after any redirects. */
const char *appnet_stream_url(void);
/* The Content-Length of the current reply, 0 until known or when it has none. */
u32 appnet_stream_length(void);
/* Whether the last request's redirects left its host, port or TLS. */
int appnet_stream_moved(void);
void appnet_stream_close(void);

/* A plain TCP connection for the C SDK's sockets (WIFI_NETOP_TCP_BG, a core
 * from AUDIO_TCP_BG_VERSION on), with bytes moved by wifi_tcp_*. 1 once the
 * core has taken it: wifi_tcp_state() then says how it goes, and
 * appnet_tcp_status() why it ended. The caller holds the rings from here until
 * appnet_tcp_release(); until then every other call here fails, except a link
 * check and, from AUDIO_UDP_VERSION, a name lookup and the UDP port below. Not
 * while a stream is open. */
int appnet_tcp_open(u32 ip, u32 port, u32 timeout_ms);
int appnet_tcp_listen(u32 port);
/* Two independent connections. Select the same channel before wifi_tcp_* IO.
 * The second channel requires core 127. A port of zero is invalid. */
int appnet_tcp_open_channel(u32 channel, u32 ip, u32 port, u32 timeout_ms);
int appnet_tcp_listen_channel(u32 channel, u32 port);
void appnet_tcp_release_channel(u32 channel);
/* The connection's WIFI_NETS_* once wifi_tcp_state() is WIFI_TCP_CLOSED. */
u32 appnet_tcp_status(void);
void appnet_tcp_release(void);

/* The core's one UDP port for the C SDK's datagram sockets
 * (WIFI_NETOP_UDP_BIND), with datagrams moved by wifi_udp_*; `port` 0 lets
 * the core pick one, which wifi_udp_port() then says. Binding again moves
 * it. */
int appnet_udp_bind(u32 port);
void appnet_udp_unbind(void);
/* The running core's AUDIO_CORE_VERSION; 0 with no core. */
u32 appnet_core_version(void);
/* The WIFI_NETS_* of the last call that failed. */
u32 appnet_last_status(void);

#endif
