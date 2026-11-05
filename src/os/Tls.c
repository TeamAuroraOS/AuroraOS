/* HTTPS: BearSSL's TLS 1.2 client on the ARM9, over the core's
 * WIFI_NETOP_TCP rings (WiFiTcp.c). See include/tls.h and docs/tls.md.
 * BearSSL is by Thomas Pornin, MIT license (lib/bearssl/LICENSE.txt). */
#include <string.h>
#include "bearssl.h"
#include "ff.h"
#include "power.h"
#include "timer.h"
#include "tls.h"
#include "wifi.h"

#define ROOTS_MAX   256
#define ARENA_SIZE  (128u * 1024u)
#define SESSIONS    4
#define HOST_MAX    64
#define END_WAIT_MS 5000u
/* ARM9-only; Boot9 and Process9 seed their generators from these. */
#define PRNG_REGS   0x10011000u

extern const br_x509_trust_anchor tls_roots[];
extern const unsigned tls_roots_count;

static br_ssl_client_context cc;
static br_x509_minimal_context xc;
static unsigned char iobuf[BR_SSL_BUFSIZE_BIDI];

/* The built-in roots, then those read from the card; the card's names and
 * keys are kept in the arena. */
static br_x509_trust_anchor anchors[ROOTS_MAX];
static u32 n_anchors;
static u8 arena[ARENA_SIZE];
static u32 arena_used;
static int ready;

static br_hmac_drbg_context drbg;
static br_sha256_context pool;
static int pool_on;

static struct {
  char host[HOST_MAX];
  br_ssl_session_parameters p;
} sessions[SESSIONS];
static u32 session_next;

/* TLS 1.2 only, with forward secrecy first, ChaCha20 (fast without AES
 * hardware) before AES-GCM, and plain RSA key exchange and CBC last, for old
 * servers. */
static const uint16_t suites[] = {
    BR_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256,
    BR_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,
    BR_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
    BR_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
    BR_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,
    BR_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,
    BR_TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA256,
    BR_TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA256,
    BR_TLS_ECDHE_ECDSA_WITH_AES_256_CBC_SHA384,
    BR_TLS_ECDHE_RSA_WITH_AES_256_CBC_SHA384,
    BR_TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA,
    BR_TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA,
    BR_TLS_ECDHE_ECDSA_WITH_AES_256_CBC_SHA,
    BR_TLS_ECDHE_RSA_WITH_AES_256_CBC_SHA,
    BR_TLS_RSA_WITH_AES_128_GCM_SHA256,
    BR_TLS_RSA_WITH_AES_256_GCM_SHA384,
    BR_TLS_RSA_WITH_AES_128_CBC_SHA256,
    BR_TLS_RSA_WITH_AES_256_CBC_SHA256,
    BR_TLS_RSA_WITH_AES_128_CBC_SHA,
    BR_TLS_RSA_WITH_AES_256_CBC_SHA,
};

static const br_hash_class *const hashes[] = {
    &br_sha1_vtable, &br_sha224_vtable, &br_sha256_vtable,
    &br_sha384_vtable, &br_sha512_vtable,
};

static void client_init(void) {
  br_ssl_engine_context *eng = &cc.eng;
  br_ssl_client_zero(&cc);
  br_ssl_engine_set_versions(eng, BR_TLS12, BR_TLS12);
  br_x509_minimal_init(&xc, &br_sha256_vtable, anchors, n_anchors);
  br_ssl_engine_set_suites(eng, suites, sizeof(suites) / sizeof(suites[0]));
  br_ssl_client_set_default_rsapub(&cc);
  br_ssl_engine_set_default_rsavrfy(eng);
  br_ssl_engine_set_default_ecdsa(eng);
  br_x509_minimal_set_rsa(&xc, br_ssl_engine_get_rsavrfy(eng));
  br_x509_minimal_set_ecdsa(&xc, br_ssl_engine_get_ec(eng),
                            br_ssl_engine_get_ecdsa(eng));
  for (int id = br_sha1_ID; id <= br_sha512_ID; id++) {
    br_ssl_engine_set_hash(eng, id, hashes[id - br_sha1_ID]);
    br_x509_minimal_set_hash(&xc, id, hashes[id - br_sha1_ID]);
  }
  br_ssl_engine_set_x509(eng, &xc.vtable);
  br_ssl_engine_set_prf_sha256(eng, &br_tls12_sha256_prf);
  br_ssl_engine_set_prf_sha384(eng, &br_tls12_sha384_prf);
  br_ssl_engine_set_default_aes_cbc(eng);
  br_ssl_engine_set_default_aes_gcm(eng);
  br_ssl_engine_set_default_chapol(eng);
  br_ssl_engine_set_buffer(eng, iobuf, sizeof(iobuf), 1);
}

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static int same_ci(const char *a, const char *b) {
  while (*a && lower(*a) == lower(*b)) {
    a++;
    b++;
  }
  return *a == *b;
}

