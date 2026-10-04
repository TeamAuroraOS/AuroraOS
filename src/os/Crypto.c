/* SPDX-License-Identifier: GPL-2.0 */
/* LICENSE: GPL-2.0, part of the Wi-Fi driver. See docs/wifi.md "License and
 * credits".
 *
 * SHA-1 (FIPS 180-4), HMAC (RFC 2104), PBKDF2 (RFC 8018), the IEEE 802.11
 * PRF and AES-128 with RFC 3394 key wrap (FIPS 197), from the standards. */
#include "crypto.h"

void crypto_wipe(void *p, uint32_t n) {
  volatile uint8_t *v = (volatile uint8_t *)p;
  for (uint32_t i = 0; i < n; i++)
    v[i] = 0;
}

static uint32_t rol(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

static void sha1_block(uint32_t h[5], const uint8_t *p) {
  uint32_t w[16];
  for (int i = 0; i < 16; i++)
    w[i] = ((uint32_t)p[4 * i] << 24) | ((uint32_t)p[4 * i + 1] << 16) |
           ((uint32_t)p[4 * i + 2] << 8) | p[4 * i + 3];
  uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
  for (int i = 0; i < 80; i++) {
    uint32_t f, k;
    if (i >= 16)
      w[i & 15] = rol(w[(i + 13) & 15] ^ w[(i + 8) & 15] ^ w[(i + 2) & 15] ^
                          w[i & 15],
                      1);
    if (i < 20) {
      f = (b & c) | (~b & d);
      k = 0x5A827999u;
    } else if (i < 40) {
      f = b ^ c ^ d;
      k = 0x6ED9EBA1u;
    } else if (i < 60) {
      f = (b & c) | (b & d) | (c & d);
      k = 0x8F1BBCDCu;
    } else {
      f = b ^ c ^ d;
      k = 0xCA62C1D6u;
    }
    uint32_t t = rol(a, 5) + f + e + k + w[i & 15];
    e = d;
    d = c;
    c = rol(b, 30);
    b = a;
    a = t;
  }
  h[0] += a;
  h[1] += b;
  h[2] += c;
  h[3] += d;
  h[4] += e;
}

void sha1_init(Sha1 *c) {
  c->h[0] = 0x67452301u;
  c->h[1] = 0xEFCDAB89u;
  c->h[2] = 0x98BADCFEu;
  c->h[3] = 0x10325476u;
  c->h[4] = 0xC3D2E1F0u;
  c->bytes = 0;
  c->n = 0;
}

void sha1_update(Sha1 *c, const uint8_t *p, uint32_t len) {
  c->bytes += len;
  while (len) {
    c->buf[c->n++] = *p++;
    len--;
    if (c->n == 64) {
      sha1_block(c->h, c->buf);
      c->n = 0;
    }
  }
}

void sha1_final(Sha1 *c, uint8_t out[20]) {
  uint32_t hi = c->bytes >> 29, lo = c->bytes << 3;
  uint8_t pad = 0x80;
  sha1_update(c, &pad, 1);
  pad = 0;
  while (c->n != 56)
    sha1_update(c, &pad, 1);
  uint8_t l[8];
  for (int i = 0; i < 4; i++) {
    l[i] = (uint8_t)(hi >> (24 - 8 * i));
    l[4 + i] = (uint8_t)(lo >> (24 - 8 * i));
  }
  sha1_update(c, l, 8);
  for (int i = 0; i < 20; i++)
    out[i] = (uint8_t)(c->h[i / 4] >> (24 - 8 * (i % 4)));
}

static void sha1_copy(Sha1 *d, const Sha1 *s) {
  for (int i = 0; i < 5; i++)
    d->h[i] = s->h[i];
  d->bytes = s->bytes;
  d->n = s->n;
  for (int i = 0; i < 64; i++)
    d->buf[i] = s->buf[i];
}

void hmac_init(Hmac *h, const uint8_t *key, uint32_t klen) {
  uint8_t k[64];
  for (int i = 0; i < 64; i++)
    k[i] = 0;
  if (klen > 64u) {
    sha1_init(&h->in);
    sha1_update(&h->in, key, klen);
    sha1_final(&h->in, k);
  } else {
    for (uint32_t i = 0; i < klen; i++)
      k[i] = key[i];
  }
  for (int i = 0; i < 64; i++)
    k[i] ^= 0x36;
  sha1_init(&h->in);
  sha1_update(&h->in, k, 64);
  for (int i = 0; i < 64; i++)
    k[i] ^= 0x36 ^ 0x5C;
  sha1_init(&h->out);
  sha1_update(&h->out, k, 64);
  crypto_wipe(k, sizeof(k));
}

void hmac_update(Hmac *h, const uint8_t *p, uint32_t len) {
  sha1_update(&h->in, p, len);
}

void hmac_final(Hmac *h, uint8_t out[20]) {
  uint8_t ih[20];
  sha1_final(&h->in, ih);
  sha1_update(&h->out, ih, 20);
  sha1_final(&h->out, out);
}

static void hmac_copy(Hmac *d, const Hmac *s) {
  sha1_copy(&d->in, &s->in);
  sha1_copy(&d->out, &s->out);
}

void hmac_sha1(const uint8_t *key, uint32_t klen, const uint8_t *msg,
               uint32_t mlen, uint8_t out[20]) {
  Hmac h;
  hmac_init(&h, key, klen);
  hmac_update(&h, msg, mlen);
  hmac_final(&h, out);
  crypto_wipe(&h, sizeof(h));
}

void wpa_pmk(const uint8_t *pass, uint32_t plen, const uint8_t *ssid,
             uint32_t slen, uint8_t pmk[32]) {
  Hmac base, h;
  uint8_t u[20], t[20];
  hmac_init(&base, pass, plen);
  for (uint32_t blk = 1; blk <= 2; blk++) {
    uint8_t cnt[4] = {0, 0, 0, (uint8_t)blk};
    hmac_copy(&h, &base);
    hmac_update(&h, ssid, slen);
    hmac_update(&h, cnt, 4);
    hmac_final(&h, u);
    for (int i = 0; i < 20; i++)
      t[i] = u[i];
    for (int r = 1; r < 4096; r++) {
      hmac_copy(&h, &base);
      hmac_update(&h, u, 20);
      hmac_final(&h, u);
      for (int i = 0; i < 20; i++)
        t[i] ^= u[i];
    }
    for (uint32_t i = 0; i < 20 && (blk - 1u) * 20u + i < 32u; i++)
      pmk[(blk - 1u) * 20u + i] = t[i];
  }
  crypto_wipe(&base, sizeof(base));
  crypto_wipe(&h, sizeof(h));
  crypto_wipe(u, sizeof(u));
  crypto_wipe(t, sizeof(t));
}

void prf_sha1(const uint8_t *key, uint32_t klen, const char *label,
              const uint8_t *data, uint32_t dlen, uint8_t *out, uint32_t olen) {
  Hmac base, h;
  uint8_t d[20], zero = 0, i = 0;
  uint32_t ll = 0, pos = 0;
  while (label[ll])
    ll++;
  hmac_init(&base, key, klen);
  while (pos < olen) {
    hmac_copy(&h, &base);
    hmac_update(&h, (const uint8_t *)label, ll);
    hmac_update(&h, &zero, 1);
    hmac_update(&h, data, dlen);
    hmac_update(&h, &i, 1);
    hmac_final(&h, d);
    for (int k = 0; k < 20 && pos < olen; k++)
      out[pos++] = d[k];
    i++;
  }
  crypto_wipe(&base, sizeof(base));
  crypto_wipe(&h, sizeof(h));
  crypto_wipe(d, sizeof(d));
}

/* The S-boxes, generated once (the multiplicative inverse in GF(2^8) and the
 * affine map) rather than typed in. */
static uint8_t sbox[256], isbox[256];
static int sbox_ready;

static uint8_t rotl8(uint8_t x, int n) {
  return (uint8_t)((x << n) | (x >> (8 - n)));
}

static void aes_tables(void) {
  uint8_t p = 1, q = 1;
  if (sbox_ready)
    return;
  do {
    p = (uint8_t)(p ^ (p << 1) ^ ((p & 0x80) ? 0x1B : 0)); /* p * 3 */
    q ^= (uint8_t)(q << 1);                                 /* q / 3 */
    q ^= (uint8_t)(q << 2);
    q ^= (uint8_t)(q << 4);
    if (q & 0x80)
      q ^= 0x09;
    sbox[p] = (uint8_t)(q ^ rotl8(q, 1) ^ rotl8(q, 2) ^ rotl8(q, 3) ^
                        rotl8(q, 4) ^ 0x63);
  } while (p != 1);
  sbox[0] = 0x63;
  for (int i = 0; i < 256; i++)
    isbox[sbox[i]] = (uint8_t)i;
  sbox_ready = 1;
}

static uint8_t xt(uint8_t x) {
  return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1B : 0));
}

