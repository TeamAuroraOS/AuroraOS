/* SPDX-License-Identifier: GPL-2.0 */
/* LICENSE: GPL-2.0, part of the Wi-Fi driver. See docs/wifi.md "License and
 * credits".
 *
 * What a join needs on the ARM9 side, shared by the OS and the apps that
 * link AppNet.c: the firmware staged from the card, the request block, and
 * the reason a join failed. */
#include "aurora.h"
#include "crypto.h"
#include "ff.h"
#include "timer.h"
#include "wifi.h"

extern void os_cache_sync(void);

static int load_blob(const char *path, u32 dst, u32 maxlen,
                     volatile u32 *len_out) {
  static FIL f;
  UINT br = 0;
  *len_out = 0;
  if (f_open(&f, path, FA_READ) != FR_OK)
    return 0;
  u32 sz = (u32)f_size(&f);
  if (sz == 0 || sz > maxlen) {
    f_close(&f);
    return 0;
  }
  FRESULT fr = f_read(&f, (void *)dst, sz, &br);
  f_close(&f);
  *len_out = br;
  return fr == FR_OK && br == sz;
}

int wifi_fw_stage(u32 main_type) {
  WifiFw *fw = (WifiFw *)WIFI_FW_ADDR;
  int ok = 1;
  fw->magic = 0;
  fw->main_len = 0;
  fw->main_type = 0;
  ok &= load_blob(WIFI_FW_DIR "/STUBDATA.BIN", WIFI_FW_STUBDATA, 0x1000,
                  &fw->stubdata_len);
  ok &= load_blob(WIFI_FW_DIR "/STUBCODE.BIN", WIFI_FW_STUBCODE, 0x1000,
                  &fw->stubcode_len);
  ok &= load_blob(WIFI_FW_DIR "/DATABASE.BIN", WIFI_FW_DATABASE, 0x1000,
                  &fw->database_len);
  ok &= load_blob(main_type == WIFI_FW_TYPE4 ? WIFI_FW_DIR "/MAINTYP4.BIN"
                                             : WIFI_FW_DIR "/MAINTYP1.BIN",
                  WIFI_FW_MAIN, 0x60000, &fw->main_len);
  if (ok) {
    fw->main_type = main_type;
    fw->magic = WIFI_FW_MAGIC;
  }
  return ok;
}

static int hex_digit(char c) {
  return c >= '0' && c <= '9'   ? c - '0'
         : c >= 'a' && c <= 'f' ? c - 'a' + 10
         : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                : -1;
}

/* Read by the core at the start of a boot with WIFI_OPT_CONNECT. The password
 * stays on this side: the core gets the PMK made from it, or the PSK itself
 * when the password is 64 hex digits. The seed for the core's SNonce is
 * hashed from timer readings around that work. */
void wifi_request(const char *ssid, const char *pass) {
  static u32 count;
  WifiReq *q = (WifiReq *)WIFI_REQ_ADDR;
  uint8_t pmk[32], d[20];
  u32 n = 0, p = 0, t;
  Sha1 h;
  while (ssid[n] && n < sizeof(q->ssid))
    n++;
  while (pass && pass[p] && p < 64u)
    p++;
  q->magic = 0;
  sha1_init(&h);
  t = timer_ticks();
  sha1_update(&h, (const uint8_t *)&t, 4);
  count++;
  sha1_update(&h, (const uint8_t *)&count, 4);
  for (u32 i = 0; i < sizeof(q->ssid); i++)
    q->ssid[i] = i < n ? (u8)ssid[i] : 0;
  q->ssid_len = n;
  q->has_pmk = 0;
  if (p) {
    int hex = p == 64u;
    for (u32 i = 0; hex && i < 64u; i++)
      hex = hex_digit(pass[i]) >= 0;
    if (hex) {
      for (u32 i = 0; i < 32u; i++)
        pmk[i] = (uint8_t)(hex_digit(pass[2 * i]) << 4 | hex_digit(pass[2 * i + 1]));
    } else {
      wpa_pmk((const uint8_t *)pass, p, (const uint8_t *)ssid, n, pmk);
    }
    for (u32 i = 0; i < 32u; i++)
      q->pmk[i] = pmk[i];
    q->has_pmk = 1;
    crypto_wipe(pmk, sizeof(pmk));
  }
  t = timer_ticks();
  sha1_update(&h, (const uint8_t *)&t, 4);
  sha1_final(&h, d);
  for (u32 i = 0; i < 32u; i++)
    q->seed[i] = d[i % 20u] ^ (uint8_t)(t >> (8 * (i & 3u)));
  crypto_wipe(d, sizeof(d));
  q->magic = WIFI_REQ_MAGIC;
  os_cache_sync();
}

/* Once the core is done with it, so the PMK does not stay in shared memory. */
void wifi_request_clear(void) {
  WifiReq *q = (WifiReq *)WIFI_REQ_ADDR;
  q->magic = 0;
  q->ssid_len = 0;
  q->has_pmk = 0;
  for (u32 i = 0; i < sizeof(q->pmk); i++)
    q->pmk[i] = 0;
  for (u32 i = 0; i < sizeof(q->seed); i++)
    q->seed[i] = 0;
  os_cache_sync();
}

static char *put(char *d, const char *e, const char *s) {
  while (*s && d < e)
    *d++ = *s++;
  return d;
}

void wifi_join_why(const WifiShared *w, const char *ssid, char *out,
                   u32 size) {
  char num[12], *p = out, *e;
  u32 v, k = 0;
  if (!size)
    return;
  e = out + size - 1u;
  if (w->fw_assert) {
    p = put(p, e, "the Wi-Fi firmware stopped");
  } else if (w->wmi_event != 0x1001u) {
    p = put(p, e, "the Wi-Fi chip did not start");
  } else {
    switch (w->conn_stage) {
      case WIFI_CONN_NOTFOUND:
        p = put(put(p, e, ssid), e, " was not found (it must be on, on 2.4 GHz)");
        break;
      case WIFI_CONN_SECURED:
        p = put(p, e, w->sec == WIFI_SEC_WPA2 ? "it needs a password: set it in Settings > Wi-Fi"
                                             : "its security is not supported (WPA2 is)");
        break;
      case WIFI_CONN_BADPASS:
        p = put(p, e, "the network did not accept the password");
        break;
      case WIFI_CONN_KEYS:
        p = put(p, e, "the password check did not finish");
        break;
      case WIFI_CONN_FAILED:
        p = put(p, e, "the firmware could not join it, reason ");
        v = w->conn_reason & 0xFFu;
        do {
          num[k++] = (char)('0' + v % 10u);
          v /= 10u;
        } while (v);
        while (k && p < e)
          *p++ = num[--k];
        break;
      case WIFI_CONN_NODHCP:
        p = put(p, e, "no address came over DHCP");
        break;
      case WIFI_CONN_NOPING:
        p = put(p, e, "the router did not answer");
        break;
      default:
        p = put(p, e, "joining stopped early");
        break;
    }
  }
  *p = '\0';
}
