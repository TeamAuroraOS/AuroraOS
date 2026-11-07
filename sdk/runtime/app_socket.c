/* BSD sockets over the ARM11 core's one TCP connection (WIFI_NETOP_TCP_BG,
 * opened by src/os/AppNet.c, bytes through the WifiTcp rings) and its one
 * UDP port (WIFI_NETOP_UDP_BIND, datagrams through WifiUdp), with the name
 * and address helpers. sdk/README.md "Sockets" has what that allows. */
#include "app_internal.h"
#include "appnet.h"
#include "crash.h"
#include "timer.h"
#include "wifi.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>

/* The core gives up on a connection with nothing acknowledged for this long,
 * the SYN included. */
#define CONNECT_MS 20000u
/* The core ends a connection at most 3 s after the FIN was asked for. */
#define CLOSE_MS   4000u
#define LEAVE_MS   1000u

enum { S_FREE, S_NEW, S_CONNECTING, S_OPEN, S_DONE };
#define SHUT_R 1u
#define SHUT_W 2u

typedef struct {
  u8 state, type, nonblock, shut;
  u8 holds;   /* TCP: this socket has the core's connection and its rings */
  u8 bound;   /* UDP: this socket has the core's port */
  u8 has_peer; /* UDP: connect() gave it one */
  u8 linger0; /* SO_LINGER on with 0 s: close() resets */
  u8 keepalive, reuse, broadcast;
  int err;    /* SO_ERROR */
  u32 rcv_ms, snd_ms;
  struct sockaddr_in peer;
} Sock;

static Sock g_s[APP_SOCK_COUNT];
/* A closed socket's connection that the core is still ending. */
static bool g_closing;

int h_errno;

static int fail(int e) {
  errno = e;
  return -1;
}

static Sock *sock_of(int fd, int *e) {
  if (app_sock_fd(fd) && g_s[fd - APP_SOCK_FIRST].state != S_FREE)
    return &g_s[fd - APP_SOCK_FIRST];
  *e = fd >= 0 && fd < APP_SOCK_FIRST ? ENOTSOCK : EBADF;
  return NULL;
}

static u32 since_ms(u64 t0) {
  return (u32)((app_ticks64() - t0) * 1000u / timer_hz());
}

/* One step of a wait: the core gets on with the connection meanwhile. HOME
 * and an ARM11 fault are looked at once a frame (app_poll_home also keeps the
 * connection alive), net_on_wait()'s function ten times a second, and a
 * closed lid pauses the wait. */
static void pause_from(u64 t0) {
  static u64 next;
  static u32 shown;
  u64 now = app_ticks64();
  u32 ms = since_ms(t0);
  if (now >= next) {
    app_poll_home();
    crash_poll_arm11();
    next = now + timer_hz() / 60u;
  }
  if (ms < 100u)
    shown = 0;
  if (ms / 100u != shown) {
    shown = ms / 100u;
    app_net_waiting(ms);
  }
  app_lid_wait();
  delay(2000);
}

static int nets_errno(u32 st, bool connecting) {
  switch (st) {
  case WIFI_NETS_OK:
    return 0;
  case WIFI_NETS_NOLINK:
    return ENETDOWN;
  case WIFI_NETS_TIMEOUT:
    return ETIMEDOUT;
  case WIFI_NETS_NOHOST:
    return EHOSTUNREACH;
  case WIFI_NETS_REFUSED:
    return connecting ? ECONNREFUSED : ECONNRESET;
  case WIFI_NETS_TOOBIG:
    return EINVAL;
  case WIFI_NETS_NOSEND:
    return ENOBUFS;
  case WIFI_NETS_BUSY:
    return EBUSY;
  default:
    return EIO;
  }
}

static int sin_check(const struct sockaddr *a, socklen_t len) {
  if (!a || len < (socklen_t)sizeof(struct sockaddr_in))
    return EINVAL;
  return a->sa_family == AF_INET ? 0 : EAFNOSUPPORT;
}

static void release(Sock *s) {
  s->holds = 0;
  appnet_tcp_release();
}

/* Brings a socket up to date with the core. A finished connection gives the
 * rings back once everything it received has been read. */
static void update(Sock *s) {
  u32 st;
  if (!s->holds)
    return;
  st = wifi_tcp_state();
  if (s->state == S_CONNECTING && st == WIFI_TCP_OPEN) {
    s->state = S_OPEN;
  } else if (st == WIFI_TCP_CLOSED && s->state != S_DONE) {
    int e = nets_errno(appnet_tcp_status(), s->state == S_CONNECTING);
    if (!e && s->state == S_CONNECTING)
      e = ECONNREFUSED;
    s->err = e;
    s->state = S_DONE;
  }
  if (s->state == S_DONE && !wifi_tcp_rx_ready())
    release(s);
}

static void settle(void) {
  u64 t0;
  if (!g_closing)
    return;
  t0 = app_ticks64();
  while (wifi_tcp_state() != WIFI_TCP_CLOSED && since_ms(t0) < CLOSE_MS) {
    wifi_tcp_alive();
    delay(2000);
  }
  if (wifi_tcp_state() != WIFI_TCP_CLOSED) {
    wifi_tcp_close(WIFI_TCP_ABORT);
    t0 = app_ticks64();
    while (wifi_tcp_state() != WIFI_TCP_CLOSED && since_ms(t0) < 500u)
      wifi_tcp_alive();
  }
  g_closing = false;
  appnet_tcp_release();
}

static Sock *udp_holder(void) {
  for (int i = 0; i < APP_SOCK_COUNT; i++)
    if (g_s[i].bound)
      return &g_s[i];
  return NULL;
}

