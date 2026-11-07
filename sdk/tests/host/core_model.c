/* See model.h. */
#define _GNU_SOURCE /* memmem */
#include "audio.h"
#include "model.h"
#include "wifi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define MAP_ADDR 0x23000000u
#define MAP_SIZE 0x01000000u
#define SEG      1460u
#define OUR_IP   0xC0A80132u /* 192.168.1.50 */
/* A host on our subnet that does not answer ARP. */
#define NOHOST_IP 0xC0A80163u /* 192.168.1.99 */

ModelStats g_model;

static uint32_t g_version;
static int g_session;

typedef struct {
  uint32_t ip, port;
  int kind;
  uint32_t arg;
} ServerDef;

static ServerDef g_defs[16];
static int g_ndefs;

static struct {
  char name[64];
  uint32_t ip;
} g_names[16];
static int g_nnames;

static ServerDef g_peers[16];
static int g_npeers;

/* The UDP port, and datagrams on their way to it: what a peer sent arrives
 * a millisecond later. */
static struct {
  uint32_t alive;
  uint64_t alive_t;
  int blasted;
} g_udp;

typedef struct {
  uint64_t at;
  uint32_t ip, port, len;
  uint8_t data[1472];
} Flight;

static Flight g_flight[64];
static int g_nflight;

static struct {
  int on, open, server_fin, server_rst, replied;
  uint32_t status; /* what it ends with if it times out, as TcpRun.status */
  const ServerDef *sv;
  uint64_t t0, alive_t, fin_t;
  uint32_t alive, timeout_ms, in_len, in_total;
  uint8_t in[65536];
  uint8_t *out;
  uint32_t out_len, out_pos, out_cap;
} c;

static AudioCtrl *ctl(void) { return (AudioCtrl *)(uintptr_t)AUDIO_CTRL_ADDR; }
static WifiNetIo *nio(void) { return (WifiNetIo *)(uintptr_t)WIFI_NET_ADDR; }
static WifiTcp *tcp(void) { return (WifiTcp *)(uintptr_t)WIFI_TCP_ADDR; }
static WifiShared *ws(void) { return (WifiShared *)(uintptr_t)WIFI_SHARED_ADDR; }
static WifiUdp *udp(void) { return (WifiUdp *)(uintptr_t)WIFI_UDP_ADDR; }

uint8_t model_pattern(uint32_t i) { return (uint8_t)(i * 7u + (i >> 8)); }