static void pool_add(const void *p, u32 n) {
  if (!pool_on) {
    br_sha256_init(&pool);
    pool_on = 1;
  }
  br_sha256_update(&pool, p, n);
}

void tls_entropy(const void *p, u32 n) { pool_add(p, n); }

/* Whatever the moment holds: the PRNG block, the free-running timers, the
 * clock, and the Wi-Fi state, whose counters and timings move with the
 * network. */
static void gather(br_sha256_context *h) {
  u32 v[13];
  RtcTime rt;
  for (u32 i = 0; i < 8u; i++)
    v[i] = *(volatile u32 *)(PRNG_REGS + 4u * i);
  v[8] = timer_ticks();
  for (u32 i = 0; i < 4u; i++)
    v[9 + i] = *(volatile u16 *)(0x10003000u + 4u * i);
  br_sha256_update(h, v, sizeof(v));
  if (rtc_read(&rt))
    br_sha256_update(h, &rt, sizeof(rt));
  br_sha256_update(h, (const void *)WIFI_SHARED_ADDR, sizeof(WifiShared));
}

static void seed(const u8 *file, u32 n) {
  br_sha256_context h;
  u8 s[32];
  br_sha256_init(&h);
  gather(&h);
  br_sha256_update(&h, file, n);
  pool_add(s, 0);
  br_sha256_out(&pool, s);
  br_sha256_update(&h, s, sizeof(s));
  br_sha256_out(&h, s);
  br_hmac_drbg_init(&drbg, &br_sha256_vtable, s, sizeof(s));
  memset(s, 0, sizeof(s));
}

/* 32 bytes for a handshake, after stirring in this moment. */
static void fresh(u8 *out) {
  br_sha256_context h;
  u8 s[32];
  br_sha256_init(&h);
  gather(&h);
  pool_add(s, 0);
  br_sha256_out(&pool, s);
  br_sha256_update(&h, s, sizeof(s));
  br_sha256_out(&h, s);
  br_hmac_drbg_update(&drbg, s, sizeof(s));
  br_hmac_drbg_generate(&drbg, out, 32);
  memset(s, 0, sizeof(s));
}

typedef struct {
  u8 *p;
  u32 len, cap;
  int over;
} Dn;

static br_x509_decoder_context xd;
static br_pem_decoder_context pem;
static Dn dn;

static void dn_add(void *ctx, const void *b, size_t n) {
  Dn *d = (Dn *)ctx;
  if (d->over || n > d->cap - d->len) {
    d->over = 1;
    return;
  }
  memcpy(d->p + d->len, b, n);
  d->len += n;
}

static void cert_begin(void) {
  dn.p = arena + arena_used;
  dn.len = 0;
  dn.cap = ARENA_SIZE - arena_used;
  dn.over = 0;
  br_x509_decoder_init(&xd, dn_add, &dn);
}

static void cert_push(void *ctx, const void *b, size_t n) {
  (void)ctx;
  br_x509_decoder_push(&xd, b, n);
}

static u8 *keep(const void *p, u32 n) {
  u8 *d = arena + arena_used;
  if (n > ARENA_SIZE - arena_used)
    return 0;
  memcpy(d, p, n);
  arena_used += n;
  return d;
}

/* A certificate from the card becomes an anchor: a CA's may sign others, any
 * other is trusted only as the server's own (a self-signed server). */