static void tick(void) {
  bool any = g_closing;
  for (int i = 0; i < APP_SOCK_COUNT && !any; i++)
    any = g_s[i].holds;
  if (udp_holder())
    wifi_udp_alive();
  if (!any)
    return;
  wifi_tcp_alive();
  if (g_closing && wifi_tcp_state() == WIFI_TCP_CLOSED) {
    g_closing = false;
    appnet_tcp_release();
  }
}

static ssize_t tcp_recv(Sock *s, void *buf, size_t n, int flags) {
  u8 *p = (u8 *)buf;
  bool wait = !s->nonblock && !(flags & MSG_DONTWAIT);
  u64 t0 = app_ticks64();
  size_t done = 0;

  if (s->state == S_NEW)
    return -ENOTCONN;
  if (!n)
    return 0;
  for (;;) {
    update(s);
    if (s->state != S_CONNECTING) {
      if (s->holds) {
        u32 got = flags & MSG_PEEK ? wifi_tcp_peek(p, (u32)n)
                                   : wifi_tcp_read(p + done, (u32)(n - done));
        if (flags & MSG_PEEK) {
          if (got)
            return got;
        } else {
          done += got;
          if (done == n || (done && !(flags & MSG_WAITALL)))
            return (ssize_t)done;
        }
        if (wifi_tcp_eof())
          return (ssize_t)done;
      }
      if (s->shut & SHUT_R)
        return (ssize_t)done;
      if (s->state == S_DONE && !s->holds) {
        int e = s->err;
        if (done)
          return (ssize_t)done;
        s->err = 0;
        return e ? -e : 0;
      }
    }
    if (!wait || (s->rcv_ms && since_ms(t0) >= s->rcv_ms))
      return done ? (ssize_t)done : -EAGAIN;
    pause_from(t0);
  }
}

static ssize_t tcp_send(Sock *s, const void *buf, size_t n, int flags) {
  const u8 *p = (const u8 *)buf;
  bool wait = !s->nonblock && !(flags & MSG_DONTWAIT);
  u64 t0 = app_ticks64();
  size_t done = 0;

  if (s->shut & SHUT_W)
    return -EPIPE;
  if (s->state == S_NEW)
    return -ENOTCONN;
  for (;;) {
    update(s);
    if (s->state == S_DONE) {
      int e = s->err ? s->err : EPIPE;
      if (done)
        return (ssize_t)done;
      s->err = 0;
      return -e;
    }
    if (s->state == S_OPEN) {
      done += wifi_tcp_write(p + done, (u32)(n - done));
      if (done == n)
        return (ssize_t)n;
    }
    if (!wait || (s->snd_ms && since_ms(t0) >= s->snd_ms))
      return done ? (ssize_t)done : -EAGAIN;
    pause_from(t0);
  }
}

static bool online(void) { return appnet_ip() || appnet_join(); }

/* The core's UDP port for s, `port` 0 for one the core picks. 0 or an
 * errno. */
static int udp_bind_port(Sock *s, u32 port) {
  int e = 0;
  if (udp_holder()) {
    app_net_fail("another UDP socket has the console's port");
    return EADDRINUSE;
  }
  if (!app_net_begin())
    return EBUSY;
  if (!online())
    e = ENETDOWN;
  else if (!appnet_udp_bind(port))
    e = appnet_last_status() == WIFI_NETS_TOOBIG
            ? EADDRNOTAVAIL
            : nets_errno(appnet_last_status(), false);
  app_net_end();
  if (!e)
    s->bound = 1;
  return e;
}

static bool udp_from_peer(const Sock *s, u32 ip, u32 port) {
  return !s->has_peer || (ip == ntohl(s->peer.sin_addr.s_addr) &&
                          port == ntohs(s->peer.sin_port));
}

/* Drops what a connected socket would not take; the next datagram's length,
 * or -1 with none waiting. */
static int udp_next(const Sock *s) {
  for (;;) {
    u32 ip, port;
    int len = wifi_udp_recv(NULL, 0, &ip, &port, 0);
    if (len < 0 || udp_from_peer(s, ip, port))
      return len;
    wifi_udp_recv(NULL, 0, NULL, NULL, 1);
  }
}

static ssize_t udp_send(Sock *s, const void *buf, size_t n, int flags,
                        const struct sockaddr *to, socklen_t len) {
  const struct sockaddr_in *in = (const struct sockaddr_in *)to;
  bool wait = !s->nonblock && !(flags & MSG_DONTWAIT);
  u64 t0 = app_ticks64();
  u32 ip, port;
  int e;

  if (s->shut & SHUT_W)
    return -EPIPE;
  if (to) {
    if ((e = sin_check(to, len)) != 0)
      return -e;
  } else if (s->has_peer) {
    in = &s->peer;
  } else {
    return -EDESTADDRREQ;
  }
  ip = ntohl(in->sin_addr.s_addr);
  port = ntohs(in->sin_port);
  if (n > WIFI_UDP_SEND_MAX)
    return -EMSGSIZE;
  if (!port)
    return -EINVAL;
  if (ip == INADDR_BROADCAST && !s->broadcast)
    return -EACCES;
  if (!ip || (ip >> 24) == 127u)
    return -ENETUNREACH; /* there is no loopback */
  if (!s->bound && (e = udp_bind_port(s, 0)) != 0)
    return -e;
  for (;;) {
    if (wifi_udp_state() != WIFI_UDP_ON)
      return -ENETDOWN;
    if (wifi_udp_send(ip, port, buf, (u32)n))
      return (ssize_t)n;
    if (!wait || (s->snd_ms && since_ms(t0) >= s->snd_ms))
      return -EAGAIN;
    pause_from(t0);
  }
}