void model_reset(uint32_t version) {
  static int mapped;
  if (!mapped) {
    void *p = mmap((void *)(uintptr_t)MAP_ADDR, MAP_SIZE,
                   PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (p != (void *)(uintptr_t)MAP_ADDR) {
      printf("model: cannot map 0x%08X\n", MAP_ADDR);
      exit(2);
    }
    mapped = 1;
  }
  memset((void *)(uintptr_t)MAP_ADDR, 0, MAP_SIZE);
  free(c.out);
  memset(&c, 0, sizeof(c));
  memset(&g_model, 0, sizeof(g_model));
  g_ndefs = g_nnames = g_npeers = g_nflight = 0;
  memset(&g_udp, 0, sizeof(g_udp));
  g_version = version;
  g_session = 1;
  ctl()->magic = AUDIO_MAGIC;
  ctl()->version = version;
  ws()->session = 1;
  ws()->ip = OUR_IP;
}

void model_offline(void) {
  g_session = 0;
  ws()->session = 0;
  ws()->ip = 0;
}

void model_dns(const char *name, uint32_t ip) {
  snprintf(g_names[g_nnames].name, sizeof(g_names[0].name), "%s", name);
  g_names[g_nnames++].ip = ip;
}

void model_server(uint32_t ip, uint32_t port, int kind, uint32_t arg) {
  g_defs[g_ndefs++] = (ServerDef){ip, port, kind, arg};
}

void model_udp_peer(uint32_t ip, uint32_t port, int kind, uint32_t arg) {
  g_peers[g_npeers++] = (ServerDef){ip, port, kind, arg};
}

static void fly(uint32_t ip, uint32_t port, const uint8_t *p, uint32_t len) {
  Flight *f;
  if (g_nflight == 64)
    return;
  f = &g_flight[g_nflight++];
  f->at = host_now_us() + 1000u;
  f->ip = ip;
  f->port = port;
  f->len = len;
  memcpy(f->data, p, len);
}

static void udp_stop(void) {
  udp()->state = WIFI_UDP_OFF;
  g_model.udp_on = 0;
  g_nflight = 0;
}

/* As udp_deliver in WiFi11.c. */
static void udp_arrive(const Flight *f) {
  WifiUdp *u = udp();
  uint32_t need = WIFI_UDP_HEAD + ((f->len + 3u) & ~3u);
  uint32_t head = u->rx_head, room = WIFI_UDP_RX - (head - u->rx_tail);
  uint8_t h[8] = {(uint8_t)(f->ip >> 24), (uint8_t)(f->ip >> 16),
                  (uint8_t)(f->ip >> 8),  (uint8_t)f->ip,
                  (uint8_t)(f->port >> 8), (uint8_t)f->port,
                  (uint8_t)(f->len >> 8), (uint8_t)f->len};
  if (room > WIFI_UDP_RX || need > room) {
    u->dropped++;
    return;
  }
  for (uint32_t i = 0; i < 8; i++)
    u->rx[(head + i) & (WIFI_UDP_RX - 1u)] = h[i];
  for (uint32_t i = 0; i < f->len; i++)
    u->rx[(head + 8 + i) & (WIFI_UDP_RX - 1u)] = f->data[i];
  u->rx_head = head + need;
}

static void peer_gets(const ServerDef *p, uint32_t to, uint32_t port,
                      const uint8_t *d, uint32_t len) {
  if (p->kind == UP_ECHO) {
    fly(p->ip, p->port, d, len);
  } else if (p->kind == UP_BLAST && !g_udp.blasted) {
    uint8_t b[600];
    g_udp.blasted = 1;
    for (uint32_t k = 0; k < p->arg; k++) {
      memset(b, (int)k, sizeof(b));
      fly(p->ip, p->port, b, sizeof(b));
    }
  }
}

static void udp_step(void) {
  WifiUdp *u = udp();
  uint64_t now = host_now_us();
  if (!g_model.udp_on)
    return;
  if (u->alive != g_udp.alive) {
    g_udp.alive = u->alive;
    g_udp.alive_t = now;
  }
  if (now - g_udp.alive_t >= WIFI_TCP_ALIVE_MS * 1000ull) {
    g_model.udp_alive_drops++;
    udp_stop();
    return;
  }
  for (int k = 0; k < 4 && u->tx_head != u->tx_tail; k++) {
    uint32_t tail = u->tx_tail, have = u->tx_head - tail, len, ip, port;
    uint8_t h[8], d[1472];
    if (have > WIFI_UDP_TX || have < 8u) {
      printf("model: UDP tx ring broken\n");
      exit(3);
    }
    for (uint32_t i = 0; i < 8; i++)
      h[i] = u->tx[(tail + i) & (WIFI_UDP_TX - 1u)];
    ip = (uint32_t)h[0] << 24 | (uint32_t)h[1] << 16 | (uint32_t)h[2] << 8 | h[3];
    port = (uint32_t)h[4] << 8 | h[5];
    len = (uint32_t)h[6] << 8 | h[7];
    if (len > WIFI_UDP_SEND_MAX || 8u + ((len + 3u) & ~3u) > have) {
      printf("model: bad UDP record (len %u)\n", (unsigned)len);
      exit(3);
    }
    for (uint32_t i = 0; i < len; i++)
      d[i] = u->tx[(tail + 8 + i) & (WIFI_UDP_TX - 1u)];
    u->tx_tail = tail + 8u + ((len + 3u) & ~3u);
    if (ip == NOHOST_IP) {
      u->unsent++; /* ARP unanswered */
      continue;
    }
    g_model.udp_sent++;
    if (ip == 0xFFFFFFFFu)
      g_model.udp_to_bcast++;
    for (int i = 0; i < g_npeers; i++)
      if ((g_peers[i].ip == ip || (ip == 0xFFFFFFFFu &&
                                   g_peers[i].kind == UP_ECHO)) &&
          g_peers[i].port == port)
        peer_gets(&g_peers[i], ip, port, d, len);
  }
  /* In the order they were sent. */
  while (g_nflight && g_flight[0].at <= now) {
    udp_arrive(&g_flight[0]);
    memmove(g_flight, g_flight + 1, (size_t)--g_nflight * sizeof(Flight));
  }
}

static void out_put(const uint8_t *p, uint32_t n) {
  if (c.out_len + n > c.out_cap) {
    c.out_cap = (c.out_len + n) * 2u + 4096u;
    c.out = realloc(c.out, c.out_cap);
  }
  memcpy(c.out + c.out_len, p, n);
  c.out_len += n;
}

static void end(uint32_t st) {
  nio()->status = st;
  if (g_version >= AUDIO_UDP_VERSION)
    tcp()->end_status = st;
  tcp()->state = WIFI_TCP_CLOSED;
  c.on = 0;
  g_model.bg = 0;
  g_model.last_status = st;
}

static void on_accept(void) {
  if (c.sv->kind == SV_BULK) {
    for (uint32_t i = 0; i < c.sv->arg; i++) {
      uint8_t b = model_pattern(i);
      out_put(&b, 1);
    }
    c.server_fin = 1;
  }
}

static void server_input(const uint8_t *p, uint32_t n) {
  uint32_t keep = n;
  g_model.got += n;
  c.in_total += n;
  if (c.in_len + keep > sizeof(c.in))
    keep = sizeof(c.in) - c.in_len;
  memcpy(c.in + c.in_len, p, keep);
  c.in_len += keep;
  switch (c.sv->kind) {
  case SV_ECHO:
    out_put(p, n);
    break;
  case SV_RESET:
    out_put(p, n);
    if (c.in_total >= c.sv->arg)
      c.server_rst = 1;
    break;
  case SV_HTTP:
    if (!c.replied && c.in_len >= 4u &&
        memmem(c.in, c.in_len, "\r\n\r\n", 4)) {
      char head[96];
      int hl = snprintf(head, sizeof(head),
                        "HTTP/1.0 200 OK\r\nContent-Length: %u\r\n\r\n",
                        (unsigned)c.sv->arg);
      out_put((const uint8_t *)head, (uint32_t)hl);
      for (uint32_t i = 0; i < c.sv->arg; i++) {
        uint8_t b = model_pattern(i);
        out_put(&b, 1);
      }
      c.replied = c.server_fin = 1;
    }
    break;
  default:
    break;
  }
}

static void bg_step(void) {
  WifiTcp *t = tcp();
  uint64_t now = host_now_us();
  uint32_t have, take, room, give;
  if (!c.on)
    return;
  if (t->alive != c.alive) {
    c.alive = t->alive;
    c.alive_t = now;
  }
  if (t->close == WIFI_TCP_ABORT) {
    g_model.aborts++;
    end(WIFI_NETS_OK);
    return;
  }
  if (now - c.alive_t >= WIFI_TCP_ALIVE_MS * 1000ull) {
    g_model.alive_drops++;
    end(c.status);
    return;
  }
  if (!c.open) {
    if (!c.sv || c.sv->kind == SV_REFUSE) {
      if (now - c.t0 >= 1000u)
        end(WIFI_NETS_REFUSED);
      return;
    }
    if (c.sv->kind == SV_SILENT) {
      if (now - c.t0 >= c.timeout_ms * 1000ull)
        end(c.status);
      return;
    }
    if (now - c.t0 < 2000u)
      return;
    c.open = 1;
    t->state = WIFI_TCP_OPEN;
    on_accept();
  }

  have = t->tx_head - t->tx_tail;
  if (have > WIFI_TCP_TX) {
    printf("model: tx ring broken (head %u tail %u)\n", (unsigned)t->tx_head,
           (unsigned)t->tx_tail);
    exit(3);
  }
  take = have > SEG ? SEG : have;
  if (c.sv->kind == SV_SINK && take > c.sv->arg)
    take = c.sv->arg;
  if (c.server_rst)
    take = 0;
  if (take) {
    uint8_t buf[SEG];
    for (uint32_t i = 0; i < take; i++)
      buf[i] = t->tx[(t->tx_tail + i) & (WIFI_TCP_TX - 1u)];
    t->tx_tail += take;
    server_input(buf, take);
  }

  room = WIFI_TCP_RX - (t->rx_head - t->rx_tail);
  if (room > WIFI_TCP_RX) {
    printf("model: rx ring broken\n");
    exit(3);
  }
  give = c.out_len - c.out_pos;
  if (give > SEG)
    give = SEG;
  if (give > room)
    give = room;
  for (uint32_t i = 0; i < give; i++)
    t->rx[(t->rx_head + i) & (WIFI_TCP_RX - 1u)] = c.out[c.out_pos + i];
  t->rx_head += give;
  c.out_pos += give;
  if (c.out_pos == c.out_len) {
    if (c.server_rst) {
      end(WIFI_NETS_REFUSED);
      return;
    }
    if (c.server_fin)
      t->eof = 1;
  }

  if (t->close == WIFI_TCP_FIN) {
    if (!c.fin_t)
      c.fin_t = now;
    if ((t->tx_head == t->tx_tail && (t->eof || now - c.fin_t >= 500000u)) ||
        now - c.fin_t >= 3000000u) {
      g_model.fins++;
      end(WIFI_NETS_OK);
    }
  }
}

static void tcp_start(void) {
  WifiNetIo *io = nio();
  WifiTcp *t = tcp();
  t->tx_tail = t->rx_head = t->eof = 0;
  t->state = WIFI_TCP_CONNECTING;
  if (!io->port || io->port > 0xFFFFu) {
    io->status = WIFI_NETS_TOOBIG;
    t->state = WIFI_TCP_CLOSED;
    return;
  }
  if (io->ip == NOHOST_IP) {
    io->status = WIFI_NETS_NOHOST;
    if (g_version >= AUDIO_UDP_VERSION)
      t->end_status = WIFI_NETS_NOHOST;
    t->state = WIFI_TCP_CLOSED;
    return;
  }
  free(c.out);
  memset(&c, 0, sizeof(c));
  for (int i = 0; i < g_ndefs; i++)
    if (g_defs[i].ip == io->ip && g_defs[i].port == io->port)
      c.sv = &g_defs[i];
  c.on = 1;
  c.t0 = c.alive_t = host_now_us();
  c.alive = t->alive;
  c.timeout_ms = io->timeout_ms < 100u ? 2000u
                 : io->timeout_ms > 20000u ? 20000u
                                           : io->timeout_ms;
  io->status = c.status = WIFI_NETS_TIMEOUT;
  g_model.connects++;
  g_model.bg = 1;
}

static void udp_bind(void) {
  WifiNetIo *io = nio();
  WifiUdp *u = udp();
  uint32_t port = io->port;
  if (port > 0xFFFFu || port == 68u) {
    io->status = WIFI_NETS_TOOBIG;
    return;
  }
  u->tx_tail = u->rx_head = u->dropped = u->unsent = 0;
  u->port = port ? port : 50123u;
  u->state = WIFI_UDP_ON;
  g_nflight = 0;
  g_udp.alive = u->alive;
  g_udp.alive_t = host_now_us();
  g_udp.blasted = 0;
  g_model.udp_on = 1;
  g_model.udp_binds++;
  io->status = WIFI_NETS_OK;
}

static void net_op(uint32_t op) {
  WifiNetIo *io = nio();
  if (op < 16u)
    g_model.ops[op]++;
  if (c.on) {
    if (op == WIFI_NETOP_STATUS)
      return; /* WifiNetIo still reports on the connection */
    if (g_version < AUDIO_UDP_VERSION ||
        (op != WIFI_NETOP_DNS && op != WIFI_NETOP_UDP_BIND &&
         op != WIFI_NETOP_UDP_UNBIND)) {
      g_model.bg_stopped++;
      end(WIFI_NETS_OK);
    }
  }
  io->status = WIFI_NETS_NOLINK;
  if (!g_session)
    return;
  switch (op) {
  case WIFI_NETOP_STATUS:
    io->status = WIFI_NETS_OK;
    break;
  case WIFI_NETOP_DNS: {
    char name[256];
    uint32_t i = 0;
    for (; i < sizeof(name) - 1u && io->name[i]; i++)
      name[i] = io->name[i];
    name[i] = 0;
    io->status = WIFI_NETS_NONAME;
    io->dns_rcode = 3;
    for (int k = 0; k < g_nnames; k++)
      if (!strcmp(g_names[k].name, name)) {
        io->ip = g_names[k].ip;
        io->dns_rcode = 0;
        io->status = WIFI_NETS_OK;
      }
    break;
  }
  case WIFI_NETOP_PING:
    io->status = WIFI_NETS_OK;
    io->rtt_us = 1000;
    break;
  case WIFI_NETOP_TCP_BG:
    tcp_start();
    break;
  case WIFI_NETOP_UDP_BIND:
    if (g_version >= AUDIO_UDP_VERSION)
      udp_bind();
    break;
  case WIFI_NETOP_UDP_UNBIND:
    if (g_version >= AUDIO_UDP_VERSION) {
      udp_stop();
      g_model.udp_unbinds++;
      io->status = WIFI_NETS_OK;
    }
    break;
  case WIFI_NETOP_LEAVE:
    udp_stop();
    io->status = WIFI_NETS_OK;
    break;
  default:
    break;
  }
}

void model_step(void) {
  AudioCtrl *a = ctl();
  if (a->cmd_seq != a->ack_seq) {
    if (a->cmd == AUDIO_CMD_WIFI_NET)
      net_op(a->arg0);
    a->ack_seq = a->cmd_seq;
  }
  bg_step();
  udp_step();
}
