/* SPDX-License-Identifier: GPL-2.0 */
/* LICENSE: GPL-2.0, part of the Wi-Fi driver. See docs/wifi.md "License and
 * credits".
 *
 * The EAPOL-Key exchanges of WPA2-PSK, from IEEE 802.11-2016 12.7 and IEEE
 * 802.1X-2004. Only key descriptor version 2 (HMAC-SHA1 MICs, AES key wrap),
 * which a CCMP pairwise cipher gives. */
#include "wpa11.h"
#include "crypto.h"

#define ETH_EAPOL 0x888Eu

/* Offsets in an EAPOL frame. */
#define E_VER   0
#define E_TYPE  1
#define E_LEN   2
#define E_DESC  4
#define E_INFO  5
#define E_KLEN  7
#define E_RPLY  9
#define E_NONCE 17
#define E_IV    49
#define E_RSC   65
#define E_MIC   81
#define E_DLEN  97
#define E_DATA  99

#define KI_VER     0x0007u
#define KI_PAIR    0x0008u
#define KI_INSTALL 0x0040u
#define KI_ACK     0x0080u
#define KI_MIC     0x0100u
#define KI_SECURE  0x0200u
#define KI_ENC     0x1000u

static uint32_t be16(const uint8_t *p) { return ((uint32_t)p[0] << 8) | p[1]; }

static void put16(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v >> 8);
  p[1] = (uint8_t)v;
}

static void copy(uint8_t *d, const uint8_t *s, uint32_t n) {
  for (uint32_t i = 0; i < n; i++)
    d[i] = s[i];
}

static int cmp(const uint8_t *a, const uint8_t *b, uint32_t n) {
  for (uint32_t i = 0; i < n; i++)
    if (a[i] != b[i])
      return a[i] < b[i] ? -1 : 1;
  return 0;
}

void wpa_begin(WpaState *s, const uint8_t pmk[32], const uint8_t spa[6],
               const uint8_t seed[32], const uint8_t *ie, uint32_t ie_len,
               uint32_t group) {
  crypto_wipe(s, sizeof(*s));
  copy(s->pmk, pmk, 32);
  copy(s->spa, spa, 6);
  prf_sha1(seed, 32, "Init Counter", spa, 6, s->snonce, 32);
  if (ie_len > sizeof(s->ie))
    ie_len = sizeof(s->ie);
  copy(s->ie, ie, ie_len);
  s->ie_len = ie_len;
  s->group = group;
}

void wpa_end(WpaState *s) { crypto_wipe(s, sizeof(*s)); }

/* PTK = PRF-384(PMK, "Pairwise key expansion", min(AA, SPA) || max(AA, SPA)
 * || min(ANonce, SNonce) || max(ANonce, SNonce)). */
static void wpa_ptk(WpaState *s) {
  uint8_t b[76];
  int a_first = cmp(s->aa, s->spa, 6) < 0;
  copy(b, a_first ? s->aa : s->spa, 6);
  copy(b + 6, a_first ? s->spa : s->aa, 6);
  int n_first = cmp(s->anonce, s->snonce, 32) < 0;
  copy(b + 12, n_first ? s->anonce : s->snonce, 32);
  copy(b + 44, n_first ? s->snonce : s->anonce, 32);
  prf_sha1(s->pmk, 32, "Pairwise key expansion", b, sizeof(b), s->ptk, 48);
  s->have_ptk = 1;
}

/* HMAC-SHA1-128 with the KCK over the frame, its MIC field taken as zero. */
static void wpa_mic(const WpaState *s, const uint8_t *e, uint32_t len,
                    uint8_t mic[16]) {
  Hmac h;
  uint8_t z[16], d[20];
  for (int i = 0; i < 16; i++)
    z[i] = 0;
  hmac_init(&h, s->ptk, 16);
  hmac_update(&h, e, E_MIC);
  hmac_update(&h, z, 16);
  hmac_update(&h, e + E_MIC + 16, len - E_MIC - 16u);
  hmac_final(&h, d);
  copy(mic, d, 16);
  crypto_wipe(&h, sizeof(h));
}

/* An answer to the authenticator: `info`, the replay counter it sent, an
 * optional nonce and key data, and the MIC. */
static uint32_t wpa_reply(const WpaState *s, const uint8_t *rx, uint32_t info,
                          const uint8_t *nonce, const uint8_t *data,
                          uint32_t dlen, uint8_t *out) {
  uint8_t *e = out + 14;
  uint32_t elen = E_DATA + dlen;
  copy(out, s->aa, 6);
  copy(out + 6, s->spa, 6);
  put16(out + 12, ETH_EAPOL);
  for (uint32_t i = 0; i < elen; i++)
    e[i] = 0;
  e[E_VER] = rx[E_VER] ? rx[E_VER] : 1;
  e[E_TYPE] = 3; /* EAPOL-Key */
  put16(e + E_LEN, elen - 4u);
  e[E_DESC] = 2; /* RSN */
  put16(e + E_INFO, info);
  copy(e + E_RPLY, rx + E_RPLY, 8);
  if (nonce)
    copy(e + E_NONCE, nonce, 32);
  put16(e + E_DLEN, dlen);
  copy(e + E_DATA, data, dlen);
  wpa_mic(s, e, elen, e + E_MIC);
  return 14u + elen;
}