static ssize_t udp_recv(Sock *s, void *buf, size_t n, int flags,
                        struct sockaddr_in *from) {
  bool wait = !s->nonblock && !(flags & MSG_DONTWAIT);
  u64 t0 = app_ticks64();
  int e;

  if (!s->bound && (e = udp_bind_port(s, 0)) != 0)
    return -e;
  for (;;) {
    u32 ip, port;
    int len;
    if (s->shut & SHUT_R)
      return 0;
    if (udp_next(s) >= 0) {
      len = wifi_udp_recv(buf, (u32)n, &ip, &port, !(flags & MSG_PEEK));
      if (from) {
        memset(from, 0, sizeof(*from));
        from->sin_family = AF_INET;
        from->sin_port = htons((u16)port);
        from->sin_addr.s_addr = htonl(ip);
      }
      return (flags & MSG_TRUNC) || (size_t)len < n ? len : (ssize_t)n;
    }
    if (wifi_udp_state() != WIFI_UDP_ON)
      return -ENETDOWN;
    if (!wait || (s->rcv_ms && since_ms(t0) >= s->rcv_ms))
      return -EAGAIN;
    pause_from(t0);
  }
}

static ssize_t sk_recv(Sock *s, void *buf, size_t n, int flags) {
  return s->type == SOCK_DGRAM ? udp_recv(s, buf, n, flags, NULL)
                               : tcp_recv(s, buf, n, flags);
}

static ssize_t sk_send(Sock *s, const void *buf, size_t n, int flags) {
  return s->type == SOCK_DGRAM ? udp_send(s, buf, n, flags, NULL, 0)
                               : tcp_send(s, buf, n, flags);
}

static int sock_read(int fd, void *buf, size_t n) {
  int e;
  Sock *s = sock_of(fd, &e);
  return s ? (int)sk_recv(s, buf, n, 0) : -e;
}

static int sock_write(int fd, const void *buf, size_t n) {
  int e;
  Sock *s = sock_of(fd, &e);
  return s ? (int)sk_send(s, buf, n, 0) : -e;
}

static int sock_close(int fd) {
  int e;
  Sock *s = sock_of(fd, &e);
  if (!s)
    return -e;
  if (s->bound)
    appnet_udp_unbind();
  update(s);
  if (s->holds) {
    if (wifi_tcp_state() != WIFI_TCP_CLOSED) {
      wifi_tcp_close(s->linger0 ? WIFI_TCP_ABORT : WIFI_TCP_FIN);
      g_closing = true;
      s->holds = 0;
    } else {
      release(s); /* what was not read is dropped */
    }
  }
  memset(s, 0, sizeof(*s));
  return 0;
}

static int sock_fstat(int fd, void *st) {
  int e;
  struct stat *t = (struct stat *)st;
  if (!sock_of(fd, &e))
    return -e;
  memset(t, 0, sizeof(*t));
  t->st_mode = S_IFSOCK | 0666;
  t->st_blksize = WIFI_TCP_RX;
  return 0;
}

static void leave(void) {
  u64 t0;
  if (udp_holder())
    appnet_udp_unbind();
  for (int i = 0; i < APP_SOCK_COUNT; i++)
    if (g_s[i].holds) {
      g_s[i].holds = 0;
      g_closing = true;
    }
  if (!g_closing)
    return;
  if (wifi_tcp_state() != WIFI_TCP_CLOSED)
    wifi_tcp_close(WIFI_TCP_FIN);
  t0 = app_ticks64();
  while (wifi_tcp_state() != WIFI_TCP_CLOSED && since_ms(t0) < LEAVE_MS)
    wifi_tcp_alive();
  if (wifi_tcp_state() != WIFI_TCP_CLOSED)
    wifi_tcp_close(WIFI_TCP_ABORT);
  g_closing = false;
  appnet_tcp_release();
}

static const AppSockOps g_ops = {sock_read,  sock_write, sock_close,
                                 sock_fstat, tick,       settle,
                                 leave};

int socket(int domain, int type, int protocol) {
  int nb = (type & SOCK_NONBLOCK) != 0, i;
  u32 core;
  type &= ~(SOCK_NONBLOCK | SOCK_CLOEXEC);
  if (domain != AF_INET)
    return fail(EAFNOSUPPORT);
  if (type != SOCK_STREAM && type != SOCK_DGRAM)
    return fail(EPROTONOSUPPORT);
  if (protocol &&
      protocol != (type == SOCK_STREAM ? IPPROTO_TCP : IPPROTO_UDP))
    return fail(EPROTONOSUPPORT);
  core = appnet_core_version();
  if (type == SOCK_DGRAM && core && core < AUDIO_UDP_VERSION)
    return fail(EPROTONOSUPPORT);
  for (i = 0; i < APP_SOCK_COUNT && g_s[i].state != S_FREE; i++)
    ;
  if (i == APP_SOCK_COUNT)
    return fail(EMFILE);
  g_app_sock = &g_ops;
  memset(&g_s[i], 0, sizeof(g_s[i]));
  g_s[i].state = S_NEW;
  g_s[i].type = (u8)type;
  g_s[i].nonblock = (u8)nb;
  return APP_SOCK_FIRST + i;
}

static int wait_connected(Sock *s) {
  u64 t0 = app_ticks64();
  for (;;) {
    update(s);
    if (s->state == S_OPEN)
      return 0;
    if (s->state == S_DONE) {
      int e = s->err;
      s->err = 0;
      return fail(e ? e : ECONNREFUSED);
    }
    pause_from(t0);
  }
}

