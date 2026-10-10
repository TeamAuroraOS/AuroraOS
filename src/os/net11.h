/* SPDX-License-Identifier: GPL-2.0 */
/* LICENSE: GPL-2.0, part of the Wi-Fi driver. See docs/wifi.md "License and
 * credits". */
#ifndef AURORA_NET11_H
#define AURORA_NET11_H

#include <stdint.h>

/* The ARM11 core's IP stack, just enough to get an address, look up a name,
 * ping and make one short TCP exchange: DHCP, ARP, DNS, ICMP echo and a TCP
 * client. Frames in and out are Ethernet II; WiFi11.c converts them to and
 * from 802.3 with an LLC/SNAP header. Addresses are host order,
 * a << 24 | b << 16 | c << 8 | d for a.b.c.d. */
typedef struct {
  uint8_t mac[6];
  uint32_t xid;     /* DHCP transaction */
  uint32_t offered; /* address in the last offer */
  uint32_t server;  /* DHCP server identifier */
  uint32_t ip, mask, gw, dns, lease;
  uint8_t gw_mac[6];
  int have_gw_mac;
  uint16_t ping_id;
  /* The last echo reply or ICMP error for our echo: its sender, the TTL it
   * arrived with, and its ICMP bytes. */
  uint32_t reply_from, reply_ttl, reply_len;
  /* One more MAC, for a host on our own subnet. */
  uint32_t arp_ip;
  uint8_t arp_mac[6];
  int have_arp;
  /* The DNS query in flight, and the RCODE of its answer. */
  uint16_t dns_id, dns_port;
  uint32_t dns_rcode;
  /* One TCP connection at a time, ours to tcp_ip:tcp_rport. Our first data
   * byte is tcp_iss + 1. What the server sends goes into tcp_rx in order;
   * a segment that skips ahead is dropped and the server sends it again. */
  uint32_t tcp_state; /* NET_TCP_* */
  uint32_t tcp_ip;
  uint16_t tcp_lport, tcp_rport;
  uint32_t tcp_iss, snd_una, snd_nxt, rcv_nxt;
  uint32_t tcp_mss, tcp_wnd; /* the server's */
  int tcp_fin;               /* the server closed its side */
  uint8_t *tcp_rx;
  uint32_t tcp_rx_cap, tcp_rx_len;
} NetState;

enum {
  NET_TCP_CLOSED = 0,
  NET_TCP_SYN_SENT,
  NET_TCP_OPEN,
  NET_TCP_RESET, /* the server sent RST */
};

/* TCP flags, as in the header. */
#define NET_TCP_FIN 0x01u
#define NET_TCP_SYN 0x02u
#define NET_TCP_RST 0x04u
#define NET_TCP_PSH 0x08u
#define NET_TCP_ACK 0x10u

enum {
  NET_RX_NONE = 0,
  NET_RX_OFFER,      /* value: offered address          */
  NET_RX_ACK,        /* value: our address              */
  NET_RX_NAK,        /* value: server                   */
  NET_RX_ARP_REPLY,  /* value: who answered             */
  NET_RX_ARP_ASKED,  /* value: who asked; reply built   */
  NET_RX_PING_REPLY, /* value: sequence number          */
  NET_RX_PINGED,     /* value: who pinged; reply built  */
  NET_RX_OTHER,      /* value: ethertype | IP protocol << 16 */
  NET_RX_DNS,        /* value: the first A record, 0 if none; dns_rcode set */
  NET_RX_PING_ERR,   /* value: ICMP type << 8 | code | sequence << 16, for
                        an unreachable or time-exceeded about our echo */
  NET_RX_TCP,        /* value: the segment's flags; an ACK may be built */
};

/* Longest frame built here but for TCP data (a DHCP message). */
#define NET_FRAME_MAX 400
/* Echo payload, as Linux's ping sends by default: 64 ICMP bytes. */
#define NET_PING_DATA 56u
/* Most TCP data put in one segment, and the longest frame that makes. */
#define NET_TCP_SEG 512u
#define NET_TX_MAX  (14u + 20u + 20u + NET_TCP_SEG)
/* The MSS we announce: a whole segment, with room to spare, still fits one
 * HTC frame (WiFi11.c HTC_FRAME_MAX). */
#define NET_TCP_MSS 1200u
/* The most window announced, whatever the reply buffer holds: what the chip
 * has been seen to keep while a frame takes 0.3 s to read. A larger burst can
 * overflow it, and a lost segment drops everything behind it. */
#define NET_TCP_WND 8192u

/* Each returns the frame length. */
uint32_t net_dhcp(NetState *n, uint8_t *f, int request);
uint32_t net_arp_request(NetState *n, uint8_t *f, uint32_t ip);
uint32_t net_ping(NetState *n, uint8_t *f, uint32_t dst, const uint8_t *dmac,
                  uint16_t seq);
/* An A query for `name` from dns_port with dns_id; 0 when the name is not a
 * valid host name. */
uint32_t net_dns(NetState *n, uint8_t *f, uint32_t server,
                 const uint8_t *dmac, const char *name);

/* Starts the connection: a SYN goes to dmac (net_tcp_seg with NET_TCP_SYN),
 * and the reply is read into rx, at most rx_cap bytes. */
void net_tcp_open(NetState *n, uint32_t ip, uint16_t lport, uint16_t rport,
                  uint32_t iss, uint8_t *rx, uint32_t rx_cap);
/* A segment of the connection: `flags`, sequence number `seq` and `len`
 * bytes of `data` (at most NET_TCP_SEG), acknowledging rcv_nxt. A SYN
 * carries our MSS. Returns the frame length; f holds NET_TX_MAX bytes. */
uint32_t net_tcp_seg(NetState *n, uint8_t *f, const uint8_t *dmac,
                     uint32_t flags, uint32_t seq, const uint8_t *data,
                     uint32_t len);
/* 1 once `b` holds an HTTP response's headers and as much body as their
 * Content-Length says. */
int net_http_done(const uint8_t *b, uint32_t len);

/* Takes one received frame. An answer it owes (ARP, echo, a TCP ACK) goes
 * into `reply`, NET_FRAME_MAX bytes, with its length in *reply_len (0 if
 * none). */
int net_rx(NetState *n, const uint8_t *f, uint32_t len, uint8_t *reply,
           uint32_t *reply_len, uint32_t *value);

#endif