static uint8_t gmul(uint8_t a, uint8_t b) {
  uint8_t r = 0;
  while (b) {
    if (b & 1)
      r ^= a;
    a = xt(a);
    b >>= 1;
  }
  return r;
}

void aes128_init(Aes128 *a, const uint8_t key[16]) {
  uint8_t rcon = 1;
  aes_tables();
  for (int i = 0; i < 16; i++)
    a->rk[i] = key[i];
  for (int i = 16; i < 176; i += 4) {
    uint8_t t0 = a->rk[i - 4], t1 = a->rk[i - 3], t2 = a->rk[i - 2],
            t3 = a->rk[i - 1];
    if (i % 16 == 0) {
      uint8_t s = t0;
      t0 = (uint8_t)(sbox[t1] ^ rcon);
      t1 = sbox[t2];
      t2 = sbox[t3];
      t3 = sbox[s];
      rcon = xt(rcon);
    }
    a->rk[i] = a->rk[i - 16] ^ t0;
    a->rk[i + 1] = a->rk[i - 15] ^ t1;
    a->rk[i + 2] = a->rk[i - 14] ^ t2;
    a->rk[i + 3] = a->rk[i - 13] ^ t3;
  }
}

/* The state is column-major: s[row + 4 * column]. */
static void add_key(uint8_t s[16], const uint8_t *rk) {
  for (int i = 0; i < 16; i++)
    s[i] ^= rk[i];
}