/* A UDP socket's default destination and the one sender it takes. */
static int udp_connect(Sock *s, const struct sockaddr *addr, socklen_t len) {
  const struct sockaddr_in *in = (const struct sockaddr_in *)addr;
  int e;
  if (addr && len >= (socklen_t)sizeof(sa_family_t) &&
      addr->sa_family == AF_UNSPEC) {
    s->has_peer = 0;
    return 0;
  }
  if ((e = sin_check(addr, len)) != 0)
    return fail(e);
  if (!in->sin_port)
    return fail(EINVAL);
  if (!s->bound && (e = udp_bind_port(s, 0)) != 0)
    return fail(e);
  s->peer = *in;
  s->has_peer = 1;
  return 0;
}

int connect(int fd, const struct sockaddr *addr, socklen_t len) {
  int e;
  Sock *s = sock_of(fd, &e);
  const struct sockaddr_in *in = (const struct sockaddr_in *)addr;
  u32 ip, port;

  if (!s)
    return fail(e);
  if (s->type == SOCK_DGRAM)
    return udp_connect(s, addr, len);
  if ((e = sin_check(addr, len)) != 0)
    return fail(e);
  update(s);
  if (s->state == S_CONNECTING)
    return s->nonblock ? fail(EALREADY) : wait_connected(s);
  if (s->state == S_OPEN || s->holds)
    return fail(EISCONN);
  ip = ntohl(in->sin_addr.s_addr);
  port = ntohs(in->sin_port);
  /* Nothing listens on the console itself. */
  if (!port || !ip || (ip >> 24) == 127u || ip == appnet_ip())
    return fail(ECONNREFUSED);
  for (int i = 0; i < APP_SOCK_COUNT; i++) {
    update(&g_s[i]);
    if (g_s[i].holds) {
      app_net_fail("another socket has the network connection");
      return fail(EBUSY);
    }
  }
  if (!app_net_begin())
    return fail(EBUSY);
  if (!online())
    e = ENETDOWN;
  else if (!appnet_tcp_open(ip, port, CONNECT_MS))
    e = nets_errno(appnet_last_status(), true);
  else
    e = 0;
  app_net_end();
  if (e)
    return fail(e);
  s->holds = 1;
  s->state = S_CONNECTING;
  s->shut = 0;
  s->err = 0;
  s->peer = *in;
  if (s->nonblock)
    return fail(EINPROGRESS);
  return wait_connected(s);
}

int bind(int fd, const struct sockaddr *addr, socklen_t len) {
  int e;
  Sock *s = sock_of(fd, &e);
  const struct sockaddr_in *in = (const struct sockaddr_in *)addr;
  u32 ip;
  if (!s)
    return fail(e);
  if ((e = sin_check(addr, len)) != 0)
    return fail(e);
  ip = ntohl(in->sin_addr.s_addr);
  if (ip != INADDR_ANY && ip != appnet_ip())
    return fail(EADDRNOTAVAIL);
  /* The core picks a TCP connection's own port. */
  if (s->type == SOCK_STREAM)
    return in->sin_port ? fail(EADDRNOTAVAIL) : 0;
  if (s->bound)
    return fail(EINVAL);
  e = udp_bind_port(s, ntohs(in->sin_port));
  return e ? fail(e) : 0;
}

int listen(int fd, int backlog) {
  int e;
  (void)backlog;
  return sock_of(fd, &e) ? fail(EOPNOTSUPP) : fail(e);
}

int accept(int fd, struct sockaddr *addr, socklen_t *len) {
  int e;
  (void)addr;
  (void)len;
  return sock_of(fd, &e) ? fail(EOPNOTSUPP) : fail(e);
}

static ssize_t result(ssize_t r) { return r < 0 ? fail((int)-r) : r; }

ssize_t send(int fd, const void *buf, size_t n, int flags) {
  int e;
  Sock *s = sock_of(fd, &e);
  return s ? result(sk_send(s, buf, n, flags)) : fail(e);
}

ssize_t recv(int fd, void *buf, size_t n, int flags) {
  int e;
  Sock *s = sock_of(fd, &e);
  return s ? result(sk_recv(s, buf, n, flags)) : fail(e);
}

ssize_t sendto(int fd, const void *buf, size_t n, int flags,
               const struct sockaddr *to, socklen_t len) {
  int e;
  Sock *s = sock_of(fd, &e);
  if (!s)
    return fail(e);
  if (s->type == SOCK_DGRAM)
    return result(udp_send(s, buf, n, flags, to, len));
  /* A connected stream ignores `to`, as on Linux. */
  return result(tcp_send(s, buf, n, flags));
}

static void put_addr(const struct sockaddr_in *in, struct sockaddr *to,
                     socklen_t *len) {
  socklen_t n;
  if (!to || !len)
    return;
  n = *len < (socklen_t)sizeof(*in) ? *len : (socklen_t)sizeof(*in);
  memcpy(to, in, n);
  *len = sizeof(*in);
}

ssize_t recvfrom(int fd, void *buf, size_t n, int flags,
                 struct sockaddr *from, socklen_t *len) {
  int e;
  Sock *s = sock_of(fd, &e);
  struct sockaddr_in src;
  ssize_t r;
  if (!s)
    return fail(e);
  if (s->type == SOCK_DGRAM) {
    r = udp_recv(s, buf, n, flags, &src);
    if (r >= 0)
      put_addr(&src, from, len);
    return result(r);
  }
  r = tcp_recv(s, buf, n, flags);
  if (r >= 0)
    put_addr(&s->peer, from, len);
  return result(r);
}

/* SHUT_WR does not send the FIN before close(): the core ends a connection
 * soon after its FIN, and what the server sent later would be lost. */
