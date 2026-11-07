/* What the code under test calls in the OS that the PC has no use for, and
 * the simulated clock. Waits and cache syncs let the model core take a
 * step, as the real one runs beside the ARM9. */
#include "aurora.h"
#include "ff.h"
#include "model.h"
#include "tls.h"
#include "wifi.h"

#include <stdint.h>

static uint64_t g_us = 1000000u;

uint64_t host_now_us(void) { return g_us; }
void host_advance(uint32_t us) { g_us += us; }

uint32_t timer_hz(void) { return 1000000u; }
uint32_t timer_ticks(void) { return (uint32_t)++g_us; }

void os_cache_sync(void) {
  g_us += 2u;
  model_step();
}

void gpu_wait_idle(void) {}

/* devkitARM's delay loop: about 30 ns a pass at 134 MHz. */
void delay(volatile u32 cycles) {
  g_us += cycles * 30u / 1000u + 1u;
  model_step();
}

DWORD get_fattime(void) {
  return ((DWORD)(2026 - 1980) << 25) | (11u << 21) | (7u << 16);
}

/* HTTPS is not under test here. */
u32 tls_exchange(const TlsLink *link, const char *host, u32 ip, u32 port,
                 const void *req, u32 req_len, int head_only, char *resp,
                 u32 max, u32 *got, u32 *stage, u32 timeout_ms, int *err) {
  *got = 0;
  *err = 0;
  return WIFI_NETS_TLS;
}
u32 tls_stream_open(const TlsLink *link, const char *host, u32 ip, u32 port,
                    u32 timeout_ms, int *err) {
  *err = 0;
  return WIFI_NETS_TLS;
}
int tls_stream_pump(void) { return 0; }
u32 tls_stream_read(void *buf, u32 max) { return 0; }
u32 tls_stream_write(const void *buf, u32 n) { return 0; }
int tls_stream_closed(int *err) {
  *err = 0;
  return 1;
}
void tls_stream_close(int graceful) {}
const char *tls_error_text(int err) { return "TLS is not in the host harness"; }

/* Joining is not modelled: the model core starts on a joined network. */
int wifi_fw_stage(uint32_t main_type) { return 0; }
void wifi_request(const char *ssid, const char *pass) {}
void wifi_request_clear(void) {}
void wifi_join_why(const WifiShared *w, const char *ssid, char *out,
                   uint32_t max) {
  if (max)
    out[0] = 0;
}
