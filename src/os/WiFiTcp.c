/* SPDX-License-Identifier: GPL-2.0 */
/* LICENSE: GPL-2.0, part of the Wi-Fi driver. See docs/wifi.md "License and
 * credits".
 *
 * The ARM9 side of WIFI_NETOP_TCP (wifi.h), for the OS and apps alike. The
 * ARM9 caches FCRAM and the core does not, so every call cleans what this
 * side wrote and invalidates what it is about to read. */
#include "aurora.h"
#include "wifi.h"

static WifiTcp *const t = (WifiTcp *)WIFI_TCP_ADDR;

/* ARM946E-S data cache: 32-byte lines, 4 KB. */
static void drain(void) {
  __asm__ volatile("mcr p15, 0, %0, c7, c10, 4" ::"r"(0) : "memory");
}

static void clean(const volatile void *p, u32 n) {
  u32 a = (u32)p & ~31u, e = (u32)p + n;
  for (; a < e; a += 32u)
    __asm__ volatile("mcr p15, 0, %0, c7, c10, 1" ::"r"(a) : "memory");
  drain();
}

/* Only for memory this side never writes, so no line in it is dirty. Past a
 * few KB, cleaning and dropping the whole cache by index is cheaper. */
static void inval(const volatile void *p, u32 n) {
  if (n > 2048u) {
    for (u32 i = 0; i < 0x400u; i += 32u)
      for (u32 way = 0; way < 4u; way++)
        __asm__ volatile("mcr p15, 0, %0, c7, c14, 2" ::"r"(way << 30 | i)
                         : "memory");
  } else {
    u32 a = (u32)p & ~31u, e = (u32)p + n;
    for (; a < e; a += 32u)
      __asm__ volatile("mcr p15, 0, %0, c7, c6, 1" ::"r"(a) : "memory");
  }
  drain();
}

static void core_line(void) { inval(&t->state, 32u); }

void wifi_tcp_setup(u32 ip, u32 port, u32 timeout_ms) {
  WifiNetIo *io = (WifiNetIo *)WIFI_NET_ADDR;
  t->tx_head = t->rx_tail = 0;
  t->close = WIFI_TCP_KEEP;
  t->alive = 0;
  t->state = WIFI_TCP_IDLE;
  t->tx_tail = t->rx_head = t->eof = 0;
  clean(t, 64u);
  io->ip = ip;
  io->port = port;
  io->timeout_ms = timeout_ms;
  io->status = WIFI_NETS_NOLINK;
  io->stage = 0;
  io->resent = 0;
  clean(io, sizeof(*io));
}

u32 wifi_tcp_state(void) {
  core_line();
  return t->state;
}

u32 wifi_tcp_write(const void *p, u32 n) {
  const u8 *s = (const u8 *)p;
  u32 head = t->tx_head, room, done = 0;
  core_line();
  room = WIFI_TCP_TX - (head - t->tx_tail);
  if (room > WIFI_TCP_TX || t->state == WIFI_TCP_CLOSED)
    return 0;
  if (n > room)
    n = room;
  while (done < n) {
    u32 pos = (head + done) & (WIFI_TCP_TX - 1u), k = WIFI_TCP_TX - pos;
    if (k > n - done)
      k = n - done;
    for (u32 i = 0; i < k; i++)
      t->tx[pos + i] = s[done + i];
    clean(&t->tx[pos], k);
    done += k;
  }
  if (done) {
    t->tx_head = head + done;
    clean(&t->tx_head, 4u);
  }
  return done;
}

static u32 rx_copy(void *p, u32 n) {
  u8 *d = (u8 *)p;
  u32 tail = t->rx_tail, have, done = 0;
  core_line();
  have = t->rx_head - tail;
  if (have > WIFI_TCP_RX)
    return 0;
  if (n > have)
    n = have;
  while (done < n) {
    u32 pos = (tail + done) & (WIFI_TCP_RX - 1u), k = WIFI_TCP_RX - pos;
    if (k > n - done)
      k = n - done;
    inval(&t->rx[pos], k);
    for (u32 i = 0; i < k; i++)
      d[done + i] = t->rx[pos + i];
    done += k;
  }
  return done;
}

u32 wifi_tcp_read(void *p, u32 n) {
  u32 done = rx_copy(p, n);
  if (done) {
    t->rx_tail = t->rx_tail + done;
    clean(&t->rx_tail, 4u);
  }
  return done;
}

u32 wifi_tcp_peek(void *p, u32 n) { return rx_copy(p, n); }

u32 wifi_tcp_rx_ready(void) {
  u32 have;
  core_line();
  have = t->rx_head - t->rx_tail;
  return have > WIFI_TCP_RX ? 0 : have;
}

u32 wifi_tcp_tx_room(void) {
  u32 room;
  core_line();
  room = WIFI_TCP_TX - (t->tx_head - t->tx_tail);
  return room > WIFI_TCP_TX || t->state == WIFI_TCP_CLOSED ? 0 : room;
}

int wifi_tcp_eof(void) {
  core_line();
  return t->eof && t->rx_head == t->rx_tail;
}

void wifi_tcp_close(u32 how) {
  t->close = how;
  clean(&t->close, 4u);
}

void wifi_tcp_alive(void) {
  t->alive = t->alive + 1u;
  clean(&t->alive, 4u);
}

u32 wifi_tcp_end_status(void) {
  core_line();
  return t->end_status;
}
