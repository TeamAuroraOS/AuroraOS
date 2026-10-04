/* SPDX-License-Identifier: GPL-2.0 */
/* LICENSE: GPL-2.0, part of the Wi-Fi driver. See docs/wifi.md "License and
 * credits". */
#ifndef AURORA_NET11_H
#define AURORA_NET11_H

#include <stdint.h>

/* The ARM11 core's IP stack, just enough to get an address, look up a name
 * and ping: DHCP, ARP, DNS and ICMP echo. Frames in and out are Ethernet II;
 * WiFi11.c converts them to and from 802.3 with an LLC/SNAP header.
 * Addresses are host order, a << 24 | b << 16 | c << 8 | d for a.b.c.d. */
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
} NetState;

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
};

/* Longest frame built here (a DHCP message). */
#define NET_FRAME_MAX 400
/* Echo payload, as Linux's ping sends by default: 64 ICMP bytes. */
#define NET_PING_DATA 56u

/* Each returns the frame length. */
uint32_t net_dhcp(NetState *n, uint8_t *f, int request);
uint32_t net_arp_request(NetState *n, uint8_t *f, uint32_t ip);
uint32_t net_ping(NetState *n, uint8_t *f, uint32_t dst, const uint8_t *dmac,
                  uint16_t seq);
/* An A query for `name` from dns_port with dns_id; 0 when the name is not a
 * valid host name. */
uint32_t net_dns(NetState *n, uint8_t *f, uint32_t server,
                 const uint8_t *dmac, const char *name);

/* Takes one received frame. An answer it owes (ARP, echo) goes into `reply`,
 * NET_FRAME_MAX bytes, with its length in *reply_len (0 if none). */
int net_rx(NetState *n, const uint8_t *f, uint32_t len, uint8_t *reply,
           uint32_t *reply_len, uint32_t *value);

#endif