int shutdown(int fd, int how) {
  int e;
  Sock *s = sock_of(fd, &e);
  if (!s)
    return fail(e);
  if (how < SHUT_RD || how > SHUT_RDWR)
    return fail(EINVAL);
  update(s);
  if (s->type == SOCK_DGRAM ? !s->has_peer : s->state == S_NEW)
    return fail(ENOTCONN);
  if (how != SHUT_WR)
    s->shut |= SHUT_R;
  if (how != SHUT_RD)
    s->shut |= SHUT_W;
  return 0;
}

static int put_int(void *value, socklen_t *len, int v) {
  if (!value || !len || *len < (socklen_t)sizeof(int))
    return fail(EINVAL);
  memcpy(value, &v, sizeof(int));
  *len = sizeof(int);
  return 0;
}

static int put_tv(void *value, socklen_t *len, u32 ms) {
  struct timeval tv;
  if (!value || !len || *len < (socklen_t)sizeof(tv))
    return fail(EINVAL);
  tv.tv_sec = (time_t)(ms / 1000u);
  tv.tv_usec = (suseconds_t)(ms % 1000u * 1000u);
  memcpy(value, &tv, sizeof(tv));
  *len = sizeof(tv);
  return 0;
}

int getsockopt(int fd, int level, int name, void *value, socklen_t *len) {
  int e;
  Sock *s = sock_of(fd, &e);
  if (!s)
    return fail(e);
  if (level == IPPROTO_TCP)
    return name == TCP_NODELAY ? put_int(value, len, 1) : fail(ENOPROTOOPT);
  if (level != SOL_SOCKET)
    return fail(ENOPROTOOPT);
  switch (name) {
  case SO_ERROR:
    update(s);
    e = s->state == S_DONE ? s->err : 0;
    s->err = 0;
    return put_int(value, len, e);
  case SO_TYPE:
    return put_int(value, len, s->type);
  case SO_RCVBUF:
    return put_int(value, len,
                   (int)(s->type == SOCK_DGRAM ? WIFI_UDP_RX : WIFI_TCP_RX));
  case SO_SNDBUF:
    return put_int(value, len,
                   (int)(s->type == SOCK_DGRAM ? WIFI_UDP_TX : WIFI_TCP_TX));
  case SO_BROADCAST:
    return put_int(value, len, s->broadcast);
  case SO_KEEPALIVE:
    return put_int(value, len, s->keepalive);
  case SO_REUSEADDR:
    return put_int(value, len, s->reuse);
  case SO_RCVTIMEO:
    return put_tv(value, len, s->rcv_ms);
  case SO_SNDTIMEO:
    return put_tv(value, len, s->snd_ms);
  case SO_LINGER: {
    struct linger l = {s->linger0, 0};
    if (!value || !len || *len < (socklen_t)sizeof(l))
      return fail(EINVAL);
    memcpy(value, &l, sizeof(l));
    *len = sizeof(l);
    return 0;
  }
  default:
    return fail(ENOPROTOOPT);
  }
}

static int get_tv(const void *value, socklen_t len, u32 *ms) {
  struct timeval tv;
  u64 v;
  if (!value || len < (socklen_t)sizeof(tv))
    return fail(EINVAL);
  memcpy(&tv, value, sizeof(tv));
  if (tv.tv_sec < 0 || tv.tv_usec < 0 || tv.tv_usec >= 1000000)
    return fail(EDOM);
  v = (u64)tv.tv_sec * 1000u + (u64)(tv.tv_usec + 999) / 1000u;
  *ms = v > 0xFFFFFFFFu ? 0xFFFFFFFFu : (u32)v;
  return 0;
}

int setsockopt(int fd, int level, int name, const void *value,
               socklen_t len) {
  int e, v = 0;
  Sock *s = sock_of(fd, &e);
  if (!s)
    return fail(e);
  if (level == IPPROTO_TCP)
    return name == TCP_NODELAY ? 0 : fail(ENOPROTOOPT);
  if (level != SOL_SOCKET)
    return fail(ENOPROTOOPT);
  if (name == SO_RCVTIMEO)
    return get_tv(value, len, &s->rcv_ms);
  if (name == SO_SNDTIMEO)
    return get_tv(value, len, &s->snd_ms);
  if (name == SO_LINGER) {
    struct linger l;
    if (!value || len < (socklen_t)sizeof(l))
      return fail(EINVAL);
    memcpy(&l, value, sizeof(l));
    s->linger0 = l.l_onoff && !l.l_linger;
    return 0;
  }
  if (!value || len < (socklen_t)sizeof(int))
    return fail(EINVAL);
  memcpy(&v, value, sizeof(int));
  switch (name) {
  case SO_BROADCAST:
    s->broadcast = v != 0;
    return 0;
  case SO_KEEPALIVE:
    s->keepalive = v != 0;
    return 0;
  case SO_REUSEADDR:
  case SO_REUSEPORT:
    s->reuse = v != 0;
    return 0;
  case SO_RCVBUF:
  case SO_SNDBUF:
    return 0; /* the rings are what they are */
  default:
    return fail(ENOPROTOOPT);
  }
}

int getpeername(int fd, struct sockaddr *addr, socklen_t *len) {
  int e;
  Sock *s = sock_of(fd, &e);
  if (!s)
    return fail(e);
  update(s);
  if (s->type == SOCK_DGRAM ? !s->has_peer
                            : s->state != S_OPEN &&
                                  !(s->state == S_DONE && s->holds))
    return fail(ENOTCONN);
  put_addr(&s->peer, addr, len);
  return 0;
}

/* The core does not say which port it gave a TCP connection: 0. */
int getsockname(int fd, struct sockaddr *addr, socklen_t *len) {
  int e;
  Sock *s = sock_of(fd, &e);
  struct sockaddr_in in;
  if (!s)
    return fail(e);
  memset(&in, 0, sizeof(in));
  in.sin_family = AF_INET;
  in.sin_addr.s_addr = htonl(appnet_ip());
  if (s->bound)
    in.sin_port = htons((u16)wifi_udp_port());
  put_addr(&in, addr, len);
  return 0;
}

