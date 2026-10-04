/* SPDX-License-Identifier: GPL-2.0 */
/* LICENSE: GPL-2.0, part of the Wi-Fi driver. See docs/wifi.md "License and
 * credits".
 *
 * DHCP (RFC 2131/2132), ARP (RFC 826), DNS (RFC 1035) and ICMP echo (RFC
 * 792), from the RFCs. */
#include "net11.h"

#define ETH_IP  0x0800u
#define ETH_ARP 0x0806u
#define IP_ICMP 1u
#define IP_UDP  17u

#define BOOTP_LEN 300u /* the minimum size BOOTP relays and servers expect */

static void put16(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v >> 8);
  p[1] = (uint8_t)v;
}

static void put32(uint8_t *p, uint32_t v) {
  put16(p, v >> 16);
  put16(p + 2, v);
}

static uint32_t get16(const uint8_t *p) { return ((uint32_t)p[0] << 8) | p[1]; }

static uint32_t get32(const uint8_t *p) {
  return (get16(p) << 16) | get16(p + 2);
}

static void copy(uint8_t *d, const uint8_t *s, uint32_t n) {
  for (uint32_t i = 0; i < n; i++)
    d[i] = s[i];
}

static void zero(uint8_t *d, uint32_t n) {
  for (uint32_t i = 0; i < n; i++)
    d[i] = 0;
}

static int same(const uint8_t *a, const uint8_t *b, uint32_t n) {
  for (uint32_t i = 0; i < n; i++)
    if (a[i] != b[i])
      return 0;
  return 1;
}

/* The Internet checksum: one's complement of the one's complement sum. */
static uint32_t csum(const uint8_t *p, uint32_t n) {
  uint32_t s = 0;
  for (uint32_t i = 0; i + 1 < n; i += 2)
    s += get16(p + i);
  if (n & 1u)
    s += (uint32_t)p[n - 1] << 8;
  while (s >> 16)
    s = (s & 0xFFFFu) + (s >> 16);
  return ~s & 0xFFFFu;
}

static void eth(uint8_t *f, const uint8_t *dst, const uint8_t *src,
                uint32_t type) {
  copy(f, dst, 6);
  copy(f + 6, src, 6);
  put16(f + 12, type);
}

/* A 20-byte IP header at f + 14 for `len` bytes of payload. */
static void ip(uint8_t *f, uint32_t proto, uint32_t src, uint32_t dst,
               uint32_t len) {
  static uint16_t id;
  uint8_t *h = f + 14;
  h[0] = 0x45;
  h[1] = 0;
  put16(h + 2, 20u + len);
  put16(h + 4, ++id);
  put16(h + 6, 0);
  h[8] = 64;
  h[9] = (uint8_t)proto;
  put16(h + 10, 0);
  put32(h + 12, src);
  put32(h + 16, dst);
  put16(h + 10, csum(h, 20));
}

static const uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

uint32_t net_dhcp(NetState *n, uint8_t *f, int request) {
  uint8_t *u = f + 14 + 20, *b = u + 8, *o = b + 240;
  zero(f, 14u + 20u + 8u + BOOTP_LEN);
  eth(f, bcast, n->mac, ETH_IP);

  b[0] = 1; /* BOOTREQUEST */
  b[1] = 1; /* Ethernet */
  b[2] = 6;
  put32(b + 4, n->xid);
  put16(b + 10, 0x8000); /* broadcast: no address yet to receive unicast */
  copy(b + 28, n->mac, 6);
  put32(b + 236, 0x63825363); /* magic cookie */

  *o++ = 53; /* message type */
  *o++ = 1;
  *o++ = request ? 3 : 1;
  *o++ = 61; /* client identifier: hardware type, MAC */
  *o++ = 7;
  *o++ = 1;
  copy(o, n->mac, 6);
  o += 6;
  if (request) {
    *o++ = 50; /* requested address */
    *o++ = 4;
    put32(o, n->offered);
    o += 4;
    *o++ = 54; /* server identifier */
    *o++ = 4;
    put32(o, n->server);
    o += 4;
  }
  *o++ = 12; /* host name */
  *o++ = 8;
  copy(o, (const uint8_t *)"AuroraOS", 8);
  o += 8;
  *o++ = 55; /* parameters wanted: mask, router, DNS, lease */
  *o++ = 4;
  *o++ = 1;
  *o++ = 3;
  *o++ = 6;
  *o++ = 51;
  *o++ = 255;

  put16(u, 68);
  put16(u + 2, 67);
  put16(u + 4, 8u + BOOTP_LEN);
  put16(u + 6, 0); /* no UDP checksum, which IPv4 allows */
  ip(f, IP_UDP, 0, 0xFFFFFFFFu, 8u + BOOTP_LEN);
  return 14u + 20u + 8u + BOOTP_LEN;
}