void aes128_encrypt(const Aes128 *a, const uint8_t in[16], uint8_t out[16]) {
  uint8_t s[16], t[16];
  for (int i = 0; i < 16; i++)
    s[i] = in[i];
  add_key(s, a->rk);
  for (int round = 1; round <= 10; round++) {
    for (int r = 0; r < 4; r++) /* SubBytes, ShiftRows */
      for (int c = 0; c < 4; c++)
        t[r + 4 * c] = sbox[s[r + 4 * ((c + r) & 3)]];
    if (round < 10) {
      for (int c = 0; c < 4; c++) { /* MixColumns */
        uint8_t *v = t + 4 * c, a0 = v[0], a1 = v[1], a2 = v[2], a3 = v[3];
        s[4 * c] = (uint8_t)(xt(a0) ^ xt(a1) ^ a1 ^ a2 ^ a3);
        s[4 * c + 1] = (uint8_t)(a0 ^ xt(a1) ^ xt(a2) ^ a2 ^ a3);
        s[4 * c + 2] = (uint8_t)(a0 ^ a1 ^ xt(a2) ^ xt(a3) ^ a3);
        s[4 * c + 3] = (uint8_t)(xt(a0) ^ a0 ^ a1 ^ a2 ^ xt(a3));
      }
    } else {
      for (int i = 0; i < 16; i++)
        s[i] = t[i];
    }
    add_key(s, a->rk + 16 * round);
  }
  for (int i = 0; i < 16; i++)
    out[i] = s[i];
}