static void cert_end(void) {
  br_x509_pkey *k = br_x509_decoder_get_pkey(&xd);
  br_x509_trust_anchor *ta = &anchors[n_anchors];
  u32 mark = arena_used;
  if (!k || dn.over || !dn.len || n_anchors >= ROOTS_MAX)
    return;
  arena_used += dn.len;
  ta->dn.data = dn.p;
  ta->dn.len = dn.len;
  ta->flags = br_x509_decoder_isCA(&xd) ? BR_X509_TA_CA : 0;
  ta->pkey.key_type = k->key_type;
  if (k->key_type == BR_KEYTYPE_RSA) {
    ta->pkey.key.rsa.n = keep(k->key.rsa.n, k->key.rsa.nlen);
    ta->pkey.key.rsa.nlen = k->key.rsa.nlen;
    ta->pkey.key.rsa.e = keep(k->key.rsa.e, k->key.rsa.elen);
    ta->pkey.key.rsa.elen = k->key.rsa.elen;
    if (!ta->pkey.key.rsa.n || !ta->pkey.key.rsa.e) {
      arena_used = mark;
      return;
    }
  } else if (k->key_type == BR_KEYTYPE_EC) {
    ta->pkey.key.ec.curve = k->key.ec.curve;
    ta->pkey.key.ec.q = keep(k->key.ec.q, k->key.ec.qlen);
    ta->pkey.key.ec.qlen = k->key.ec.qlen;
    if (!ta->pkey.key.ec.q) {
      arena_used = mark;
      return;
    }
  } else {
    arena_used = mark;
    return;
  }
  n_anchors++;
}

static int is_cert_name(const char *s) {
  u32 n = (u32)strlen(s);
  if (n < 5 || s[n - 4] != '.')
    return 0;
  s += n - 3;
  return same_ci(s, "pem") || same_ci(s, "crt") || same_ci(s, "cer") ||
         same_ci(s, "der");
}

/* PEM (any number of certificates, other objects skipped) or one DER
 * certificate, read in pieces. */
static void load_file(const char *path) {
  static FIL f;
  static u8 buf[2048];
  UINT got = 0;
  int der = -1, in_cert = 0;
  if (f_open(&f, path, FA_READ) != FR_OK)
    return;
  br_pem_decoder_init(&pem);
  while (f_read(&f, buf, sizeof(buf), &got) == FR_OK && got) {
    const u8 *p = buf;
    size_t left = got;
    if (der < 0) {
      der = buf[0] == 0x30;
      if (der)
        cert_begin();
    }
    if (der) {
      br_x509_decoder_push(&xd, buf, got);
      continue;
    }
    while (left) {
      size_t k = br_pem_decoder_push(&pem, p, left);
      p += k;
      left -= k;
      switch (br_pem_decoder_event(&pem)) {
        case BR_PEM_BEGIN_OBJ: {
          const char *name = br_pem_decoder_name(&pem);
          in_cert = same_ci(name, "CERTIFICATE") ||
                    same_ci(name, "X509 CERTIFICATE");
          if (in_cert)
            cert_begin();
          br_pem_decoder_setdest(&pem, in_cert ? cert_push : 0, 0);
          break;
        }
        case BR_PEM_END_OBJ:
          if (in_cert)
            cert_end();
          in_cert = 0;
          break;
        case BR_PEM_ERROR:
          f_close(&f);
          return;
        default:
          break;
      }
    }
  }
  if (der == 1)
    cert_end();
  f_close(&f);
}

static void load_dir(void) {
  static DIR d;
  static FILINFO fi;
  static char path[sizeof(TLS_CERT_DIR) + 1 + sizeof(fi.fname)];
  if (f_opendir(&d, TLS_CERT_DIR) != FR_OK)
    return;
  while (f_readdir(&d, &fi) == FR_OK && fi.fname[0]) {
    u32 n = sizeof(TLS_CERT_DIR) - 1u;
    if ((fi.fattrib & AM_DIR) || !is_cert_name(fi.fname))
      continue;
    memcpy(path, TLS_CERT_DIR "/", n + 1u);
    memcpy(path + n + 1u, fi.fname, strlen(fi.fname) + 1u);
    load_file(path);
  }
  f_closedir(&d);
}