static uint32_t arp(NetState *n, uint8_t *f, uint32_t op, const uint8_t *dmac,
                    uint32_t dip) {
  uint8_t *a = f + 14;
  eth(f, op == 1 ? bcast : dmac, n->mac, ETH_ARP);
  put16(a, 1);      /* Ethernet */
  put16(a + 2, ETH_IP);
  a[4] = 6;
  a[5] = 4;
  put16(a + 6, op);
  copy(a + 8, n->mac, 6);
  put32(a + 14, n->ip);
  if (op == 1)
    zero(a + 18, 6);
  else
    copy(a + 18, dmac, 6);
  put32(a + 24, dip);
  return 14u + 28u;
}

uint32_t net_arp_request(NetState *n, uint8_t *f, uint32_t ip_) {
  return arp(n, f, 1, bcast, ip_);
}

uint32_t net_ping(NetState *n, uint8_t *f, uint32_t dst, const uint8_t *dmac,
                  uint16_t seq) {
  static const char hello[] = "AuroraOS on a 3DS says hello :) ";
  uint8_t *c = f + 14 + 20;
  eth(f, dmac, n->mac, ETH_IP);
  c[0] = 8; /* echo request */
  c[1] = 0;
  put16(c + 2, 0);
  put16(c + 4, n->ping_id);
  put16(c + 6, seq);
  for (uint32_t i = 0; i < NET_PING_DATA; i++)
    c[8 + i] = i < sizeof(hello) - 1u ? (uint8_t)hello[i] : (uint8_t)i;
  put16(c + 2, csum(c, 8u + NET_PING_DATA));
  ip(f, IP_ICMP, n->ip, dst, 8u + NET_PING_DATA);
  return 14u + 20u + 8u + NET_PING_DATA;
}

/* The UDP checksum over the pseudo header and the datagram at `u`. */
static uint32_t udp_csum(uint32_t src, uint32_t dst, const uint8_t *u,
                         uint32_t len) {
  uint8_t ph[12];
  uint32_t s = 0;
  put32(ph, src);
  put32(ph + 4, dst);
  ph[8] = 0;
  ph[9] = IP_UDP;
  put16(ph + 10, len);
  s = (~csum(ph, 12) & 0xFFFFu) + (~csum(u, len) & 0xFFFFu);
  while (s >> 16)
    s = (s & 0xFFFFu) + (s >> 16);
  s = ~s & 0xFFFFu;
  return s ? s : 0xFFFFu; /* 0 would mean "no checksum" */
}

uint32_t net_dns(NetState *n, uint8_t *f, uint32_t server,
                 const uint8_t *dmac, const char *name) {
  uint8_t *u = f + 14 + 20, *d = u + 8, *q = d + 12;
  uint32_t qlen = 0;
  /* Labels of 1 to 63 bytes, 253 in all; one trailing dot is allowed. */
  for (const char *s = name; *s;) {
    const char *e = s;
    while (*e && *e != '.')
      e++;
    uint32_t l = (uint32_t)(e - s);
    if (l == 0 || l > 63u || qlen + 1u + l > 253u)
      return 0;
    q[qlen++] = (uint8_t)l;
    for (uint32_t i = 0; i < l; i++) {
      char c = s[i];
      if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_'))
        return 0;
      q[qlen++] = (uint8_t)c;
    }
    s = *e ? e + 1 : e;
  }
  if (!qlen)
    return 0;
  q[qlen++] = 0;
  put16(q + qlen, 1); /* A */
  put16(q + qlen + 2, 1); /* IN */
  qlen += 4;

  eth(f, dmac, n->mac, ETH_IP);
  put16(d, n->dns_id);
  put16(d + 2, 0x0100); /* a standard query, recursion desired */
  put16(d + 4, 1);
  put16(d + 6, 0);
  put16(d + 8, 0);
  put16(d + 10, 0);
  uint32_t ul = 8u + 12u + qlen;
  put16(u, n->dns_port);
  put16(u + 2, 53);
  put16(u + 4, ul);
  put16(u + 6, 0);
  put16(u + 6, udp_csum(n->ip, server, u, ul));
  ip(f, IP_UDP, n->ip, server, ul);
  return 14u + 20u + ul;
}