static bool file_fd(int fd) {
  struct stat st;
  return fd >= APP_FD_FIRST && fd < APP_SOCK_FIRST &&
         app_fs_fstat(fd, &st) == 0;
}

int fcntl(int fd, int cmd, ...) {
  int e, arg = 0;
  Sock *s = sock_of(fd, &e);
  va_list ap;
  va_start(ap, cmd);
  if (cmd == F_SETFL || cmd == F_SETFD)
    arg = va_arg(ap, int);
  va_end(ap);
  if (!s && !(fd >= 0 && fd < APP_FD_FIRST) && !file_fd(fd))
    return fail(EBADF);
  switch (cmd) {
  case F_GETFD:
  case F_SETFD:
    return 0;
  case F_GETFL:
    return O_RDWR | (s && s->nonblock ? O_NONBLOCK : 0);
  case F_SETFL:
    if (s)
      s->nonblock = (arg & O_NONBLOCK) != 0;
    return 0;
  default:
    return fail(EINVAL);
  }
}

int ioctl(int fd, unsigned long request, ...) {
  int e, *arg;
  Sock *s = sock_of(fd, &e);
  va_list ap;
  va_start(ap, request);
  arg = va_arg(ap, int *);
  va_end(ap);
  if (!s)
    return fail(e == ENOTSOCK ? ENOTTY : e);
  if (!arg)
    return fail(EFAULT);
  if (request == FIONBIO) {
    s->nonblock = *arg != 0;
    return 0;
  }
  if (request == FIONREAD) {
    int next = s->bound ? udp_next(s) : -1;
    update(s);
    *arg = s->type == SOCK_DGRAM ? (next < 0 ? 0 : next)
           : s->holds            ? (int)wifi_tcp_rx_ready()
                                 : 0;
    return 0;
  }
  return fail(ENOTTY);
}

static short udp_ready(Sock *s) {
  short r = 0;
  if (!s->bound)
    return POLLOUT | POLLWRNORM; /* a send binds it */
  if (wifi_udp_state() != WIFI_UDP_ON)
    return POLLERR | POLLHUP;
  if ((s->shut & SHUT_R) || udp_next(s) >= 0)
    r |= POLLIN | POLLRDNORM;
  if (!(s->shut & SHUT_W) && wifi_udp_room(WIFI_UDP_SEND_MAX))
    r |= POLLOUT | POLLWRNORM;
  return r;
}

static short sock_ready(Sock *s) {
  short r = 0;
  if (s->type == SOCK_DGRAM)
    return udp_ready(s);
  update(s);
  switch (s->state) {
  case S_NEW:
    return POLLOUT | POLLHUP;
  case S_CONNECTING:
    return 0;
  case S_OPEN:
    if ((s->shut & SHUT_R) || wifi_tcp_rx_ready() || wifi_tcp_eof())
      r |= POLLIN | POLLRDNORM;
    if (!(s->shut & SHUT_W) && wifi_tcp_tx_room())
      r |= POLLOUT | POLLWRNORM;
    return r;
  default:
    r = POLLIN | POLLRDNORM | POLLHUP;
    if (s->err)
      r |= POLLERR | POLLOUT;
    return r;
  }
}

/* What fd is ready for, of `ev`; POLLERR, POLLHUP and POLLNVAL always. */
static short ready(int fd, short ev) {
  const short always = POLLERR | POLLHUP | POLLNVAL;
  int e;
  Sock *s;
  if (fd < 0)
    return 0;
  if ((s = sock_of(fd, &e)) != NULL)
    return sock_ready(s) & (ev | always);
  if (fd == 0)
    return ev & (POLLIN | POLLRDNORM); /* reads give end of file */
  if (fd == 1 || fd == 2)
    return ev & (POLLOUT | POLLWRNORM);
  if (file_fd(fd))
    return ev & (POLLIN | POLLRDNORM | POLLOUT | POLLWRNORM);
  return POLLNVAL;
}

int poll(struct pollfd *fds, nfds_t n, int timeout) {
  u64 t0 = app_ticks64();
  if (n && !fds)
    return fail(EFAULT);
  for (;;) {
    int count = 0;
    for (nfds_t i = 0; i < n; i++) {
      fds[i].revents = ready(fds[i].fd, fds[i].events);
      if (fds[i].revents)
        count++;
    }
    if (count || !timeout || (timeout > 0 && since_ms(t0) >= (u32)timeout))
      return count;
    pause_from(t0);
  }
}

int select(int n, fd_set *rd, fd_set *wr, fd_set *ex, struct timeval *tv) {
  fd_set in_r, in_w;
  u32 limit = 0;
  u64 t0 = app_ticks64();
  if (n < 0 || n > FD_SETSIZE)
    return fail(EINVAL);
  if (tv && get_tv(tv, sizeof(*tv), &limit) < 0)
    return fail(EINVAL);
  FD_ZERO(&in_r);
  FD_ZERO(&in_w);
  if (rd)
    in_r = *rd;
  if (wr)
    in_w = *wr;
  for (;;) {
    int count = 0;
    if (rd)
      FD_ZERO(rd);
    if (wr)
      FD_ZERO(wr);
    for (int fd = 0; fd < n; fd++) {
      short r;
      bool want_r = FD_ISSET(fd, &in_r), want_w = FD_ISSET(fd, &in_w);
      if (!want_r && !want_w)
        continue;
      r = ready(fd, POLLIN | POLLOUT);
      if (r & POLLNVAL)
        return fail(EBADF);
      if (want_r && (r & (POLLIN | POLLHUP | POLLERR))) {
        FD_SET(fd, rd);
        count++;
      }
      if (want_w && (r & (POLLOUT | POLLERR))) {
        FD_SET(fd, wr);
        count++;
      }
    }
    if (ex)
      FD_ZERO(ex); /* no out-of-band data */
    if (count || (tv && since_ms(t0) >= limit))
      return count;
    pause_from(t0);
  }
}