/* Once per run of the OS or an app: the anchors, and the generator seeded
 * with what the last run left in the seed file, which then gets new bytes.
 * Uses the card as the caller has it mounted, or mounts it briefly. */
static void setup(void) {
  static FATFS fs;
  static DIR probe;
  static FIL f;
  u8 s[32];
  UINT n = 0;
  int own = 0, card;
  FRESULT r;
  if (ready)
    return;
  ready = 1;
  for (u32 i = 0; i < tls_roots_count && n_anchors < ROOTS_MAX; i++)
    anchors[n_anchors++] = tls_roots[i];

  r = f_opendir(&probe, "0:/");
  if (r == FR_NOT_ENABLED) {
    own = 1;
    r = f_mount(&fs, "0:", 1);
  } else if (r == FR_OK) {
    f_closedir(&probe);
  }
  card = r == FR_OK;
  if (card) {
    load_dir();
    if (f_open(&f, TLS_SEED_FILE, FA_READ) == FR_OK) {
      if (f_read(&f, s, sizeof(s), &n) != FR_OK)
        n = 0;
      f_close(&f);
    }
  }
  seed(s, n);
  if (card && f_open(&f, TLS_SEED_FILE, FA_WRITE | FA_CREATE_ALWAYS) == FR_OK) {
    br_hmac_drbg_generate(&drbg, s, sizeof(s));
    f_write(&f, s, sizeof(s), &n);
    f_close(&f);
  }
  memset(s, 0, sizeof(s));
  if (own)
    f_mount(NULL, "0:", 0);
}

/* Days since 1 January of year 0 (proleptic Gregorian), as BearSSL counts. */
static u32 civil_days(u32 y, u32 m, u32 d) {
  u32 era, yoe, doy;
  if (m <= 2u)
    y--;
  era = y / 400u;
  yoe = y - era * 400u;
  doy = (153u * (m > 2u ? m - 3u : m + 9u) + 2u) / 5u + d - 1u;
  return era * 146097u + yoe * 365u + yoe / 4u - yoe / 100u + doy + 60u;
}

/* The clock, but never before AURORA_BUILD_DATE: a console whose clock was
 * never set still checks dates against a real lower bound. */
static void set_time(void) {
  u32 floor = civil_days(AURORA_BUILD_DATE / 10000u,
                         AURORA_BUILD_DATE / 100u % 100u,
                         AURORA_BUILD_DATE % 100u);
  u32 days = floor, secs = 0;
  RtcTime t;
  if (rtc_read(&t) && t.year >= 2000) {
    u32 d = civil_days((u32)t.year, (u32)t.month, (u32)t.day);
    if (d > floor) {
      days = d;
      secs = (u32)(t.hour * 3600 + t.min * 60 + t.sec);
    }
  }
  br_x509_minimal_set_time(&xc, days, secs);
}

static int session_find(const char *host) {
  for (u32 i = 0; i < SESSIONS; i++)
    if (sessions[i].host[0] && same_ci(sessions[i].host, host))
      return (int)i;
  return -1;
}

static void session_drop(const char *host) {
  int i = session_find(host);
  if (i >= 0)
    memset(&sessions[i], 0, sizeof(sessions[i]));
}

static void session_keep(const char *host) {
  int i = session_find(host);
  if (i < 0)
    i = (int)(session_next++ % SESSIONS);
  memcpy(sessions[i].host, host, strlen(host) + 1u);
  br_ssl_engine_get_session_parameters(&cc.eng, &sessions[i].p);
  if (!sessions[i].p.session_id_len)
    memset(&sessions[i], 0, sizeof(sessions[i]));
}

/* An idle pass waits for the next millisecond on the timer, off FCRAM,
 * which the core runs from uncached. */
static void idle(const TlsLink *l) {
  u32 t = l->ms();
  while (l->ms() == t)
    ;
}

/* How much of the reply is in. HTTP_SHORT: the headers promise more body than
 * has come, so a connection that ends now has cut it off. */
enum { HTTP_MORE, HTTP_DONE, HTTP_SHORT };

