/* SPDX-License-Identifier: GPL-2.0 */
/* LICENSE: GPL-2.0, ath6kl-derived; credit Octoblimp. See docs/wifi.md
 * "License and credits". */
#include "aurora.h"
#include "anim.h"
#include "audio.h"
#include "gpu.h"
#include "timer.h"
#include "ui.h"
#include "wifi.h"

extern void os_cache_sync(void);

static AudioCtrl *const ctrl = (AudioCtrl *)AUDIO_CTRL_ADDR;

static u32 posted; /* WifiShared.seq when the running command was posted */
static int online; /* WifiShared.session as last read */

static u32 shared_seq(void) {
  os_cache_sync();
  return ((volatile WifiShared *)WIFI_SHARED_ADDR)->seq;
}

/* arg0 goes in the command block, not WifiShared, where a line the ARM11 has
 * dirtied could be written back over it. */
static void post(u32 cmd, u32 arg0) {
  posted = shared_seq();
  ctrl->arg0 = arg0;
  ctrl->cmd = cmd;
  ctrl->cmd_seq = ctrl->cmd_seq + 1;
  os_cache_sync();
}

void wifi_probe(void) { post(AUDIO_CMD_WIFI, 0); }

/* The firmware must already be staged. */
void wifi_boot(u32 opts) { post(AUDIO_CMD_WIFI_BOOT, opts); }

int wifi_done(void) { return shared_seq() != posted; }

int wifi_online(void) { return online; }

void wifi_get(WifiShared *out) {
  os_cache_sync();
  volatile unsigned char *s = (volatile unsigned char *)WIFI_SHARED_ADDR;
  unsigned char *d = (unsigned char *)out;
  for (unsigned i = 0; i < sizeof(WifiShared); i++)
    d[i] = s[i];
  out->sdmmcctl = *(volatile uint16_t *)0x10000020u; /* CFG9 SDMMCCTL (ARM9) */
  online = out->session == 1u;
}

/* A Wi-Fi command keeps the core busy for up to minutes, and the core is what
 * presents frames: a present posted meanwhile is never run and its wait stalls
 * every redraw, and a GPU request can even replace a command the core has not
 * picked up yet. So while one runs, framebuffer A is put on screen and drawn
 * into directly, as the crash screen does, until the core is back. */
static int direct, direct_back;

void wifi_direct_on(void) {
  if (direct)
    return;
  direct_back = g_fb_bot != VRAM_BOT_PHYS;
  anim_flush();
  gpu_wait_idle();
  if (direct_back)
    gpu_show_a();
  screen_use_backbuffer(0);
  direct = 1;
}

int wifi_direct_off(void) {
  if (!direct)
    return 1;
  if (!wifi_done())
    return 0;
  if (direct_back)
    screen_use_backbuffer(1);
  direct = 0;
  return 1;
}

int wifi_wait(void (*tick)(u32 ms), u32 max_ms) {
  u32 t0 = timer_ticks(), shown = 0xFFFFFFFFu;
  for (;;) {
    if (wifi_done())
      return 1;
    u32 ms = timer_us_since(t0) / 1000u;
    if (ms >= max_ms)
      return 0;
    if (tick && ms / 100u != shown) {
      shown = ms / 100u;
      tick(ms);
    }
    ui_idle();
  }
}

/* Posts the operation whose operands are in WifiNetIo and waits for it. 1
 * when the core finished it. */
static int net_run(uint32_t op, uint32_t timeout_ms, void (*tick)(uint32_t ms)) {
  wifi_direct_on();
  post(AUDIO_CMD_WIFI_NET, op);
  int done = wifi_wait(tick, 3u * timeout_ms + 15000u);
  wifi_direct_off();
  os_cache_sync();
  return done;
}

uint32_t wifi_net(uint32_t op, uint32_t ip, uint32_t seq, const char *name,
                  uint32_t timeout_ms, WifiNetResult *r,
                  void (*tick)(uint32_t ms)) {
  WifiNetIo *io = (WifiNetIo *)WIFI_NET_ADDR;
  u32 n = 0;
  r->status = WIFI_NETS_BUSY;
  r->ip = r->rtt_us = r->ttl = r->bytes = r->from = r->icmp = 0;
  r->dns_rcode = 0;
  if (!wifi_direct_off())
    return r->status;
  io->ip = ip;
  io->seq = seq;
  io->timeout_ms = timeout_ms;
  io->status = WIFI_NETS_NOLINK;
  for (; name && name[n] && n < sizeof(io->name) - 1u; n++)
    io->name[n] = name[n];
  io->name[n] = '\0';
  if (!net_run(op, timeout_ms, tick))
    return r->status;
  r->status = io->status;
  r->ip = io->ip;
  r->rtt_us = io->rtt_us;
  r->ttl = io->ttl;
  r->bytes = io->bytes;
  r->from = io->from;
  r->icmp = io->icmp;
  r->dns_rcode = io->dns_rcode;
  online = ((volatile WifiShared *)WIFI_SHARED_ADDR)->session == 1u;
  return r->status;
}

uint32_t wifi_http(uint32_t ip, uint32_t port, const char *req,
                   uint32_t req_len, char *resp, uint32_t max, uint32_t *got,
                   uint32_t *stage, uint32_t timeout_ms,
                   void (*tick)(uint32_t ms)) {
  WifiNetIo *io = (WifiNetIo *)WIFI_NET_ADDR;
  volatile char *q = (volatile char *)WIFI_HTTP_REQ;
  const volatile char *a = (const volatile char *)WIFI_HTTP_RESP;
  *got = 0;
  *stage = 0;
  if (req_len == 0 || req_len > WIFI_HTTP_REQ_MAX)
    return WIFI_NETS_TOOBIG;
  if (!wifi_direct_off())
    return WIFI_NETS_BUSY;
  for (u32 i = 0; i < req_len; i++)
    q[i] = req[i];
  io->ip = ip;
  io->port = port;
  io->req_len = req_len;
  io->resp_len = 0;
  io->stage = 0;
  io->timeout_ms = timeout_ms;
  io->status = WIFI_NETS_NOLINK;
  if (!net_run(WIFI_NETOP_HTTP, timeout_ms, tick))
    return WIFI_NETS_BUSY;
  u32 n = io->resp_len;
  if (n > WIFI_HTTP_RESP_MAX)
    n = WIFI_HTTP_RESP_MAX;
  if (n > max)
    n = max;
  for (u32 i = 0; resp && i < n; i++)
    resp[i] = a[i];
  *got = n;
  *stage = io->stage;
  online = ((volatile WifiShared *)WIFI_SHARED_ADDR)->session == 1u;
  return io->status;
}