/* Past a (possibly compressed) name at `off`; 0 if it runs out. */
static uint32_t dns_skip(const uint8_t *d, uint32_t len, uint32_t off) {
  while (off < len) {
    uint32_t l = d[off];
    if ((l & 0xC0u) == 0xC0u)
      return off + 2u <= len ? off + 2u : 0u;
    if (l & 0xC0u)
      return 0;
    if (l == 0)
      return off + 1u;
    off += 1u + l;
  }
  return 0;
}

/* The first A record of an answer to our query; 0 if it has none. */
static uint32_t dns_answer(NetState *n, const uint8_t *d, uint32_t len) {
  uint32_t qd = get16(d + 4), an = get16(d + 6), off = 12;
  n->dns_rcode = d[3] & 0x0Fu;
  for (uint32_t i = 0; i < qd; i++) {
    off = dns_skip(d, len, off);
    if (!off || off + 4u > len)
      return 0;
    off += 4u;
  }
  for (uint32_t i = 0; i < an; i++) {
    off = dns_skip(d, len, off);
    if (!off || off + 10u > len)
      return 0;
    uint32_t type = get16(d + off), cls = get16(d + off + 2);
    uint32_t rdlen = get16(d + off + 8);
    off += 10u;
    if (off + rdlen > len)
      return 0;
    if (type == 1u && cls == 1u && rdlen == 4u)
      return get32(d + off);
    off += rdlen;
  }
  return 0;
}

typedef struct {
  uint32_t type, server, mask, gw, dns, lease;
} DhcpOpts;

/* DHCP options of a reply, from `o` to `end`. */
static void dhcp_opts(DhcpOpts *d, const uint8_t *o, const uint8_t *end) {
  while (o < end && *o != 255) {
    if (*o == 0) {
      o++;
      continue;
    }
    if (o + 2 > end || o + 2 + o[1] > end)
      break;
    uint32_t c = o[0], l = o[1];
    const uint8_t *v = o + 2;
    if (c == 53 && l >= 1)
      d->type = v[0];
    else if (c == 54 && l >= 4)
      d->server = get32(v);
    else if (c == 1 && l >= 4)
      d->mask = get32(v);
    else if (c == 3 && l >= 4)
      d->gw = get32(v);
    else if (c == 6 && l >= 4)
      d->dns = get32(v);
    else if (c == 51 && l >= 4)
      d->lease = get32(v);
    o += 2 + l;
  }
}