/* The reply's headers and as much body as their Content-Length says: none
 * for a HEAD, a 204 or a 304. Interim replies are skipped. */
static int http_complete(const char *b, u32 len, int head_only) {
  static const char cl[] = "content-length:";
  u32 start = 0, end, code;
  for (;;) {
    end = code = 0;
    for (u32 i = start; i + 3u < len; i++)
      if (b[i] == '\r' && b[i + 1] == '\n' && b[i + 2] == '\r' &&
          b[i + 3] == '\n') {
        end = i + 4u;
        break;
      }
    if (!end)
      return HTTP_MORE;
    for (u32 i = start; i < end && b[i] != '\r'; i++)
      if (b[i] == ' ') {
        for (u32 k = 1; k <= 3u && i + k < end; k++)
          code = code * 10u + (u32)(b[i + k] - '0');
        break;
      }
    if (code >= 200u)
      break;
    start = end;
  }
  if (head_only || code == 204u || code == 304u)
    return HTTP_DONE;
  for (u32 i = start + 1u; i + sizeof(cl) < end; i++) {
    u32 k = 0, v = 0, digits = 0;
    if (b[i - 1] != '\n')
      continue;
    while (cl[k] && lower(b[i + k]) == cl[k])
      k++;
    if (cl[k])
      continue;
    for (i += k; i < end && (b[i] == ' ' || b[i] == '\t'); i++)
      ;
    for (; i < end && b[i] >= '0' && b[i] <= '9' && v < 100000000u; i++) {
      v = v * 10u + (u32)(b[i] - '0');
      digits++;
    }
    if (!digits)
      return HTTP_MORE;
    return len - end >= v ? HTTP_DONE : HTTP_SHORT;
  }
  return HTTP_MORE;
}

