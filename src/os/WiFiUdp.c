/* SPDX-License-Identifier: GPL-2.0 */
/* LICENSE: GPL-2.0, part of the Wi-Fi driver. See docs/wifi.md "License and
 * credits".
 *
 * The ARM9 side of WIFI_NETOP_UDP_BIND (wifi.h), for apps. As in WiFiTcp.c,
 * the ARM9 caches FCRAM and the core does not: every call cleans what this
 * side wrote and invalidates what it is about to read. */
#include "aurora.h"
#include "wifi.h"

static WifiUdp *const u = (WifiUdp *)WIFI_UDP_ADDR;

static void drain(void) {
  __asm__ volatile("mcr p15, 0, %0, c7, c10, 4" ::"r"(0) : "memory");
}

static void clean(const volatile void *p, u32 n) {
  u32 a = (u32)p & ~31u, e = (u32)p + n;
  for (; a < e; a += 32u)
    __asm__ volatile("mcr p15, 0, %0, c7, c10, 1" ::"r"(a) : "memory");
  drain();
}

/* Only for memory this side never writes. */
static void inval(const volatile void *p, u32 n) {
  u32 a = (u32)p & ~31u, e = (u32)p + n;
  for (; a < e; a += 32u)
    __asm__ volatile("mcr p15, 0, %0, c7, c6, 1" ::"r"(a) : "memory");
  drain();
}

static void core_line(void) { inval(&u->state, 32u); }

static u32 pad4(u32 n) { return (n + 3u) & ~3u; }

/* n bytes of a ring from total `at`, which may wrap past its end. */
static void ring_op(void (*op)(const volatile void *, u32),
                    const volatile u8 *ring, u32 size, u32 at, u32 n) {
  u32 pos = at & (size - 1u), k = size - pos;
  if (k > n)
    k = n;
  op(ring + pos, k);
  if (n > k)
    op(ring, n - k);
}

void wifi_udp_setup(u32 port) {
  WifiNetIo *io = (WifiNetIo *)WIFI_NET_ADDR;
  u->tx_head = u->rx_tail = 0;
  u->alive = 0;
  clean(u, 32u);
  io->port = port;
  io->status = WIFI_NETS_NOLINK;
  clean(io, sizeof(*io));
}

u32 wifi_udp_state(void) {
  core_line();
  return u->state;
}

u32 wifi_udp_port(void) {
  core_line();
  return u->port;
}

int wifi_udp_room(u32 n) {
  u32 used;
  core_line();
  used = u->tx_head - u->tx_tail;
  return used <= WIFI_UDP_TX && WIFI_UDP_HEAD + pad4(n) <= WIFI_UDP_TX - used;
}

int wifi_udp_send(u32 ip, u32 port, const void *p, u32 n) {
  const u8 *s = (const u8 *)p;
  u32 head = u->tx_head;
  u8 h[WIFI_UDP_HEAD];
  if (n > WIFI_UDP_SEND_MAX || !wifi_udp_room(n) || u->state != WIFI_UDP_ON)
    return 0;
  h[0] = (u8)(ip >> 24);
  h[1] = (u8)(ip >> 16);
  h[2] = (u8)(ip >> 8);
  h[3] = (u8)ip;
  h[4] = (u8)(port >> 8);
  h[5] = (u8)port;
  h[6] = (u8)(n >> 8);
  h[7] = (u8)n;
  for (u32 i = 0; i < WIFI_UDP_HEAD; i++)
    u->tx[(head + i) & (WIFI_UDP_TX - 1u)] = h[i];
  for (u32 i = 0; i < n; i++)
    u->tx[(head + WIFI_UDP_HEAD + i) & (WIFI_UDP_TX - 1u)] = s[i];
  /* The record reaches memory before the counter. */
  ring_op(clean, u->tx, WIFI_UDP_TX, head, WIFI_UDP_HEAD + n);
  u->tx_head = head + WIFI_UDP_HEAD + pad4(n);
  clean(&u->tx_head, 4u);
  return 1;
}

int wifi_udp_recv(void *p, u32 max, u32 *ip, u32 *port, int take) {
  u8 *d = (u8 *)p, h[WIFI_UDP_HEAD];
  u32 tail = u->rx_tail, have, len;
  core_line();
  have = u->rx_head - tail;
  if (have < WIFI_UDP_HEAD || have > WIFI_UDP_RX)
    return -1;
  ring_op(inval, u->rx, WIFI_UDP_RX, tail, WIFI_UDP_HEAD);
  for (u32 i = 0; i < WIFI_UDP_HEAD; i++)
    h[i] = u->rx[(tail + i) & (WIFI_UDP_RX - 1u)];
  len = (u32)h[6] << 8 | h[7];
  if (WIFI_UDP_HEAD + pad4(len) > have)
    return -1;
  if (len && max)
    ring_op(inval, u->rx, WIFI_UDP_RX, tail + WIFI_UDP_HEAD,
            len < max ? len : max);
  if (ip)
    *ip = (u32)h[0] << 24 | (u32)h[1] << 16 | (u32)h[2] << 8 | h[3];
  if (port)
    *port = (u32)h[4] << 8 | h[5];
  for (u32 i = 0; i < len && i < max; i++)
    d[i] = u->rx[(tail + WIFI_UDP_HEAD + i) & (WIFI_UDP_RX - 1u)];
  if (take) {
    u->rx_tail = tail + WIFI_UDP_HEAD + pad4(len);
    clean(&u->rx_tail, 4u);
  }
  return (int)len;
}

void wifi_udp_alive(void) {
  u->alive = u->alive + 1u;
  clean(&u->alive, 4u);
}
