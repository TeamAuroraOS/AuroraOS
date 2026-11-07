#ifndef _NETINET_IN_H_
#define _NETINET_IN_H_

#include <stdint.h>
#include <sys/socket.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef _IN_ADDR_T_DECLARED
typedef __uint32_t in_addr_t;
#define _IN_ADDR_T_DECLARED
#endif

#ifndef _IN_PORT_T_DECLARED
typedef __uint16_t in_port_t;
#define _IN_PORT_T_DECLARED
#endif

#define IPPROTO_IP   0
#define IPPROTO_ICMP 1
#define IPPROTO_TCP  6
#define IPPROTO_UDP  17
#define IPPROTO_IPV6 41

#define INADDR_ANY       ((in_addr_t)0x00000000u)
#define INADDR_BROADCAST ((in_addr_t)0xFFFFFFFFu)
#define INADDR_NONE      ((in_addr_t)0xFFFFFFFFu)
#define INADDR_LOOPBACK  ((in_addr_t)0x7F000001u)

#define INET_ADDRSTRLEN  16
#define INET6_ADDRSTRLEN 46

#define IP_TOS 1
#define IP_TTL 2

struct in_addr {
  in_addr_t s_addr;
};

struct sockaddr_in {
  sa_family_t sin_family;
  in_port_t sin_port;
  struct in_addr sin_addr;
  unsigned char sin_zero[8];
};

/* Only so code with an IPv6 branch compiles: AF_INET6 sockets are refused. */
struct in6_addr {
  uint8_t s6_addr[16];
};

struct sockaddr_in6 {
  sa_family_t sin6_family;
  in_port_t sin6_port;
  uint32_t sin6_flowinfo;
  struct in6_addr sin6_addr;
  uint32_t sin6_scope_id;
};

#define IN6ADDR_ANY_INIT {{0}}

/* The ARM is little-endian. */
static inline uint32_t htonl(uint32_t x) { return __builtin_bswap32(x); }
static inline uint16_t htons(uint16_t x) { return __builtin_bswap16(x); }
static inline uint32_t ntohl(uint32_t x) { return __builtin_bswap32(x); }
static inline uint16_t ntohs(uint16_t x) { return __builtin_bswap16(x); }
#define htonl htonl
#define htons htons
#define ntohl ntohl
#define ntohs ntohs

#ifdef __cplusplus
}
#endif

#endif
