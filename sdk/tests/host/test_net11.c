/* The ARM11 core's UDP frames (src/os/Net11.c, net_udp and net_rx), run as
 * they are: datagrams built by one NetState are taken apart by another. */
#include "check.h"
#include "net11.h"
#include "wifi.h"

#include <string.h>

#define IP(a, b, c, d) ((uint32_t)(a) << 24 | (b) << 16 | (c) << 8 | (d))

static NetState g_us, g_peer;
static const uint8_t mac_us[6] = {2, 0, 0, 0, 0, 0x50};
static const uint8_t mac_peer[6] = {2, 0, 0, 0, 0, 0x05};

static void setup(void) {
  memset(&g_us, 0, sizeof(g_us));
  memset(&g_peer, 0, sizeof(g_peer));
  memcpy(g_us.mac, mac_us, 6);
  g_us.ip = IP(192, 168, 1, 50);
  g_us.mask = IP(255, 255, 255, 0);
  g_us.gw = IP(192, 168, 1, 1);
  g_us.udp_lport = 5000;
  memcpy(g_peer.mac, mac_peer, 6);
  g_peer.ip = IP(10, 0, 0, 5);
}

static uint32_t get16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }

/* A datagram to us, as the peer at 10.0.0.5 would send it. */
static uint32_t to_us(uint8_t *f, uint32_t dst, uint32_t sport, uint32_t dport,
                      const char *text) {
  return net_udp(&g_peer, f, dst, mac_us, sport, dport,
                 (const uint8_t *)text, (uint32_t)strlen(text));
}

static int take(const uint8_t *f, uint32_t len) {
  uint8_t reply[NET_FRAME_MAX];
  uint32_t rl = 0, v = 0;
  return net_rx(&g_us, f, len, reply, &rl, &v);
}

static void t_build(void) {
  uint8_t f[NET_TX_MAX], big[NET_UDP_MAX + 1];
  const uint8_t *u = f + 14 + 20;
  uint32_t len, sum = 0;
  setup();
  CHECK_EQ(NET_UDP_MAX, WIFI_UDP_SEND_MAX);
  len = net_udp(&g_us, f, IP(10, 0, 0, 5), mac_peer, 5000, 7,
                (const uint8_t *)"hello", 5);
  CHECK_EQ(len, 14 + 20 + 8 + 5);
  CHECK(!memcmp(f, mac_peer, 6) && !memcmp(f + 6, mac_us, 6));
  CHECK_EQ(get16(f + 12), 0x0800);
  CHECK_EQ(f[14 + 9], 17);
  CHECK_EQ(get16(f + 14 + 2), 20 + 8 + 5);
  CHECK_EQ(get16(u), 5000);
  CHECK_EQ(get16(u + 2), 7);
  CHECK_EQ(get16(u + 4), 8 + 5);
  CHECK(get16(u + 6) != 0);
  CHECK(!memcmp(u + 8, "hello", 5));
  /* The pseudo header and the datagram sum to 0xFFFF with its checksum. */
  sum = (IP(192, 168, 1, 50) >> 16) + (IP(192, 168, 1, 50) & 0xFFFFu) +
        (IP(10, 0, 0, 5) >> 16) + (IP(10, 0, 0, 5) & 0xFFFFu) + 17u + 13u;
  for (uint32_t i = 0; i < 13; i += 2)
    sum += (uint32_t)u[i] << 8 | (i + 1 < 13 ? u[i + 1] : 0);
  while (sum >> 16)
    sum = (sum & 0xFFFFu) + (sum >> 16);
  CHECK_EQ(sum, 0xFFFF);
  memset(big, 'x', sizeof(big));
  CHECK_EQ(net_udp(&g_us, f, 1, mac_peer, 1, 2, big, NET_UDP_MAX),
           NET_TX_MAX);
  CHECK_EQ(net_udp(&g_us, f, 1, mac_peer, 1, 2, big, NET_UDP_MAX + 1), 0);
}

static void t_receive(void) {
  uint8_t f[NET_TX_MAX];
  uint32_t len;
  setup();
  len = to_us(f, g_us.ip, 7, 5000, "pong!");
  CHECK_EQ(take(f, len), NET_RX_UDP);
  CHECK_EQ(g_us.udp_src, IP(10, 0, 0, 5));
  CHECK_EQ(g_us.udp_sport, 7);
  CHECK_EQ(g_us.udp_len, 5);
  CHECK(!memcmp(f + g_us.udp_off, "pong!", 5));
  len = to_us(f, g_us.ip, 7, 5001, "x");
  CHECK_EQ(take(f, len), NET_RX_OTHER); /* another port */
  len = to_us(f, IP(192, 168, 1, 77), 7, 5000, "x");
  CHECK_EQ(take(f, len), NET_RX_OTHER); /* another host */
  len = to_us(f, 0xFFFFFFFFu, 7, 5000, "all");
  CHECK_EQ(take(f, len), NET_RX_UDP);
  len = to_us(f, IP(192, 168, 1, 255), 7, 5000, "net");
  CHECK_EQ(take(f, len), NET_RX_UDP);
  len = to_us(f, g_us.ip, 53, 5000, "dns port, not a lookup");
  CHECK_EQ(take(f, len), NET_RX_UDP);
  g_us.udp_lport = 0;
  len = to_us(f, g_us.ip, 7, 5000, "x");
  CHECK_EQ(take(f, len), NET_RX_OTHER); /* nothing bound */
  g_us.udp_lport = 5000;
  g_us.ip = 0;
  CHECK_EQ(take(f, len), NET_RX_OTHER); /* no address yet */
}

static void t_damaged(void) {
  uint8_t f[NET_TX_MAX];
  uint32_t len;
  setup();
  len = to_us(f, g_us.ip, 7, 5000, "checksum");
  f[14 + 20 + 8] ^= 1;
  CHECK_EQ(take(f, len), NET_RX_OTHER);
  f[14 + 20 + 8] ^= 1;
  f[14 + 20 + 6] = f[14 + 20 + 7] = 0; /* none sent: taken as it is */
  CHECK_EQ(take(f, len), NET_RX_UDP);
  len = to_us(f, g_us.ip, 7, 5000, "fragment");
  f[14 + 6] |= 0x20; /* more fragments */
  CHECK_EQ(take(f, len), NET_RX_OTHER);
  len = to_us(f, g_us.ip, 7, 5000, "short");
  f[14 + 20 + 4] = 0;
  f[14 + 20 + 5] = 4; /* a UDP length under its own header */
  CHECK_EQ(take(f, len), NET_RX_OTHER);
  len = to_us(f, g_us.ip, 7, 5000, "long");
  f[14 + 20 + 5] = 200; /* more than the IP packet holds */
  CHECK_EQ(take(f, len), NET_RX_OTHER);
}

SCENARIOS("net11", S(t_build), S(t_receive), S(t_damaged))