void aes128_decrypt(const Aes128 *a, const uint8_t in[16], uint8_t out[16]) {
  uint8_t s[16], t[16];
  for (int i = 0; i < 16; i++)
    s[i] = in[i];
  add_key(s, a->rk + 160);
  for (int round = 9; round >= 0; round--) {
    for (int r = 0; r < 4; r++) /* InvShiftRows, InvSubBytes */
      for (int c = 0; c < 4; c++)
        t[r + 4 * ((c + r) & 3)] = isbox[s[r + 4 * c]];
    add_key(t, a->rk + 16 * round);
    if (round > 0) {
      for (int c = 0; c < 4; c++) { /* InvMixColumns */
        uint8_t *v = t + 4 * c, a0 = v[0], a1 = v[1], a2 = v[2], a3 = v[3];
        s[4 * c] = (uint8_t)(gmul(a0, 14) ^ gmul(a1, 11) ^ gmul(a2, 13) ^
                             gmul(a3, 9));
        s[4 * c + 1] = (uint8_t)(gmul(a0, 9) ^ gmul(a1, 14) ^ gmul(a2, 11) ^
                                 gmul(a3, 13));
        s[4 * c + 2] = (uint8_t)(gmul(a0, 13) ^ gmul(a1, 9) ^ gmul(a2, 14) ^
                                 gmul(a3, 11));
        s[4 * c + 3] = (uint8_t)(gmul(a0, 11) ^ gmul(a1, 13) ^ gmul(a2, 9) ^
                                 gmul(a3, 14));
      }
    } else {
      for (int i = 0; i < 16; i++)
        s[i] = t[i];
    }
  }
  for (int i = 0; i < 16; i++)
    out[i] = s[i];
}

int aes_unwrap(const uint8_t kek[16], const uint8_t *in, uint32_t len,
               uint8_t *out) {
  Aes128 k;
  uint8_t a[8], b[16];
  uint32_t n = len / 8u - 1u;
  int ok = 1;
  if (len % 8u || len < 24u)
    return 0;
  aes128_init(&k, kek);
  for (int i = 0; i < 8; i++)
    a[i] = in[i];
  for (uint32_t i = 0; i < len - 8u; i++)
    out[i] = in[8 + i];
  for (int j = 5; j >= 0; j--) {
    for (uint32_t i = n; i >= 1u; i--) {
      uint32_t t = n * (uint32_t)j + i;
      for (int x = 0; x < 8; x++)
        b[x] = a[x];
      b[7] ^= (uint8_t)t;
      b[6] ^= (uint8_t)(t >> 8);
      b[5] ^= (uint8_t)(t >> 16);
      b[4] ^= (uint8_t)(t >> 24);
      for (int x = 0; x < 8; x++)
        b[8 + x] = out[(i - 1u) * 8u + (uint32_t)x];
      aes128_decrypt(&k, b, b);
      for (int x = 0; x < 8; x++) {
        a[x] = b[x];
        out[(i - 1u) * 8u + (uint32_t)x] = b[8 + x];
      }
    }
  }
  for (int i = 0; i < 8; i++)
    if (a[i] != 0xA6)
      ok = 0;
  crypto_wipe(&k, sizeof(k));
  crypto_wipe(b, sizeof(b));
  return ok;
}

void aes_wrap(const uint8_t kek[16], const uint8_t *in, uint32_t len,
              uint8_t *out) {
  Aes128 k;
  uint8_t b[16];
  uint32_t n = len / 8u;
  aes128_init(&k, kek);
  for (int i = 0; i < 8; i++)
    out[i] = 0xA6;
  for (uint32_t i = 0; i < len; i++)
    out[8 + i] = in[i];
  for (uint32_t j = 0; j < 6u; j++) {
    for (uint32_t i = 1; i <= n; i++) {
      uint32_t t = n * j + i;
      for (int x = 0; x < 8; x++) {
        b[x] = out[x];
        b[8 + x] = out[i * 8u + (uint32_t)x];
      }
      aes128_encrypt(&k, b, b);
      b[7] ^= (uint8_t)t;
      b[6] ^= (uint8_t)(t >> 8);
      b[5] ^= (uint8_t)(t >> 16);
      b[4] ^= (uint8_t)(t >> 24);
      for (int x = 0; x < 8; x++) {
        out[x] = b[x];
        out[i * 8u + (uint32_t)x] = b[8 + x];
      }
    }
  }
  crypto_wipe(&k, sizeof(k));
  crypto_wipe(b, sizeof(b));
}
