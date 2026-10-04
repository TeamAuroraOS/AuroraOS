/* SPDX-License-Identifier: GPL-2.0 */
/* LICENSE: GPL-2.0, part of the Wi-Fi driver. See docs/wifi.md "License and
 * credits". */
#ifndef AURORA_CRYPTO_H
#define AURORA_CRYPTO_H

#include <stdint.h>

/* What WPA2-PSK needs, built for both CPUs: SHA-1 (FIPS 180-4), HMAC
 * (RFC 2104), PBKDF2 (RFC 8018), the IEEE 802.11 PRF and AES-128 key wrap
 * (FIPS 197, RFC 3394). Byte access only, no library calls. */

typedef struct {
  uint32_t h[5];
  uint32_t bytes; /* fed so far; messages here stay far below 4 GB */
  uint32_t n;     /* bytes waiting in buf */
  uint8_t buf[64];
} Sha1;

void sha1_init(Sha1 *c);
void sha1_update(Sha1 *c, const uint8_t *p, uint32_t len);
void sha1_final(Sha1 *c, uint8_t out[20]);

typedef struct {
  Sha1 in, out;
} Hmac;

void hmac_init(Hmac *h, const uint8_t *key, uint32_t klen);
void hmac_update(Hmac *h, const uint8_t *p, uint32_t len);
void hmac_final(Hmac *h, uint8_t out[20]);
void hmac_sha1(const uint8_t *key, uint32_t klen, const uint8_t *msg,
               uint32_t mlen, uint8_t out[20]);

/* The WPA2 pairwise master key: PBKDF2-HMAC-SHA1, 4096 rounds, 32 bytes. */
void wpa_pmk(const uint8_t *pass, uint32_t plen, const uint8_t *ssid,
             uint32_t slen, uint8_t pmk[32]);

/* PRF-n of IEEE 802.11: HMAC-SHA1(key, label || 0 || data || i) for i = 0,
 * 1, ..., `olen` bytes of it. */
void prf_sha1(const uint8_t *key, uint32_t klen, const char *label,
              const uint8_t *data, uint32_t dlen, uint8_t *out, uint32_t olen);

typedef struct {
  uint8_t rk[176]; /* the 11 round keys */
} Aes128;

void aes128_init(Aes128 *a, const uint8_t key[16]);
void aes128_encrypt(const Aes128 *a, const uint8_t in[16], uint8_t out[16]);
void aes128_decrypt(const Aes128 *a, const uint8_t in[16], uint8_t out[16]);

/* RFC 3394 with the default IV. `len` is the wrapped length, a multiple of 8
 * and at least 24; `out` gets len - 8 bytes. 1 when the IV checks out. */
int aes_unwrap(const uint8_t kek[16], const uint8_t *in, uint32_t len,
               uint8_t *out);
/* `len` is the plain length, a multiple of 8, at least 16; out gets len + 8. */
void aes_wrap(const uint8_t kek[16], const uint8_t *in, uint32_t len,
              uint8_t *out);

/* Overwrites `n` bytes in a way the compiler keeps. */
void crypto_wipe(void *p, uint32_t n);

#endif
