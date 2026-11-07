#ifndef _NETDB_H_
#define _NETDB_H_

#include <netinet/in.h>

#ifdef __cplusplus
extern "C" {
#endif

struct addrinfo {
  int ai_flags;
  int ai_family;
  int ai_socktype;
  int ai_protocol;
  socklen_t ai_addrlen;
  struct sockaddr *ai_addr;
  char *ai_canonname;
  struct addrinfo *ai_next;
};

#define AI_PASSIVE     0x0001
#define AI_CANONNAME   0x0002
#define AI_NUMERICHOST 0x0004
#define AI_V4MAPPED    0x0008
#define AI_ALL         0x0010
#define AI_ADDRCONFIG  0x0020
#define AI_NUMERICSERV 0x0400

#define EAI_BADFLAGS -1
#define EAI_NONAME   -2
#define EAI_AGAIN    -3
#define EAI_FAIL     -4
#define EAI_FAMILY   -6
#define EAI_SOCKTYPE -7
#define EAI_SERVICE  -8
#define EAI_MEMORY   -10
#define EAI_SYSTEM   -11
#define EAI_OVERFLOW -12

#define NI_NUMERICHOST 0x01
#define NI_NUMERICSERV 0x02
#define NI_NOFQDN      0x04
#define NI_NAMEREQD    0x08
#define NI_DGRAM       0x10
#define NI_MAXHOST 1025
#define NI_MAXSERV 32

struct hostent {
  char *h_name;
  char **h_aliases;
  int h_addrtype;
  int h_length;
  char **h_addr_list;
};
#define h_addr h_addr_list[0]

#define HOST_NOT_FOUND 1
#define TRY_AGAIN      2
#define NO_RECOVERY    3
#define NO_DATA        4
#define NO_ADDRESS     NO_DATA
extern int h_errno;

/* A name is looked up with the network's DNS server; the first call joins
 * the network saved in Settings > Wi-Fi if the console is not on it yet. */
int getaddrinfo(const char *node, const char *service,
                const struct addrinfo *hints, struct addrinfo **res);
void freeaddrinfo(struct addrinfo *res);
const char *gai_strerror(int code);
/* Numbers only: there is no reverse lookup, so NI_NAMEREQD fails. */
int getnameinfo(const struct sockaddr *addr, socklen_t len, char *host,
                socklen_t hostlen, char *serv, socklen_t servlen, int flags);
/* One IPv4 address, in a buffer the next call overwrites. */
struct hostent *gethostbyname(const char *name);

#ifdef __cplusplus
}
#endif

#endif