int net_rx(NetState *n, const uint8_t *f, uint32_t len, uint8_t *reply,
           uint32_t *reply_len, uint32_t *value) {
  *reply_len = 0;
  *value = 0;
  if (len < 14u)
    return NET_RX_NONE;
  uint32_t type = get16(f + 12);

  if (type == ETH_ARP && len >= 14u + 28u) {
    const uint8_t *a = f + 14;
    uint32_t op = get16(a + 6), spa = get32(a + 14), tpa = get32(a + 24);
    if (get16(a) != 1 || get16(a + 2) != ETH_IP || a[4] != 6 || a[5] != 4)
      return NET_RX_OTHER;
    if (n->gw && spa == n->gw) {
      copy(n->gw_mac, a + 8, 6);
      n->have_gw_mac = 1;
    }
    if (n->arp_ip && spa == n->arp_ip) {
      copy(n->arp_mac, a + 8, 6);
      n->have_arp = 1;
    }
    if (op == 2 && n->ip && tpa == n->ip &&
        (spa == n->gw || (n->arp_ip && spa == n->arp_ip))) {
      *value = spa;
      return NET_RX_ARP_REPLY;
    }
    if (op == 1 && n->ip && tpa == n->ip) {
      *reply_len = arp(n, reply, 2, a + 8, spa);
      *value = spa;
      return NET_RX_ARP_ASKED;
    }
    *value = type;
    return NET_RX_OTHER;
  }

  if (type != ETH_IP || len < 14u + 20u) {
    *value = type;
    return NET_RX_OTHER;
  }
  const uint8_t *h = f + 14;
  uint32_t ihl = (h[0] & 0x0Fu) * 4u, total = get16(h + 2);
  if ((h[0] >> 4) != 4 || ihl < 20u || total < ihl || 14u + total > len) {
    *value = type;
    return NET_RX_OTHER;
  }
  uint32_t proto = h[9], src = get32(h + 12), dst = get32(h + 16);
  const uint8_t *p = h + ihl;
  uint32_t plen = total - ihl;

  if (proto == IP_UDP && plen >= 8u + 240u && get16(p + 2) == 68) {
    const uint8_t *b = p + 8;
    if (b[0] != 2 || get32(b + 4) != n->xid || !same(b + 28, n->mac, 6) ||
        get32(b + 236) != 0x63825363u)
      return NET_RX_OTHER;
    DhcpOpts d = {0, 0, 0, 0, 0, 0};
    uint32_t yi = get32(b + 16);
    dhcp_opts(&d, b + 240, p + plen);
    if (d.type == 2) {
      n->offered = yi;
      n->server = d.server ? d.server : src;
      *value = yi;
      return NET_RX_OFFER;
    }
    if (d.type == 5) {
      n->ip = yi;
      n->mask = d.mask;
      n->gw = d.gw;
      n->dns = d.dns;
      n->lease = d.lease;
      if (d.server)
        n->server = d.server;
      *value = yi;
      return NET_RX_ACK;
    }
    if (d.type == 6) {
      *value = d.server;
      return NET_RX_NAK;
    }
    return NET_RX_OTHER;
  }

  if (proto == IP_UDP && plen >= 8u + 12u && n->dns_port &&
      get16(p) == 53u && get16(p + 2) == n->dns_port) {
    const uint8_t *d = p + 8;
    uint32_t ul = get16(p + 4);
    uint32_t dl = (ul >= 8u && ul <= plen ? ul : plen) - 8u;
    if (dl < 12u || get16(d) != n->dns_id || !(d[2] & 0x80u)) {
      *value = type | (proto << 16);
      return NET_RX_OTHER;
    }
    *value = dns_answer(n, d, dl);
    return NET_RX_DNS;
  }

  if (proto == IP_ICMP && plen >= 8u && n->ip && dst == n->ip) {
    if (p[0] == 0 && get16(p + 4) == n->ping_id) {
      n->reply_from = src;
      n->reply_ttl = h[8];
      n->reply_len = plen;
      *value = get16(p + 6);
      return NET_RX_PING_REPLY;
    }
    /* Unreachable or time exceeded: it quotes the IP header and the first 8
     * bytes of what failed, which was our echo if the id is ours. */
    if ((p[0] == 3 || p[0] == 11) && plen >= 8u + 20u + 8u) {
      const uint8_t *in = p + 8;
      uint32_t il = (in[0] & 0x0Fu) * 4u;
      if (il >= 20u && plen >= 8u + il + 8u && in[9] == IP_ICMP &&
          in[il] == 8 && get16(in + il + 4) == n->ping_id) {
        n->reply_from = src;
        n->reply_ttl = h[8];
        n->reply_len = plen;
        *value = ((uint32_t)p[0] << 8) | p[1] | (get16(in + il + 6) << 16);
        return NET_RX_PING_ERR;
      }
    }
    if (p[0] == 8 && plen <= NET_FRAME_MAX - 34u) {
      uint8_t *c = reply + 14 + 20;
      eth(reply, f + 6, n->mac, ETH_IP);
      copy(c, p, plen);
      c[0] = 0; /* echo reply */
      put16(c + 2, 0);
      put16(c + 2, csum(c, plen));
      ip(reply, IP_ICMP, n->ip, src, plen);
      *reply_len = 14u + 20u + plen;
      *value = src;
      return NET_RX_PINGED;
    }
  }
  *value = type | (proto << 16);
  return NET_RX_OTHER;
}