u32 tls_exchange(const TlsLink *l, const char *host, u32 ip, u32 port,
                 const void *req, u32 req_len, int head_only, char *resp,
                 u32 max, u32 *got, u32 *stage, u32 timeout_ms, int *err) {
  br_ssl_engine_context *eng = &cc.eng;
  const u8 *q = (const u8 *)req;
  u32 sent = 0, t0, last, now, shown = 0xFFFFFFFFu, status = WIFI_NETS_TIMEOUT;
  u32 hl = (u32)strlen(host);
  int resume, closing = 0, hs = 0, ended = 0, last_look = 0;
  u8 rnd[32];
  *got = 0;
  *stage = 0;
  *err = 0;
  if (!hl || hl >= HOST_MAX) {
    *err = TLS_ERR_NAME;
    return WIFI_NETS_TLS;
  }
  setup();
  client_init();
  set_time();
  fresh(rnd);
  br_ssl_engine_inject_entropy(eng, rnd, sizeof(rnd));
  memset(rnd, 0, sizeof(rnd));
  resume = session_find(host);
  if (resume >= 0)
    br_ssl_engine_set_session_parameters(eng, &sessions[resume].p);
  if (!br_ssl_client_reset(&cc, host, resume >= 0)) {
    *err = br_ssl_engine_last_error(eng);
    return WIFI_NETS_TLS;
  }

  wifi_tcp_setup(ip, port, timeout_ms);
  *stage = WIFI_HTTP_CONNECT;
  if (!l->start())
    return WIFI_NETS_BUSY;
  t0 = last = l->ms();
  for (;;) {
    unsigned st = br_ssl_engine_current_state(eng);
    int moved = 0;
    size_t len;
    u8 *b;
    if (st & BR_SSL_CLOSED) {
      *err = br_ssl_engine_last_error(eng);
      if (!*err)
        status = hs ? WIFI_NETS_OK : WIFI_NETS_TLS;
      if (!*err && !hs)
        *err = TLS_ERR_EOF;
      break;
    }
    if (st & BR_SSL_SENDREC) {
      b = br_ssl_engine_sendrec_buf(eng, &len);
      len = wifi_tcp_write(b, (u32)len);
      if (len) {
        br_ssl_engine_sendrec_ack(eng, len);
        moved = 1;
      }
    } else if (closing) {
      status = WIFI_NETS_OK; /* the close_notify is out */
      break;
    }
    if (st & BR_SSL_RECVAPP) {
      b = br_ssl_engine_recvapp_buf(eng, &len);
      if (len > max - *got) {
        status = WIFI_NETS_TOOBIG;
        break;
      }
      memcpy(resp + *got, b, len);
      *got += (u32)len;
      br_ssl_engine_recvapp_ack(eng, len);
      moved = 1;
      if (!closing && http_complete(resp, *got, head_only) == HTTP_DONE) {
        *stage = WIFI_HTTP_DONE;
        br_ssl_engine_close(eng);
        closing = 1;
      }
    }
    if (st & BR_SSL_SENDAPP) {
      hs = 1;
      if (sent < req_len && !closing) {
        b = br_ssl_engine_sendapp_buf(eng, &len);
        if (len > req_len - sent)
          len = req_len - sent;
        memcpy(b, q + sent, len);
        br_ssl_engine_sendapp_ack(eng, len);
        sent += (u32)len;
        if (sent == req_len)
          br_ssl_engine_flush(eng, 0);
        moved = 1;
      }
    }
    if (st & BR_SSL_RECVREC) {
      b = br_ssl_engine_recvrec_buf(eng, &len);
      len = wifi_tcp_read(b, (u32)len);
      if (len) {
        br_ssl_engine_recvrec_ack(eng, len);
        moved = 1;
      } else if (wifi_tcp_eof()) {
        /* No close_notify: a reply that ends with the connection, unless its
         * own headers say it was cut off, which an attacker can also do. */
        status = !hs ? WIFI_NETS_TLS
                 : http_complete(resp, *got, head_only) == HTTP_SHORT
                     ? WIFI_NETS_TIMEOUT
                     : WIFI_NETS_OK;
        if (!hs)
          *err = TLS_ERR_EOF;
        break;
      }
    }
    if (*stage < WIFI_HTTP_DONE && wifi_tcp_state() == WIFI_TCP_OPEN)
      *stage = hs && sent == req_len ? WIFI_HTTP_WAIT : WIFI_HTTP_SEND;
    /* Only an idle pass asks: done() flushes the whole cache. The core may
     * have put its last bytes in just after this pass looked, so it takes one
     * more pass that finds nothing to end. */
    if (!moved && l->done()) {
      if (last_look) {
        ended = 1;
        break;
      }
      last_look = 1;
      continue;
    }
    now = l->ms();
    if (moved) {
      last = now;
    } else if (now - last >= timeout_ms) {
      status = WIFI_NETS_TIMEOUT;
      break;
    }
    if (l->tick && (now - t0) / 100u != shown) {
      shown = (now - t0) / 100u;
      l->tick(now - t0);
    }
    wifi_tcp_alive();
    if (!moved)
      idle(l);
  }

  if (!ended) {
    wifi_tcp_close(status == WIFI_NETS_OK ? WIFI_TCP_FIN : WIFI_TCP_ABORT);
    for (u32 w0 = l->ms(); !l->done();) {
      now = l->ms();
      if (now - w0 >= END_WAIT_MS)
        return WIFI_NETS_BUSY;
      if (l->tick && (now - t0) / 100u != shown) {
        shown = (now - t0) / 100u;
        l->tick(now - t0);
      }
      wifi_tcp_alive();
      idle(l);
    }
  } else if (!*err) {
    /* The core ended the connection first: the reply is in only if the
     * server closed after sending it. */
    u32 why = ((const WifiNetIo *)WIFI_NET_ADDR)->status;
    status = why != WIFI_NETS_OK ? why
             : hs && *got &&
                     http_complete(resp, *got, head_only) != HTTP_SHORT
                 ? WIFI_NETS_OK
                 : WIFI_NETS_TIMEOUT;
  }
  if (*err) {
    status = WIFI_NETS_TLS;
    session_drop(host);
  } else if (hs) {
    session_keep(host);
  }
  now = l->ms();
  pool_add(&now, sizeof(now));
  pool_add(got, sizeof(*got));
  return status;
}

static struct {
  const TlsLink *l;
  char host[HOST_MAX];
  int open, hs, ended, err;
  u32 t0;
} ts;

