#ifndef AURORA_TLS_H
#define AURORA_TLS_H

#include "aurora.h"

/* HTTPS for the OS and apps: a TLS 1.2 client (BearSSL, lib/bearssl) on the
 * ARM9, over a WIFI_NETOP_TCP connection the ARM11 core keeps (core 121 and
 * later). Certificates are checked against the roots built in
 * (src/os/TlsRoots.c) and any on SD:/Aurora/certs. See docs/tls.md. */

#define TLS_CERT_DIR  "0:/Aurora/certs"
#define TLS_SEED_FILE "0:/Aurora/random.bin"

/* How the caller runs the core's operation. start posts AUDIO_CMD_WIFI_NET
 * WIFI_NETOP_TCP, after wifi_tcp_setup has filled the operands, and returns 1
 * if it went out; done returns 1 once the core has finished it (with
 * WifiNetIo fresh in memory); tick (may be 0) is called about every 100 ms
 * with the milliseconds since the exchange began; ms is a millisecond
 * clock. */
typedef struct {
  int (*start)(void);
  int (*done)(void);
  void (*tick)(u32 ms);
  u32 (*ms)(void);
} TlsLink;

/* Errors of our own, beside BearSSL's BR_ERR_* (bearssl_ssl.h). */
#define TLS_ERR_EOF  1000 /* the server closed before the handshake ended */
#define TLS_ERR_NAME 1001 /* the host name is empty or too long            */

/* One HTTP exchange over TLS with `host` (the name the certificate must
 * carry, also sent as SNI) at ip:port: `req` goes out, and the reply, until
 * the server closes or its Content-Length is in, comes back in `resp`: at
 * most `max` bytes, *got of them. `head_only` says the reply has no body
 * (a HEAD request). timeout_ms bounds each wait for the server. Returns
 * WIFI_NETS_*: WIFI_NETS_TLS when the TLS layer failed, with *err saying
 * why; *stage is how far it got (WIFI_HTTP_*). */
u32 tls_exchange(const TlsLink *l, const char *host, u32 ip, u32 port,
                 const void *req, u32 req_len, int head_only, char *resp,
                 u32 max, u32 *got, u32 *stage, u32 timeout_ms, int *err);

const char *tls_error_text(int err);
/* 1 when *err is about a certificate's dates, which a wrong clock causes. */
int tls_error_is_clock(int err);

/* Bytes that may carry some randomness (input timing, say), mixed into the
 * generator the handshakes draw on. */
void tls_entropy(const void *p, u32 n);

#endif
