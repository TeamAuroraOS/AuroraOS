/* SPDX-License-Identifier: GPL-2.0 */
/* LICENSE: GPL-2.0, part of the Wi-Fi driver. See docs/wifi.md "License and
 * credits". */
#ifndef AURORA_WPA11_H
#define AURORA_WPA11_H

#include <stdint.h>

/* The station side of WPA2-PSK with CCMP: the EAPOL-Key 4-way handshake and
 * the group key handshake (IEEE 802.11-2016 12.7.6 and 12.7.7). Frames in and
 * out are Ethernet II, type 0x888E. */
typedef struct {
  uint8_t pmk[32];
  uint8_t spa[6];   /* ours */
  uint8_t aa[6];    /* the authenticator, from message 1 */
  uint8_t anonce[32], snonce[32];
  uint8_t ptk[48];  /* KCK, KEK, TK */
  int have_ptk;
  uint8_t replay[8]; /* the last replay counter of a frame whose MIC held */
  int have_replay;
  uint8_t ie[64];   /* our RSN element, as in the association request */
  uint32_t ie_len;
  uint32_t group;   /* group cipher suite type: 4 CCMP, 2 TKIP */
  /* A group key for the core to install. */
  uint8_t gtk[32];
  uint32_t gtk_len, gtk_idx;
  uint8_t gtk_rsc[8];
  int ptk_ready;    /* message 4 is out: the TK waits to be installed */
  int gtk_ready;
  uint32_t m1, m3, g1, bad_mic; /* frames taken */
} WpaState;

enum {
  WPA_RX_NONE = 0, /* not an EAPOL-Key frame */
  WPA_RX_M1,       /* message 1: message 2 built */
  WPA_RX_M3,       /* message 3: message 4 built, keys ready */
  WPA_RX_G1,       /* group message 1: group message 2 built, GTK ready */
  WPA_RX_BAD_MIC,  /* a MIC that did not hold: a wrong password, mostly */
  WPA_RX_BAD,      /* malformed, unexpected or replayed */
};

/* The longest frame built here. */
#define WPA_FRAME_MAX 200

/* For a new association. `seed` is 32 bytes to make the SNonce from. */
void wpa_begin(WpaState *s, const uint8_t pmk[32], const uint8_t spa[6],
               const uint8_t seed[32], const uint8_t *ie, uint32_t ie_len,
               uint32_t group);
/* One received frame. `info` gets its key information field. */
int wpa_rx(WpaState *s, const uint8_t *f, uint32_t len, uint8_t *out,
           uint32_t *out_len, uint32_t *info);
void wpa_end(WpaState *s); /* wipes the keys */

#endif