/* The GTK KDE in decrypted key data: dd, length, 00-0F-AC, 1, key id, 0,
 * the key. 1 when one was found. */
static int wpa_gtk(WpaState *s, const uint8_t *d, uint32_t len) {
  uint32_t i = 0;
  while (i + 2u <= len) {
    uint32_t id = d[i], l = d[i + 1];
    if (id == 0xDD && l == 0)
      break; /* padding */
    if (i + 2u + l > len)
      break;
    const uint8_t *v = d + i + 2;
    if (id == 0xDD && l >= 6u + 16u && v[0] == 0x00 && v[1] == 0x0F &&
        v[2] == 0xAC && v[3] == 1) {
      uint32_t n = l - 6u;
      if (n > sizeof(s->gtk))
        return 0;
      copy(s->gtk, v + 6, n);
      s->gtk_len = n;
      s->gtk_idx = v[4] & 3u;
      return 1;
    }
    i += 2u + l;
  }
  return 0;
}

int wpa_rx(WpaState *s, const uint8_t *f, uint32_t len, uint8_t *out,
           uint32_t *out_len, uint32_t *info) {
  static uint8_t plain[256];
  *out_len = 0;
  *info = 0;
  if (len < 14u + E_DATA || be16(f + 12) != ETH_EAPOL)
    return WPA_RX_NONE;
  const uint8_t *e = f + 14;
  uint32_t elen = 4u + be16(e + E_LEN);
  if (e[E_TYPE] != 3 || elen < E_DATA || 14u + elen > len)
    return WPA_RX_NONE;
  uint32_t ki = be16(e + E_INFO), dlen = be16(e + E_DLEN);
  *info = ki;
  if (e[E_DESC] != 2 || (ki & KI_VER) != 2u || E_DATA + dlen > elen)
    return WPA_RX_BAD;

  if ((ki & KI_PAIR) && (ki & KI_ACK) && !(ki & KI_MIC)) { /* message 1 */
    if (s->have_replay && cmp(e + E_RPLY, s->replay, 8) <= 0)
      return WPA_RX_BAD;
    copy(s->aa, f + 6, 6);
    copy(s->anonce, e + E_NONCE, 32);
    wpa_ptk(s);
    s->m1++;
    *out_len = wpa_reply(s, e, 2u | KI_PAIR | KI_MIC, s->snonce, s->ie,
                         s->ie_len, out);
    return WPA_RX_M1;
  }

  if (!s->have_ptk || !(ki & KI_MIC) || !(ki & KI_ACK))
    return WPA_RX_BAD;
  if (s->have_replay && cmp(e + E_RPLY, s->replay, 8) <= 0)
    return WPA_RX_BAD;
  if ((ki & KI_PAIR) && cmp(e + E_NONCE, s->anonce, 32) != 0)
    return WPA_RX_BAD; /* not the ANonce message 3 must repeat */
  uint8_t mic[16];
  wpa_mic(s, e, elen, mic);
  if (cmp(mic, e + E_MIC, 16) != 0) {
    s->bad_mic++;
    return WPA_RX_BAD_MIC;
  }
  copy(s->replay, e + E_RPLY, 8);
  s->have_replay = 1;

  int got = 0;
  if ((ki & KI_ENC) && dlen >= 24u && dlen % 8u == 0 &&
      dlen - 8u <= sizeof(plain)) {
    if (!aes_unwrap(s->ptk + 16, e + E_DATA, dlen, plain))
      return WPA_RX_BAD;
    got = wpa_gtk(s, plain, dlen - 8u);
    crypto_wipe(plain, dlen - 8u);
  }

  if ((ki & KI_PAIR) && (ki & KI_INSTALL)) { /* message 3 */
    s->m3++;
    if (got) {
      copy(s->gtk_rsc, e + E_RSC, 8);
      s->gtk_ready = 1;
    }
    s->ptk_ready = 1;
    *out_len = wpa_reply(s, e, 2u | KI_PAIR | KI_MIC | KI_SECURE, 0, 0, 0,
                         out);
    return WPA_RX_M3;
  }
  if (!(ki & KI_PAIR) && got) { /* group message 1 */
    s->g1++;
    copy(s->gtk_rsc, e + E_RSC, 8);
    s->gtk_ready = 1;
    *out_len = wpa_reply(s, e, 2u | KI_MIC | KI_SECURE, 0, 0, 0, out);
    return WPA_RX_G1;
  }
  return WPA_RX_BAD;
}