/* Dotted decimal, four parts, as inet_pton() takes them. */
static bool parse_quad(const char *t, u32 *ip) {
  u32 v = 0;
  for (int part = 0; part < 4; part++) {
    u32 x = 0;
    int digits = 0;
    if (part && *t++ != '.')
      return false;
    while (*t >= '0' && *t <= '9') {
      if (digits && x == 0)
        return false; /* no leading zeros */
      x = x * 10u + (u32)(*t++ - '0');
      if (++digits > 3 || x > 255u)
        return false;
    }
    if (!digits)
      return false;
    v = v << 8 | x;
  }
  if (*t)
    return false;
  *ip = v;
  return true;
}

int inet_pton(int family, const char *text, void *addr) {
  u32 ip;
  if (family != AF_INET)
    return fail(EAFNOSUPPORT);
  if (!parse_quad(text, &ip))
    return 0;
  ip = htonl(ip);
  memcpy(addr, &ip, 4);
  return 1;
}

const char *inet_ntop(int family, const void *addr, char *out,
                      socklen_t size) {
  char t[INET_ADDRSTRLEN];
  u32 ip;
  if (family != AF_INET) {
    errno = EAFNOSUPPORT;
    return NULL;
  }
  memcpy(&ip, addr, 4);
  appnet_ip_text(ntohl(ip), t);
  if ((socklen_t)strlen(t) + 1 > size) {
    errno = ENOSPC;
    return NULL;
  }
  strcpy(out, t);
  return out;
}

/* The classic forms: a, a.b, a.b.c and a.b.c.d, each part decimal, octal
 * (0...) or hex (0x...); the last part fills the bytes left. */
int inet_aton(const char *text, struct in_addr *addr) {
  u32 part[4], n = 0, ip;
  const char *t = text;
  for (;;) {
    char *end;
    unsigned long v;
    if (*t < '0' || *t > '9')
      return 0;
    v = strtoul(t, &end, 0);
    if (end == t || v > 0xFFFFFFFFul)
      return 0;
    part[n++] = (u32)v;
    t = end;
    if (*t != '.')
      break;
    if (n == 4)
      return 0;
    t++;
  }
  if (*t)
    return 0;
  for (u32 i = 0; i + 1 < n; i++)
    if (part[i] > 255u)
      return 0;
  if (part[n - 1] > (n == 1 ? 0xFFFFFFFFu : 0xFFFFFFFFu >> (8 * (n - 1))))
    return 0;
  ip = part[n - 1];
  for (u32 i = 0; i + 1 < n; i++)
    ip |= part[i] << (24 - 8 * i);
  if (addr)
    addr->s_addr = htonl(ip);
  return 1;
}

in_addr_t inet_addr(const char *text) {
  struct in_addr a;
  return inet_aton(text, &a) ? a.s_addr : INADDR_NONE;
}

char *inet_ntoa(struct in_addr addr) {
  static char t[INET_ADDRSTRLEN];
  appnet_ip_text(ntohl(addr.s_addr), t);
  return t;
}

static const struct {
  const char *name;
  u16 port;
} g_services[] = {
    {"ftp", 21},    {"ssh", 22},     {"telnet", 23}, {"smtp", 25},
    {"domain", 53}, {"http", 80},    {"pop3", 110},  {"ntp", 123},
    {"imap", 143},  {"https", 443},  {"imaps", 993}, {"pop3s", 995},
    {"irc", 6667},  {"http-alt", 8080},
};

static int service_port(const char *svc, int flags, u32 *port) {
  char *end;
  unsigned long v;
  if (!svc || !*svc) {
    *port = 0;
    return 0;
  }
  v = strtoul(svc, &end, 10);
  if (*svc >= '0' && *svc <= '9' && !*end) {
    if (v > 0xFFFFu)
      return EAI_SERVICE;
    *port = (u32)v;
    return 0;
  }
  if (flags & AI_NUMERICSERV)
    return EAI_NONAME;
  for (u32 i = 0; i < sizeof(g_services) / sizeof(g_services[0]); i++)
    if (!strcmp(svc, g_services[i].name)) {
      *port = g_services[i].port;
      return 0;
    }
  return EAI_SERVICE;
}

static int resolve(const char *node, int flags, u32 *ip) {
  struct in_addr a;
  int e = 0;
  u32 st;
  if (inet_aton(node, &a)) {
    *ip = ntohl(a.s_addr);
    return 0;
  }
  if (!strcmp(node, "localhost")) {
    *ip = INADDR_LOOPBACK;
    return 0;
  }
  if (flags & AI_NUMERICHOST)
    return EAI_NONAME;
  if (!app_net_begin())
    return EAI_AGAIN;
  if (!online()) {
    e = EAI_AGAIN;
  } else if (!appnet_resolve(node, ip)) {
    st = appnet_last_status();
    e = st == WIFI_NETS_NONAME || st == WIFI_NETS_BADNAME ? EAI_NONAME
        : st == WIFI_NETS_NOHOST                         ? EAI_FAIL
                                                          : EAI_AGAIN;
  }
  app_net_end();
  return e;
}

typedef struct {
  struct addrinfo ai;
  struct sockaddr_in in;
  char name[];
} AiBlock;

