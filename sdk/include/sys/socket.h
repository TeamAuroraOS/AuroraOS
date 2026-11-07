/* BSD sockets for AuroraOS C apps: IPv4 over the console's Wi-Fi, run by the
 * ARM11 core (sdk/runtime/app_socket.c). What works and what does not is in
 * sdk/README.md, "Sockets". */
#ifndef _SYS_SOCKET_H_
#define _SYS_SOCKET_H_

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef __socklen_t socklen_t;
typedef __uint16_t sa_family_t;

#define AF_UNSPEC 0
#define AF_INET   2
#define AF_INET6  10
#define PF_UNSPEC AF_UNSPEC
#define PF_INET   AF_INET
#define PF_INET6  AF_INET6

#define SOCK_STREAM   1
#define SOCK_DGRAM    2
#define SOCK_RAW      3
/* Or'd into socket()'s type, as on Linux. */
#define SOCK_NONBLOCK 0x4000  /* O_NONBLOCK */
#define SOCK_CLOEXEC  0x40000 /* O_CLOEXEC; nothing is ever exec'd */

#define SOL_SOCKET    1
#define SO_REUSEADDR  2
#define SO_TYPE       3
#define SO_ERROR      4
#define SO_DONTROUTE  5
#define SO_BROADCAST  6
#define SO_SNDBUF     7
#define SO_RCVBUF     8
#define SO_KEEPALIVE  9
#define SO_OOBINLINE  10
#define SO_LINGER     13
#define SO_REUSEPORT  15
#define SO_RCVLOWAT   18
#define SO_SNDLOWAT   19
#define SO_RCVTIMEO   20
#define SO_SNDTIMEO   21

#define SOMAXCONN 1

#define MSG_OOB       0x01
#define MSG_PEEK      0x02
#define MSG_DONTROUTE 0x04
#define MSG_TRUNC     0x20
#define MSG_DONTWAIT  0x40
#define MSG_WAITALL   0x100
#define MSG_NOSIGNAL  0x4000

#define SHUT_RD   0
#define SHUT_WR   1
#define SHUT_RDWR 2

struct sockaddr {
  sa_family_t sa_family;
  char sa_data[14];
};

struct sockaddr_storage {
  sa_family_t ss_family;
  char __ss_pad[126];
} __attribute__((aligned(4)));

struct linger {
  int l_onoff;
  int l_linger;
};

int socket(int domain, int type, int protocol);
int connect(int fd, const struct sockaddr *addr, socklen_t len);
int bind(int fd, const struct sockaddr *addr, socklen_t len);
int listen(int fd, int backlog);
int accept(int fd, struct sockaddr *addr, socklen_t *len);
ssize_t send(int fd, const void *buf, size_t n, int flags);
ssize_t recv(int fd, void *buf, size_t n, int flags);
ssize_t sendto(int fd, const void *buf, size_t n, int flags,
               const struct sockaddr *to, socklen_t len);
ssize_t recvfrom(int fd, void *buf, size_t n, int flags,
                 struct sockaddr *from, socklen_t *len);
int shutdown(int fd, int how);
int getsockopt(int fd, int level, int name, void *value, socklen_t *len);
int setsockopt(int fd, int level, int name, const void *value,
               socklen_t len);
int getpeername(int fd, struct sockaddr *addr, socklen_t *len);
int getsockname(int fd, struct sockaddr *addr, socklen_t *len);

#ifdef __cplusplus
}
#endif

#endif
