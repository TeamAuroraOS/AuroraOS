#ifndef _ARPA_INET_H_
#define _ARPA_INET_H_

#include <netinet/in.h>

#ifdef __cplusplus
extern "C" {
#endif

/* IPv4 only: AF_INET6 gives EAFNOSUPPORT. */
in_addr_t inet_addr(const char *text);
int inet_aton(const char *text, struct in_addr *addr);
/* In a buffer that the next call overwrites. */
char *inet_ntoa(struct in_addr addr);
int inet_pton(int family, const char *text, void *addr);
const char *inet_ntop(int family, const void *addr, char *out,
                      socklen_t size);

#ifdef __cplusplus
}
#endif

#endif