int getaddrinfo(const char *node, const char *service,
                const struct addrinfo *hints, struct addrinfo **res) {
  static const int types[2] = {SOCK_STREAM, SOCK_DGRAM};
  int family = hints ? hints->ai_family : AF_UNSPEC;
  int socktype = hints ? hints->ai_socktype : 0;
  int protocol = hints ? hints->ai_protocol : 0;
  int flags = hints ? hints->ai_flags : 0, e;
  u32 ip, port;
  struct addrinfo *head = NULL, **tail = &head;

  if (!res)
    return EAI_FAIL;
  *res = NULL;
  if (!node && !service)
    return EAI_NONAME;
  if (family != AF_UNSPEC && family != AF_INET)
    return EAI_FAMILY;
  if (socktype && socktype != SOCK_STREAM && socktype != SOCK_DGRAM)
    return EAI_SOCKTYPE;
  if (flags & ~(AI_PASSIVE | AI_CANONNAME | AI_NUMERICHOST | AI_V4MAPPED |
                AI_ALL | AI_ADDRCONFIG | AI_NUMERICSERV))
    return EAI_BADFLAGS;
  if ((e = service_port(service, flags, &port)) != 0)
    return e;
  if (node)
    e = resolve(node, flags, &ip);
  else
    ip = flags & AI_PASSIVE ? INADDR_ANY : INADDR_LOOPBACK;
  if (e) {
    h_errno = e == EAI_AGAIN ? TRY_AGAIN : HOST_NOT_FOUND;
    return e;
  }
  for (int i = 0; i < 2; i++) {
    int type = types[i];
    int proto = type == SOCK_STREAM ? IPPROTO_TCP : IPPROTO_UDP;
    size_t extra = head == NULL && (flags & AI_CANONNAME) && node
                       ? strlen(node) + 1u
                       : 0;
    AiBlock *b;
    if ((socktype && socktype != type) || (protocol && protocol != proto))
      continue;
    b = calloc(1, sizeof(*b) + extra);
    if (!b) {
      freeaddrinfo(head);
      return EAI_MEMORY;
    }
    b->in.sin_family = AF_INET;
    b->in.sin_port = htons((u16)port);
    b->in.sin_addr.s_addr = htonl(ip);
    b->ai.ai_flags = flags;
    b->ai.ai_family = AF_INET;
    b->ai.ai_socktype = type;
    b->ai.ai_protocol = proto;
    b->ai.ai_addrlen = sizeof(b->in);
    b->ai.ai_addr = (struct sockaddr *)&b->in;
    if (extra) {
      memcpy(b->name, node, extra);
      b->ai.ai_canonname = b->name;
    }
    *tail = &b->ai;
    tail = &b->ai.ai_next;
  }
  if (!head)
    return EAI_SOCKTYPE;
  *res = head;
  return 0;
}

void freeaddrinfo(struct addrinfo *res) {
  while (res) {
    struct addrinfo *next = res->ai_next;
    free(res); /* ai is the block's first member */
    res = next;
  }
}

const char *gai_strerror(int code) {
  switch (code) {
  case 0:
    return "Success";
  case EAI_BADFLAGS:
    return "Bad value for ai_flags";
  case EAI_NONAME:
    return "Name or service not known";
  case EAI_AGAIN:
    return "Temporary failure in name resolution";
  case EAI_FAIL:
    return "Non-recoverable failure in name resolution";
  case EAI_FAMILY:
    return "ai_family not supported";
  case EAI_SOCKTYPE:
    return "ai_socktype not supported";
  case EAI_SERVICE:
    return "Servname not supported for ai_socktype";
  case EAI_MEMORY:
    return "Memory allocation failure";
  case EAI_SYSTEM:
    return "System error";
  case EAI_OVERFLOW:
    return "Argument buffer overflow";
  default:
    return "Unknown error";
  }
}

int getnameinfo(const struct sockaddr *addr, socklen_t len, char *host,
                socklen_t hostlen, char *serv, socklen_t servlen,
                int flags) {
  const struct sockaddr_in *in = (const struct sockaddr_in *)addr;
  char t[12];
  if (!addr || len < (socklen_t)sizeof(*in))
    return EAI_FAIL;
  if (addr->sa_family != AF_INET)
    return EAI_FAMILY;
  if (host && hostlen) {
    if (flags & NI_NAMEREQD)
      return EAI_NONAME;
    if (!inet_ntop(AF_INET, &in->sin_addr, host, hostlen))
      return EAI_OVERFLOW;
  }
  if (serv && servlen) {
    u32 p = ntohs(in->sin_port), n = 0;
    char *q = t + sizeof(t) - 1;
    *q = 0;
    do {
      *--q = (char)('0' + p % 10u);
      n++;
    } while ((p /= 10u) != 0);
    if ((socklen_t)n + 1 > servlen)
      return EAI_OVERFLOW;
    memcpy(serv, q, n + 1u);
  }
  return 0;
}

struct hostent *gethostbyname(const char *name) {
  static struct hostent h;
  static char *list[2], *aliases[1];
  static u32 addr;
  static char hname[256];
  u32 ip;
  int e;
  if (!name) {
    h_errno = HOST_NOT_FOUND;
    return NULL;
  }
  if ((e = resolve(name, 0, &ip)) != 0) {
    h_errno = e == EAI_AGAIN ? TRY_AGAIN : HOST_NOT_FOUND;
    return NULL;
  }
  addr = htonl(ip);
  strncpy(hname, name, sizeof(hname) - 1u);
  list[0] = (char *)&addr;
  list[1] = NULL;
  aliases[0] = NULL;
  h.h_name = hname;
  h.h_aliases = aliases;
  h.h_addrtype = AF_INET;
  h.h_length = 4;
  h.h_addr_list = list;
  return &h;
}