u32 tls_stream_open(const TlsLink *l, const char *host, u32 ip, u32 port,
                    u32 timeout_ms, int *err) {
  br_ssl_engine_context *eng = &cc.eng;
  u32 hl = (u32)strlen(host);
  int resume;
  u8 rnd[32];
  *err = 0;
  if (!hl || hl >= HOST_MAX) {
    *err = TLS_ERR_NAME;
    return WIFI_NETS_TLS;
  }
  setup();
  client_init();
  set_time();
  fresh(rnd);
  br_ssl_engine_inject_entropy(eng, rnd, sizeof(rnd));
  memset(rnd, 0, sizeof(rnd));
  resume = session_find(host);
  if (resume >= 0)
    br_ssl_engine_set_session_parameters(eng, &sessions[resume].p);
  if (!br_ssl_client_reset(&cc, host, resume >= 0)) {
    *err = br_ssl_engine_last_error(eng);
    return WIFI_NETS_TLS;
  }
  wifi_tcp_setup(ip, port, timeout_ms);
  if (!l->start())
    return WIFI_NETS_BUSY;
  ts.l = l;
  memcpy(ts.host, host, hl + 1u);
  ts.open = 1;
  ts.hs = ts.ended = ts.err = 0;
  ts.t0 = l->ms();
  return WIFI_NETS_OK;
}

int tls_stream_pump(void) {
  br_ssl_engine_context *eng = &cc.eng;
  unsigned st;
  int moved = 0;
  size_t len;
  u8 *b;
  if (!ts.open || ts.ended)
    return 0;
  st = br_ssl_engine_current_state(eng);
  if (st & BR_SSL_CLOSED) {
    ts.err = br_ssl_engine_last_error(eng);
    if (!ts.err && !ts.hs)
      ts.err = TLS_ERR_EOF;
    ts.ended = 1;
    return 0;
  }
  if (st & BR_SSL_SENDAPP)
    ts.hs = 1;
  if (st & BR_SSL_SENDREC) {
    b = br_ssl_engine_sendrec_buf(eng, &len);
    len = wifi_tcp_write(b, (u32)len);
    if (len) {
      br_ssl_engine_sendrec_ack(eng, len);
      moved = 1;
    }
  }
  if (st & BR_SSL_RECVREC) {
    b = br_ssl_engine_recvrec_buf(eng, &len);
    len = wifi_tcp_read(b, (u32)len);
    if (len) {
      br_ssl_engine_recvrec_ack(eng, len);
      moved = 1;
    } else if (wifi_tcp_eof()) {
      if (!ts.hs)
        ts.err = TLS_ERR_EOF;
      ts.ended = 1;
    }
  }
  wifi_tcp_alive();
  if (!moved && !ts.ended && ts.l->done()) {
    /* Once more, for bytes the core put in just after that read. */
    b = (st & BR_SSL_RECVREC) ? br_ssl_engine_recvrec_buf(eng, &len) : 0;
    if (!b || !(len = wifi_tcp_read(b, (u32)len))) {
      ts.ended = 1;
      return 0;
    }
    br_ssl_engine_recvrec_ack(eng, len);
    moved = 1;
  }
  return moved;
}

int tls_stream_ready(void) {
  return ts.open && (ts.hs || (br_ssl_engine_current_state(&cc.eng) &
                               BR_SSL_SENDAPP) != 0);
}

int tls_stream_closed(int *err) {
  if (err)
    *err = ts.err;
  return !ts.open || ts.ended;
}

u32 tls_stream_write(const void *p, u32 n) {
  br_ssl_engine_context *eng = &cc.eng;
  size_t len;
  u8 *b;
  if (!ts.open || ts.ended || !(br_ssl_engine_current_state(eng) & BR_SSL_SENDAPP))
    return 0;
  b = br_ssl_engine_sendapp_buf(eng, &len);
  if (len > n)
    len = n;
  memcpy(b, p, len);
  br_ssl_engine_sendapp_ack(eng, len);
  br_ssl_engine_flush(eng, 0);
  return (u32)len;
}

u32 tls_stream_read(void *p, u32 n) {
  br_ssl_engine_context *eng = &cc.eng;
  size_t len;
  u8 *b;
  if (!ts.open || !(br_ssl_engine_current_state(eng) & BR_SSL_RECVAPP))
    return 0;
  b = br_ssl_engine_recvapp_buf(eng, &len);
  if (len > n)
    len = n;
  memcpy(p, b, len);
  br_ssl_engine_recvapp_ack(eng, len);
  return (u32)len;
}

void tls_stream_close(int graceful) {
  br_ssl_engine_context *eng = &cc.eng;
  u32 t0;
  if (!ts.open)
    return;
  if (graceful && !ts.ended) {
    br_ssl_engine_close(eng);
    t0 = ts.l->ms();
    while ((br_ssl_engine_current_state(eng) & BR_SSL_SENDREC) &&
           ts.l->ms() - t0 < 500u) {
      size_t len;
      u8 *b = br_ssl_engine_sendrec_buf(eng, &len);
      len = wifi_tcp_write(b, (u32)len);
      if (len)
        br_ssl_engine_sendrec_ack(eng, len);
    }
  }
  wifi_tcp_close(graceful ? WIFI_TCP_FIN : WIFI_TCP_ABORT);
  for (t0 = ts.l->ms(); !ts.l->done() && ts.l->ms() - t0 < END_WAIT_MS;) {
    wifi_tcp_alive();
    idle(ts.l);
  }
  if (ts.err)
    session_drop(ts.host);
  else if (ts.hs)
    session_keep(ts.host);
  ts.open = 0;
}

int tls_error_is_clock(int err) {
  return err == BR_ERR_X509_EXPIRED || err == BR_ERR_X509_TIME_UNKNOWN;
}

const char *tls_error_text(int err) {
  if (err >= BR_ERR_RECV_FATAL_ALERT && err < BR_ERR_SEND_FATAL_ALERT) {
    switch (err - BR_ERR_RECV_FATAL_ALERT) {
      case 40:
      case 71:
        return "the server refused the handshake: no cipher or curve in common";
      case 70:
        return "the server wants a newer TLS than 1.2";
      case 112:
        return "the server does not know this host name";
      default:
        return "the server ended the handshake with an alert";
    }
  }
  if (err >= BR_ERR_SEND_FATAL_ALERT)
    return "the server's handshake was not valid";
  switch (err) {
    case TLS_ERR_EOF:
      return "the server closed the connection during the handshake";
    case TLS_ERR_NAME:
      return "the host name is too long for TLS";
    case BR_ERR_UNSUPPORTED_VERSION:
    case BR_ERR_BAD_VERSION:
      return "the server does not offer TLS 1.2";
    case BR_ERR_BAD_CIPHER_SUITE:
      return "the server chose a cipher AuroraOS does not have";
    case BR_ERR_BAD_MAC:
      return "a record failed its integrity check";
    case BR_ERR_BAD_SIGNATURE:
    case BR_ERR_X509_BAD_SIGNATURE:
      return "a signature in the handshake did not verify";
    case BR_ERR_X509_EXPIRED:
      return "the certificate has expired or is not valid yet "
             "(check Settings > Clock)";
    case BR_ERR_X509_NOT_TRUSTED:
      return "the certificate is not from an authority AuroraOS trusts";
    case BR_ERR_X509_BAD_SERVER_NAME:
      return "the certificate is for another host name";
    case BR_ERR_X509_NOT_CA:
    case BR_ERR_X509_FORBIDDEN_KEY_USAGE:
    case BR_ERR_WRONG_KEY_USAGE:
      return "the certificate may not be used this way";
    case BR_ERR_X509_WEAK_PUBLIC_KEY:
      return "the certificate's key is too weak";
    case BR_ERR_X509_UNSUPPORTED:
    case BR_ERR_X509_WRONG_KEY_TYPE:
    case BR_ERR_INVALID_ALGORITHM:
      return "the certificate uses an algorithm AuroraOS does not have";
    case BR_ERR_X509_EMPTY_CHAIN:
      return "the server sent no certificate";
    default:
      if (err >= BR_ERR_X509_OK && err < 64)
        return "the certificate could not be read";
      return "the TLS handshake failed";
  }
}
