/* The C SDK's sockets (sdk/runtime/app_socket.c) and the address helpers,
 * over the real AppNet.c, WiFiTcp.c and WiFiUdp.c, against the model core. */
#include "app_internal.h"
#include "check.h"
#include "hostrt.h"
#include "model.h"
#include "wifi.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>

#define IP(a, b, c, d) ((uint32_t)(a) << 24 | (b) << 16 | (c) << 8 | (d))
#define ECHO_IP  IP(10, 0, 0, 5)
#define OTHER_IP IP(10, 0, 0, 6)
#define NOHOST   IP(192, 168, 1, 99)

static void start(uint32_t version) {
  model_reset(version);
  model_server(ECHO_IP, 7, SV_ECHO, 0);
  model_server(ECHO_IP, 9, SV_SINK, 0);
  model_server(ECHO_IP, 19, SV_BULK, 100000);
  model_server(ECHO_IP, 80, SV_HTTP, 5000);
  model_server(ECHO_IP, 81, SV_HOLD, 0);
  model_server(ECHO_IP, 82, SV_SILENT, 0);
  model_server(ECHO_IP, 83, SV_RESET, 10);
  model_server(OTHER_IP, 7, SV_ECHO, 0);
  model_dns("echo.test", ECHO_IP);
  model_dns("other.test", OTHER_IP);
}

static struct sockaddr_in addr(uint32_t ip, uint16_t port) {
  struct sockaddr_in a;
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  a.sin_addr.s_addr = htonl(ip);
  return a;
}

static int tcp_to(uint32_t ip, uint16_t port, int flags) {
  struct sockaddr_in a = addr(ip, port);
  int fd = socket(AF_INET, SOCK_STREAM | flags, 0);
  if (fd < 0)
    return -100;
  if (connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0 &&
      errno != EINPROGRESS) {
    int e = errno;
    g_app_sock->close(fd);
    errno = e;
    return -1;
  }
  return fd;
}

static int sclose(int fd) { return g_app_sock->close(fd); }

/* Lets the frame loop run until the model's connection is over. */
static void settle_model(void) {
  for (int i = 0; i < 600 && g_model.bg; i++)
    host_frames(1);
}

static void t_inet(void) {
  struct in_addr a;
  unsigned char b[4];
  char t[INET_ADDRSTRLEN];
  CHECK_EQ(htons(0x1234), 0x3412);
  CHECK_EQ(ntohl(htonl(0x01020304u)), 0x01020304u);
  CHECK_EQ(inet_pton(AF_INET, "192.168.1.2", b), 1);
  CHECK(b[0] == 192 && b[1] == 168 && b[2] == 1 && b[3] == 2);
  CHECK_EQ(inet_pton(AF_INET, "256.1.1.1", b), 0);
  CHECK_EQ(inet_pton(AF_INET, "1.2.3", b), 0);
  CHECK_EQ(inet_pton(AF_INET, "01.2.3.4", b), 0);
  CHECK_EQ(inet_pton(AF_INET, "1.2.3.4 ", b), 0);
  CHECK_EQ(inet_pton(AF_INET, "0.0.0.0", b), 1);
  errno = 0;
  CHECK_EQ(inet_pton(AF_INET6, "::1", b), -1);
  CHECK_EQ(errno, EAFNOSUPPORT);
  CHECK(inet_ntop(AF_INET, b, t, sizeof(t)) == t);
  CHECK(!strcmp(t, "0.0.0.0"));
  inet_pton(AF_INET, "255.255.255.255", b);
  CHECK(inet_ntop(AF_INET, b, t, 15) == NULL && errno == ENOSPC);
  CHECK(inet_ntop(AF_INET, b, t, 16) && !strcmp(t, "255.255.255.255"));
  CHECK_EQ(inet_aton("127.1", &a), 1);
  CHECK_EQ(ntohl(a.s_addr), 0x7F000001u);
  CHECK_EQ(inet_aton("0x7f.0.0.1", &a), 1);
  CHECK_EQ(ntohl(a.s_addr), 0x7F000001u);
  CHECK_EQ(inet_aton("10.1.65535", &a), 1);
  CHECK_EQ(ntohl(a.s_addr), 0x0A01FFFFu);
  CHECK_EQ(inet_aton("1.2.3.256", &a), 0);
  CHECK_EQ(inet_aton("", &a), 0);
  CHECK_EQ(inet_aton("1.2.3.4.5", &a), 0);
  CHECK_EQ(inet_addr("nope"), INADDR_NONE);
  a.s_addr = htonl(IP(10, 20, 30, 40));
  CHECK(!strcmp(inet_ntoa(a), "10.20.30.40"));
}

static void t_nameinfo(void) {
  struct sockaddr_in a = addr(IP(8, 8, 4, 4), 853);
  char host[NI_MAXHOST], serv[NI_MAXSERV];
  CHECK_EQ(getnameinfo((struct sockaddr *)&a, sizeof(a), host, sizeof(host),
                       serv, sizeof(serv), 0),
           0);
  CHECK(!strcmp(host, "8.8.4.4") && !strcmp(serv, "853"));
  CHECK_EQ(getnameinfo((struct sockaddr *)&a, sizeof(a), host, sizeof(host),
                       NULL, 0, NI_NAMEREQD),
           EAI_NONAME);
  CHECK_EQ(getnameinfo((struct sockaddr *)&a, sizeof(a), host, 4, NULL, 0, 0),
           EAI_OVERFLOW);
}

static uint32_t ai_ip(const struct addrinfo *ai) {
  return ntohl(((const struct sockaddr_in *)ai->ai_addr)->sin_addr.s_addr);
}

static uint16_t ai_port(const struct addrinfo *ai) {
  return ntohs(((const struct sockaddr_in *)ai->ai_addr)->sin_port);
}

static void t_getaddrinfo(void) {
  struct addrinfo h, *r = NULL;
  start(125);
  CHECK_EQ(getaddrinfo("10.1.2.3", "8080", NULL, &r), 0);
  CHECK(r && r->ai_next && !r->ai_next->ai_next);
  CHECK(r && r->ai_socktype == SOCK_STREAM && r->ai_protocol == IPPROTO_TCP);
  CHECK(r && r->ai_next->ai_socktype == SOCK_DGRAM);
  CHECK(r && ai_ip(r) == IP(10, 1, 2, 3) && ai_port(r) == 8080);
  CHECK(r && r->ai_family == AF_INET &&
        r->ai_addrlen == sizeof(struct sockaddr_in));
  freeaddrinfo(r);
  CHECK_EQ(g_model.ops[WIFI_NETOP_DNS], 0);

  memset(&h, 0, sizeof(h));
  h.ai_socktype = SOCK_STREAM;
  h.ai_flags = AI_CANONNAME;
  CHECK_EQ(getaddrinfo("echo.test", "http", &h, &r), 0);
  CHECK(r && !r->ai_next && ai_ip(r) == ECHO_IP && ai_port(r) == 80);
  CHECK(r && r->ai_canonname && !strcmp(r->ai_canonname, "echo.test"));
  freeaddrinfo(r);
  CHECK_EQ(g_model.ops[WIFI_NETOP_DNS], 1);
  CHECK_EQ(getaddrinfo("echo.test", NULL, &h, &r), 0);
  freeaddrinfo(r);
  CHECK_EQ(g_model.ops[WIFI_NETOP_DNS], 1); /* AppNet keeps names */

  CHECK_EQ(getaddrinfo("nobody.test", "80", &h, &r), EAI_NONAME);
  CHECK(r == NULL);
  CHECK(strlen(gai_strerror(EAI_NONAME)) > 0);
  CHECK_EQ(getaddrinfo("10.0.0.1", "nope", &h, &r), EAI_SERVICE);
  h.ai_flags = AI_NUMERICSERV;
  CHECK_EQ(getaddrinfo("10.0.0.1", "http", &h, &r), EAI_NONAME);
  h.ai_flags = AI_NUMERICHOST;
  CHECK_EQ(getaddrinfo("echo.test", "80", &h, &r), EAI_NONAME);
  h.ai_flags = AI_PASSIVE;
  CHECK_EQ(getaddrinfo(NULL, "5000", &h, &r), 0);
  CHECK(r && ai_ip(r) == 0 && ai_port(r) == 5000);
  freeaddrinfo(r);
  h.ai_flags = 0;
  CHECK_EQ(getaddrinfo(NULL, "5000", &h, &r), 0);
  CHECK(r && ai_ip(r) == IP(127, 0, 0, 1));
  freeaddrinfo(r);
  CHECK_EQ(getaddrinfo("localhost", "1", &h, &r), 0);
  CHECK(r && ai_ip(r) == IP(127, 0, 0, 1));
  freeaddrinfo(r);
  CHECK_EQ(getaddrinfo(NULL, NULL, &h, &r), EAI_NONAME);
  h.ai_family = AF_INET6;
  CHECK_EQ(getaddrinfo("10.0.0.1", "1", &h, &r), EAI_FAMILY);
  h.ai_family = AF_UNSPEC;
  h.ai_flags = 0x8000;
  CHECK_EQ(getaddrinfo("10.0.0.1", "1", &h, &r), EAI_BADFLAGS);
  h.ai_flags = 0;
  h.ai_socktype = SOCK_RAW;
  CHECK_EQ(getaddrinfo("10.0.0.1", "1", &h, &r), EAI_SOCKTYPE);

  struct hostent *he = gethostbyname("other.test");
  CHECK(he && he->h_addrtype == AF_INET && he->h_length == 4);
  CHECK(he && ntohl(*(uint32_t *)he->h_addr) == OTHER_IP);
  CHECK(gethostbyname("nobody.test") == NULL && h_errno == HOST_NOT_FOUND);
  CHECK_EQ(g_model.bg_stopped, 0);
}

static void t_socket_args(void) {
  int fds[APP_SOCK_COUNT], fd;
  struct sockaddr_in a = addr(ECHO_IP, 7);
  char c;
  start(125);
  errno = 0;
  CHECK(socket(AF_INET6, SOCK_STREAM, 0) == -1 && errno == EAFNOSUPPORT);
  CHECK(socket(AF_INET, SOCK_DGRAM, 0) == -1 && errno == EPROTONOSUPPORT);
  CHECK(socket(AF_INET, SOCK_STREAM, IPPROTO_UDP) == -1 &&
        errno == EPROTONOSUPPORT);
  for (int i = 0; i < APP_SOCK_COUNT; i++) {
    fds[i] = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    CHECK_EQ(fds[i], APP_SOCK_FIRST + i);
  }
  CHECK(socket(AF_INET, SOCK_STREAM, 0) == -1 && errno == EMFILE);
  for (int i = 0; i < APP_SOCK_COUNT; i++)
    CHECK_EQ(sclose(fds[i]), 0);
  CHECK_EQ(sclose(APP_SOCK_FIRST), -EBADF);

  CHECK(connect(1, (struct sockaddr *)&a, sizeof(a)) == -1 &&
        errno == ENOTSOCK);
  CHECK(connect(APP_SOCK_FIRST + 3, (struct sockaddr *)&a, sizeof(a)) == -1 &&
        errno == EBADF);
  fd = socket(AF_INET, SOCK_STREAM, 0);
  CHECK(recv(fd, &c, 1, 0) == -1 && errno == ENOTCONN);
  CHECK(send(fd, "x", 1, 0) == -1 && errno == ENOTCONN);
  CHECK(connect(fd, (struct sockaddr *)&a, 8) == -1 && errno == EINVAL);
  a.sin_family = AF_INET6;
  CHECK(connect(fd, (struct sockaddr *)&a, sizeof(a)) == -1 &&
        errno == EAFNOSUPPORT);
  a = addr(IP(127, 0, 0, 1), 7);
  CHECK(connect(fd, (struct sockaddr *)&a, sizeof(a)) == -1 &&
        errno == ECONNREFUSED);
  a = addr(ECHO_IP, 0);
  CHECK(connect(fd, (struct sockaddr *)&a, sizeof(a)) == -1 &&
        errno == ECONNREFUSED);
  a = addr(0, 0);
  CHECK_EQ(bind(fd, (struct sockaddr *)&a, sizeof(a)), 0);
  a = addr(0, 8000);
  CHECK(bind(fd, (struct sockaddr *)&a, sizeof(a)) == -1 &&
        errno == EADDRNOTAVAIL);
  CHECK(listen(fd, 1) == -1 && errno == EOPNOTSUPP);
  CHECK(accept(fd, NULL, NULL) == -1 && errno == EOPNOTSUPP);
  CHECK_EQ(g_model.connects, 0);
  sclose(fd);
}

static void t_echo(void) {
  char buf[64];
  struct sockaddr_in p;
  socklen_t len = sizeof(p);
  int v = -1;
  socklen_t vl = sizeof(v);
  struct stat st;
  start(125);
  int fd = tcp_to(ECHO_IP, 7, 0);
  CHECK(fd >= 0);
  CHECK_EQ(g_model.connects, 1);
  CHECK_EQ(send(fd, "hello", 5, 0), 5);
  CHECK_EQ(recv(fd, buf, sizeof(buf), 0), 5);
  CHECK(!memcmp(buf, "hello", 5));
  CHECK_EQ(getpeername(fd, (struct sockaddr *)&p, &len), 0);
  CHECK(len == sizeof(p) && ntohl(p.sin_addr.s_addr) == ECHO_IP &&
        ntohs(p.sin_port) == 7);
  CHECK_EQ(getsockopt(fd, SOL_SOCKET, SO_ERROR, &v, &vl), 0);
  CHECK_EQ(v, 0);
  CHECK_EQ(getsockopt(fd, SOL_SOCKET, SO_TYPE, &v, &vl), 0);
  CHECK_EQ(v, SOCK_STREAM);
  CHECK_EQ(getsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &v, &vl), 0);
  CHECK_EQ(v, 1);
  CHECK_EQ(setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &v, sizeof(v)), 0);
  CHECK(setsockopt(fd, SOL_SOCKET, 999, &v, sizeof(v)) == -1 &&
        errno == ENOPROTOOPT);
  len = sizeof(p);
  CHECK_EQ(getsockname(fd, (struct sockaddr *)&p, &len), 0);
  CHECK_EQ(ntohl(p.sin_addr.s_addr), IP(192, 168, 1, 50));
  /* The descriptor as read(), write() and fstat() reach it. */
  CHECK_EQ(g_app_sock->write(fd, "abc", 3), 3);
  CHECK_EQ(recv(fd, buf, 2, MSG_PEEK), 2);
  CHECK_EQ(recv(fd, buf, 3, MSG_WAITALL), 3);
  CHECK(!memcmp(buf, "abc", 3));
  CHECK_EQ(g_app_sock->write(fd, "xyz", 3), 3);
  CHECK_EQ(g_app_sock->read(fd, buf, 3), 3);
  CHECK(!memcmp(buf, "xyz", 3));
  CHECK_EQ(g_app_sock->fstat(fd, &st), 0);
  CHECK(S_ISSOCK(st.st_mode));
  CHECK_EQ(sclose(fd), 0);
  CHECK_EQ(g_model.bg, 1); /* the core is still ending it */
  settle_model();
  CHECK_EQ(g_model.fins, 1);
  CHECK_EQ(g_model.aborts, 0);
  CHECK_EQ(g_model.bg_stopped, 0);
  /* The rings are free again, so a lookup goes through. */
  CHECK(net_resolve("other.test", buf));
  CHECK(!strcmp(buf, "10.0.0.6"));
}

static void t_bulk(void) {
  static uint8_t big[200000], back[200000];
  uint32_t got = 0;
  ssize_t n;
  int bad = 0;
  start(125);
  int fd = tcp_to(ECHO_IP, 19, 0);
  CHECK(fd >= 0);
  while ((n = recv(fd, back + got, 7000, 0)) > 0)
    got += (uint32_t)n;
  CHECK_EQ(n, 0);
  CHECK_EQ(got, 100000);
  for (uint32_t i = 0; i < got; i++)
    bad += back[i] != model_pattern(i);
  CHECK_EQ(bad, 0);
  CHECK_EQ(recv(fd, back, 10, 0), 0);
  CHECK_EQ(send(fd, "after", 5, 0), 5); /* the server only closed its side */
  sclose(fd);
  settle_model();
  CHECK_EQ(g_model.fins, 1);

  /* 200 KB each way through the 16 KB and 32 KB rings. */
  fd = tcp_to(ECHO_IP, 7, 0);
  for (uint32_t i = 0; i < sizeof(big); i++)
    big[i] = (uint8_t)(i * 13u + 5u);
  CHECK_EQ(send(fd, big, 50000, 0), 50000);
  CHECK_EQ(recv(fd, back, 50000, MSG_WAITALL), 50000);
  CHECK(!memcmp(big, back, 50000));
  CHECK_EQ(send(fd, big, sizeof(big), 0), (ssize_t)sizeof(big));
  CHECK_EQ(recv(fd, back, sizeof(back), MSG_WAITALL), (ssize_t)sizeof(back));
  CHECK(!memcmp(big, back, sizeof(big)));
  CHECK_EQ(g_model.got, 5 + 250000); /* "after" too */
  sclose(fd);
  settle_model();
}

static void t_http(void) {
  static char buf[8192];
  const char req[] = "GET / HTTP/1.0\r\nHost: echo.test\r\n\r\n";
  struct addrinfo h, *r;
  uint32_t got = 0;
  ssize_t n;
  int fd;
  start(125);
  memset(&h, 0, sizeof(h));
  h.ai_socktype = SOCK_STREAM;
  CHECK_EQ(getaddrinfo("echo.test", "80", &h, &r), 0);
  fd = socket(r->ai_family, r->ai_socktype, r->ai_protocol);
  CHECK_EQ(connect(fd, r->ai_addr, r->ai_addrlen), 0);
  freeaddrinfo(r);
  CHECK_EQ(send(fd, req, sizeof(req) - 1, 0), (ssize_t)sizeof(req) - 1);
  while ((n = recv(fd, buf + got, sizeof(buf) - 1 - got, 0)) > 0)
    got += (uint32_t)n;
  buf[got] = 0;
  CHECK(!strncmp(buf, "HTTP/1.0 200 OK\r\nContent-Length: 5000\r\n\r\n", 41));
  CHECK_EQ(got, 41 + 5000);
  sclose(fd);
  settle_model();
  CHECK_EQ(g_model.bg_stopped, 0);
}

static void t_errors(void) {
  struct sockaddr_in a = addr(ECHO_IP, 4444);
  uint64_t t0;
  char buf[64];
  int fd;
  start(125);
  fd = socket(AF_INET, SOCK_STREAM, 0);
  CHECK(connect(fd, (struct sockaddr *)&a, sizeof(a)) == -1 &&
        errno == ECONNREFUSED);
  a = addr(ECHO_IP, 7); /* the same socket may try again */
  CHECK_EQ(connect(fd, (struct sockaddr *)&a, sizeof(a)), 0);
  CHECK(connect(fd, (struct sockaddr *)&a, sizeof(a)) == -1 &&
        errno == EISCONN);
  sclose(fd);

  t0 = host_now_us();
  CHECK(tcp_to(ECHO_IP, 82, 0) == -1 && errno == ETIMEDOUT);
  CHECK(host_now_us() - t0 >= 20000000u);
  CHECK(tcp_to(NOHOST, 7, 0) == -1 && errno == EHOSTUNREACH);

  fd = tcp_to(ECHO_IP, 83, 0); /* echoes 10 bytes, then resets */
  CHECK(fd >= 0);
  CHECK_EQ(send(fd, "0123456789abcdef", 16, 0), 16);
  CHECK_EQ(recv(fd, buf, sizeof(buf), MSG_WAITALL), 16);
  CHECK(recv(fd, buf, sizeof(buf), 0) == -1 && errno == ECONNRESET);
  CHECK_EQ(recv(fd, buf, sizeof(buf), 0), 0);
  CHECK(send(fd, "x", 1, 0) == -1 && errno == EPIPE);
  sclose(fd);

  model_offline();
  CHECK(tcp_to(ECHO_IP, 7, 0) == -1 && errno == ENETDOWN);
}

static void t_old_core(void) {
  start(123);
  CHECK(tcp_to(ECHO_IP, 7, 0) == -1 && errno == ENETDOWN);
  CHECK(strstr(net_error(), "newer") != NULL);
  CHECK_EQ(g_model.connects, 0);
}

static void t_nonblocking(void) {
  static char big[65536];
  struct pollfd p;
  char buf[64];
  int v = -1, fl;
  socklen_t vl = sizeof(v);
  start(125);
  int fd = tcp_to(ECHO_IP, 7, SOCK_NONBLOCK);
  CHECK(fd >= 0);
  CHECK(recv(fd, buf, 1, 0) == -1 && errno == EAGAIN);
  p.fd = fd;
  p.events = POLLOUT;
  CHECK_EQ(poll(&p, 1, 1000), 1);
  CHECK(p.revents & POLLOUT);
  CHECK_EQ(getsockopt(fd, SOL_SOCKET, SO_ERROR, &v, &vl), 0);
  CHECK_EQ(v, 0);
  CHECK_EQ(send(fd, "ping", 4, 0), 4);
  p.events = POLLIN;
  CHECK_EQ(poll(&p, 1, 1000), 1);
  CHECK(p.revents & POLLIN);
  v = 0;
  CHECK_EQ(ioctl(fd, FIONREAD, &v), 0);
  CHECK_EQ(v, 4);
  CHECK_EQ(recv(fd, buf, sizeof(buf), 0), 4);
  CHECK(recv(fd, buf, 1, 0) == -1 && errno == EAGAIN);
  fl = fcntl(fd, F_GETFL);
  CHECK(fl & O_NONBLOCK);
  CHECK_EQ(fcntl(fd, F_SETFL, fl & ~O_NONBLOCK), 0);
  CHECK(!(fcntl(fd, F_GETFL) & O_NONBLOCK));
  CHECK(recv(fd, buf, 1, MSG_DONTWAIT) == -1 && errno == EAGAIN);
  v = 1;
  CHECK_EQ(ioctl(fd, FIONBIO, &v), 0);
  CHECK(fcntl(fd, F_GETFL) & O_NONBLOCK);
  sclose(fd);
  settle_model();

  /* A server that reads nothing: the 16 KB tx ring fills, then EAGAIN. */
  fd = tcp_to(ECHO_IP, 9, SOCK_NONBLOCK);
  p.fd = fd;
  p.events = POLLOUT;
  poll(&p, 1, 1000);
  CHECK_EQ(send(fd, big, sizeof(big), 0), WIFI_TCP_TX);
  CHECK(send(fd, big, 10, 0) == -1 && errno == EAGAIN);
  CHECK_EQ(poll(&p, 1, 100), 0);
  sclose(fd);
  settle_model();

  /* A non-blocking connect that is refused. */
  fd = tcp_to(ECHO_IP, 4444, SOCK_NONBLOCK);
  p.fd = fd;
  p.events = POLLOUT;
  CHECK_EQ(poll(&p, 1, 1000), 1);
  CHECK(p.revents & POLLERR);
  vl = sizeof(v);
  CHECK_EQ(getsockopt(fd, SOL_SOCKET, SO_ERROR, &v, &vl), 0);
  CHECK_EQ(v, ECONNREFUSED);
  CHECK_EQ(getsockopt(fd, SOL_SOCKET, SO_ERROR, &v, &vl), 0);
  CHECK_EQ(v, 0); /* reading it clears it */
  sclose(fd);
  CHECK_EQ(fcntl(999, F_GETFL), -1);
  CHECK_EQ(errno, EBADF);
}

static void t_timeouts(void) {
  static char big[65536];
  struct timeval tv = {0, 300000};
  fd_set rd;
  uint64_t t0;
  char buf[16];
  start(125);
  int fd = tcp_to(ECHO_IP, 81, 0); /* holds the line, says nothing */
  CHECK_EQ(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)), 0);
  t0 = host_now_us();
  CHECK(recv(fd, buf, 1, 0) == -1 && errno == EAGAIN);
  CHECK(host_now_us() - t0 >= 300000u && host_now_us() - t0 < 400000u);
  tv.tv_usec = 250000;
  FD_ZERO(&rd);
  FD_SET(fd, &rd);
  t0 = host_now_us();
  CHECK_EQ(select(fd + 1, &rd, NULL, NULL, &tv), 0);
  CHECK(!FD_ISSET(fd, &rd));
  CHECK(host_now_us() - t0 >= 250000u);
  sclose(fd);
  settle_model();

  fd = tcp_to(ECHO_IP, 7, 0);
  CHECK_EQ(send(fd, "z", 1, 0), 1);
  FD_ZERO(&rd);
  FD_SET(fd, &rd);
  tv.tv_sec = 1;
  CHECK_EQ(select(fd + 1, &rd, NULL, NULL, &tv), 1);
  CHECK(FD_ISSET(fd, &rd));
  FD_ZERO(&rd);
  FD_SET(5, &rd); /* not open */
  CHECK(select(6, &rd, NULL, NULL, &tv) == -1 && errno == EBADF);
  sclose(fd);
  settle_model();

  fd = tcp_to(ECHO_IP, 9, 0);
  tv.tv_sec = 0;
  tv.tv_usec = 200000;
  CHECK_EQ(setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)), 0);
  CHECK_EQ(send(fd, big, sizeof(big), 0), WIFI_TCP_TX);
  CHECK(send(fd, big, 1, 0) == -1 && errno == EAGAIN);
  sclose(fd);
}

static void t_one_connection(void) {
  char ip[16];
  struct addrinfo h, *r;
  start(125);
  CHECK(net_resolve("echo.test", ip)); /* kept by AppNet */
  int a = tcp_to(ECHO_IP, 7, 0), b;
  CHECK(a >= 0);
  CHECK(tcp_to(OTHER_IP, 7, 0) == -1 && errno == EBUSY);
  CHECK(strstr(net_error(), "another socket") != NULL);
  /* Nothing else may take the core's connection away meanwhile. */
  memset(&h, 0, sizeof(h));
  h.ai_socktype = SOCK_STREAM;
  CHECK_EQ(getaddrinfo("other.test", "7", &h, &r), EAI_AGAIN);
  CHECK(!net_resolve("other.test", ip));
  CHECK(strstr(net_error(), "socket") != NULL);
  CHECK(http_get("http://10.0.0.5/") == NULL);
  CHECK_EQ(net_ping("10.0.0.5"), -1);
  CHECK(net_connected()); /* a link check still goes through */
  CHECK(net_resolve("echo.test", ip));
  CHECK_EQ(getaddrinfo("echo.test", "7", &h, &r), 0);
  freeaddrinfo(r);
  CHECK_EQ(g_model.bg_stopped, 0);
  CHECK_EQ(g_model.ops[WIFI_NETOP_DNS], 1);
  CHECK_EQ(send(a, "still", 5, 0), 5);
  CHECK_EQ(recv(a, ip, 5, MSG_WAITALL), 5);
  sclose(a);
  /* The next connect waits for the FIN rather than failing. */
  b = tcp_to(OTHER_IP, 7, 0);
  CHECK(b >= 0);
  CHECK_EQ(g_model.fins, 1);
  sclose(b);
  /* So does the next HTTP call. */
  CHECK(net_resolve("other.test", ip));
  CHECK_EQ(g_model.fins, 2);
  CHECK_EQ(g_model.bg_stopped, 0);
}

static void t_unread_data(void) {
  char ip[16];
  start(125);
  int fd = tcp_to(ECHO_IP, 83, 0);
  send(fd, "0123456789", 10, 0);
  for (int i = 0; i < 100 && g_model.bg; i++)
    host_frames(1);
  CHECK_EQ(g_model.bg, 0); /* reset, with 10 bytes not read yet */
  CHECK(!net_resolve("other.test", ip)); /* they stay in the rings */
  CHECK_EQ(recv(fd, ip, 10, MSG_WAITALL), 10);
  CHECK(!memcmp(ip, "0123456789", 10));
  CHECK(recv(fd, ip, 1, 0) == -1 && errno == ECONNRESET);
  CHECK(net_resolve("other.test", ip)); /* read out, they are given back */
  sclose(fd);
}

static void t_keepalive(void) {
  char c;
  struct timeval tv = {0, 100000};
  start(125);
  int fd = tcp_to(ECHO_IP, 81, 0);
  host_frames(60 * 20);
  CHECK_EQ(g_model.bg, 1);
  CHECK_EQ(g_model.alive_drops, 0);
  g_host_lid_us = 20000000u;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  CHECK(recv(fd, &c, 1, 0) == -1 && errno == EAGAIN);
  CHECK_EQ(g_model.bg, 1);
  /* An app that stops calling into the runtime loses it after 15 s. */
  host_advance(16000000u);
  model_step();
  CHECK_EQ(g_model.alive_drops, 1);
  CHECK(recv(fd, &c, 1, 0) == -1 && errno == ETIMEDOUT);
  sclose(fd);
}

static void t_close_kinds(void) {
  struct linger l = {1, 0};
  char buf[8];
  start(125);
  int fd = tcp_to(ECHO_IP, 7, 0);
  CHECK_EQ(setsockopt(fd, SOL_SOCKET, SO_LINGER, &l, sizeof(l)), 0);
  sclose(fd);
  settle_model();
  CHECK_EQ(g_model.aborts, 1);
  CHECK_EQ(g_model.fins, 0);

  fd = tcp_to(ECHO_IP, 7, 0);
  CHECK_EQ(send(fd, "ab", 2, 0), 2);
  CHECK_EQ(shutdown(fd, SHUT_WR), 0);
  CHECK(send(fd, "c", 1, 0) == -1 && errno == EPIPE);
  CHECK_EQ(recv(fd, buf, 2, MSG_WAITALL), 2); /* replies still come */
  CHECK_EQ(g_model.fins, 0);
  CHECK_EQ(shutdown(fd, SHUT_RD), 0);
  CHECK_EQ(recv(fd, buf, 2, 0), 0);
  sclose(fd);
  settle_model();
  CHECK_EQ(g_model.fins, 1);

  fd = tcp_to(ECHO_IP, 81, 0);
  g_app_sock->leave();
  CHECK_EQ(g_model.bg, 0);
  CHECK_EQ(g_model.fins, 2);
}

static void start_udp(void) {
  start(AUDIO_UDP_VERSION);
  model_udp_peer(ECHO_IP, 7, UP_ECHO, 0);
  model_udp_peer(OTHER_IP, 7, UP_ECHO, 0);
  model_udp_peer(ECHO_IP, 9, UP_BLAST, 20);
  model_udp_peer(ECHO_IP, 13, UP_SINK, 0);
}

static int udp_sock(int flags) { return socket(AF_INET, SOCK_DGRAM | flags, 0); }

static WifiUdp *udp_block(void) { return (WifiUdp *)(uintptr_t)WIFI_UDP_ADDR; }

static void t_udp_echo(void) {
  struct sockaddr_in to = addr(ECHO_IP, 7), from, me;
  socklen_t fl = sizeof(from), ml = sizeof(me);
  char buf[64];
  int a, b;
  start_udp();
  a = udp_sock(0);
  CHECK(a >= 0);
  CHECK_EQ(g_model.udp_binds, 0); /* not until it is used */
  CHECK_EQ(sendto(a, "hello", 5, 0, (struct sockaddr *)&to, sizeof(to)), 5);
  CHECK_EQ(g_model.udp_binds, 1);
  CHECK_EQ(recvfrom(a, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl), 5);
  CHECK(!memcmp(buf, "hello", 5));
  CHECK(fl == sizeof(from) && ntohl(from.sin_addr.s_addr) == ECHO_IP &&
        ntohs(from.sin_port) == 7);
  CHECK_EQ(getsockname(a, (struct sockaddr *)&me, &ml), 0);
  CHECK_EQ(ntohs(me.sin_port), 50123);
  CHECK(getpeername(a, (struct sockaddr *)&me, &ml) == -1 && errno == ENOTCONN);

  /* One port for the console: a second socket cannot have one too. */
  b = udp_sock(0);
  me = addr(0, 6000);
  CHECK(bind(b, (struct sockaddr *)&me, sizeof(me)) == -1 &&
        errno == EADDRINUSE);
  CHECK(strstr(net_error(), "UDP") != NULL);
  CHECK_EQ(sclose(a), 0);
  CHECK_EQ(g_model.udp_unbinds, 1);
  CHECK_EQ(bind(b, (struct sockaddr *)&me, sizeof(me)), 0);
  CHECK(bind(b, (struct sockaddr *)&me, sizeof(me)) == -1 && errno == EINVAL);
  ml = sizeof(me);
  CHECK_EQ(getsockname(b, (struct sockaddr *)&me, &ml), 0);
  CHECK_EQ(ntohs(me.sin_port), 6000);
  me = addr(0, 68);
  a = udp_sock(0);
  sclose(b);
  CHECK(bind(a, (struct sockaddr *)&me, sizeof(me)) == -1 &&
        errno == EADDRNOTAVAIL); /* the DHCP client's */
  me = addr(IP(10, 9, 9, 9), 0);
  CHECK(bind(a, (struct sockaddr *)&me, sizeof(me)) == -1 &&
        errno == EADDRNOTAVAIL); /* not our address */
  sclose(a);
  CHECK_EQ(g_model.bg_stopped, 0);
}

static void t_udp_connected(void) {
  struct sockaddr_in to = addr(ECHO_IP, 7), other = addr(OTHER_IP, 7), un;
  char buf[64];
  int fd;
  start_udp();
  fd = udp_sock(0);
  CHECK_EQ(connect(fd, (struct sockaddr *)&to, sizeof(to)), 0);
  CHECK_EQ(send(fd, "one", 3, 0), 3);
  /* The other peer's answer is not for a socket connected to the first. */
  CHECK_EQ(sendto(fd, "two", 3, 0, (struct sockaddr *)&other, sizeof(other)), 3);
  CHECK_EQ(send(fd, "three", 5, 0), 5);
  CHECK_EQ(recv(fd, buf, sizeof(buf), 0), 3);
  CHECK(!memcmp(buf, "one", 3));
  CHECK_EQ(recv(fd, buf, sizeof(buf), 0), 5);
  CHECK(!memcmp(buf, "three", 5));
  CHECK(recv(fd, buf, sizeof(buf), MSG_DONTWAIT) == -1 && errno == EAGAIN);
  CHECK_EQ(g_app_sock->write(fd, "four", 4), 4);
  CHECK_EQ(g_app_sock->read(fd, buf, sizeof(buf)), 4);
  memset(&un, 0, sizeof(un));
  un.sin_family = AF_UNSPEC;
  CHECK_EQ(connect(fd, (struct sockaddr *)&un, sizeof(un)), 0);
  CHECK(send(fd, "x", 1, 0) == -1 && errno == EDESTADDRREQ);
  sclose(fd);
}

static void t_udp_limits(void) {
  static char big[600];
  struct sockaddr_in to = addr(ECHO_IP, 7), bc = addr(0xFFFFFFFFu, 7);
  int one = 1, fd, n = 0;
  char buf[700];
  start_udp();
  fd = udp_sock(0);
  CHECK(sendto(fd, big, WIFI_UDP_SEND_MAX + 1, 0, (struct sockaddr *)&to,
               sizeof(to)) == -1 &&
        errno == EMSGSIZE);
  CHECK_EQ(sendto(fd, big, WIFI_UDP_SEND_MAX, 0, (struct sockaddr *)&to,
                  sizeof(to)),
           WIFI_UDP_SEND_MAX);
  CHECK_EQ(recv(fd, buf, sizeof(buf), 0), WIFI_UDP_SEND_MAX);
  CHECK(sendto(fd, "b", 1, 0, (struct sockaddr *)&bc, sizeof(bc)) == -1 &&
        errno == EACCES);
  CHECK_EQ(setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one)), 0);
  CHECK_EQ(sendto(fd, "b", 1, 0, (struct sockaddr *)&bc, sizeof(bc)), 1);
  for (int i = 0; i < 10; i++) {
    host_frames(1);
    while (recv(fd, buf, sizeof(buf), MSG_DONTWAIT) == 1)
      n++;
  }
  CHECK_EQ(g_model.udp_to_bcast, 1);
  CHECK_EQ(n, 2); /* both echo peers answer a broadcast */
  to = addr(ECHO_IP, 0);
  CHECK(sendto(fd, "x", 1, 0, (struct sockaddr *)&to, sizeof(to)) == -1 &&
        errno == EINVAL);
  to = addr(IP(127, 0, 0, 1), 7);
  CHECK(sendto(fd, "x", 1, 0, (struct sockaddr *)&to, sizeof(to)) == -1 &&
        errno == ENETUNREACH);
  CHECK(send(fd, "x", 1, 0) == -1 && errno == EDESTADDRREQ);
  sclose(fd);
}

static void t_udp_sizes(void) {
  struct sockaddr_in to = addr(ECHO_IP, 9), from;
  socklen_t fl = sizeof(from);
  char buf[700];
  int fd, got = 0, v = 0;
  start_udp();
  fd = udp_sock(0);
  CHECK_EQ(sendto(fd, "go", 2, 0, (struct sockaddr *)&to, sizeof(to)), 2);
  host_frames(5);
  /* 20 datagrams of 600 bytes; 13 fit the 8 KB ring, the rest are dropped. */
  CHECK_EQ(udp_block()->dropped, 7);
  CHECK_EQ(ioctl(fd, FIONREAD, &v), 0);
  CHECK_EQ(v, 600);
  CHECK_EQ(recv(fd, buf, 100, MSG_PEEK), 100);
  CHECK_EQ(recv(fd, buf, 100, MSG_PEEK | MSG_TRUNC), 600);
  CHECK_EQ(recvfrom(fd, buf, 100, 0, (struct sockaddr *)&from, &fl), 100);
  CHECK(buf[0] == 0 && ntohs(from.sin_port) == 9);
  CHECK_EQ(recv(fd, buf, sizeof(buf), 0), 600);
  CHECK(buf[0] == 1 && buf[599] == 1);
  while (recv(fd, buf, sizeof(buf), MSG_DONTWAIT) == 600)
    got++;
  CHECK_EQ(got, 11);
  CHECK(errno == EAGAIN);
  sclose(fd);
}

static void t_udp_poll(void) {
  struct sockaddr_in to = addr(ECHO_IP, 7);
  struct pollfd p;
  struct timeval tv = {0, 200000};
  fd_set rd;
  char buf[16];
  int fd;
  start_udp();
  fd = udp_sock(SOCK_NONBLOCK);
  p.fd = fd;
  p.events = POLLIN | POLLOUT;
  CHECK_EQ(poll(&p, 1, 0), 1);
  CHECK(p.revents == (POLLOUT | POLLWRNORM) || p.revents == POLLOUT);
  CHECK(recv(fd, buf, sizeof(buf), 0) == -1 && errno == EAGAIN);
  p.events = POLLIN;
  CHECK_EQ(poll(&p, 1, 50), 0);
  sendto(fd, "hi", 2, 0, (struct sockaddr *)&to, sizeof(to));
  CHECK_EQ(poll(&p, 1, 1000), 1);
  CHECK(p.revents & POLLIN);
  CHECK_EQ(recv(fd, buf, sizeof(buf), 0), 2);
  FD_ZERO(&rd);
  FD_SET(fd, &rd);
  CHECK_EQ(select(fd + 1, &rd, NULL, NULL, &tv), 0);
  sclose(fd);
}

static void t_udp_beside_tcp(void) {
  struct sockaddr_in to = addr(OTHER_IP, 7);
  struct addrinfo h, *r;
  char buf[16];
  int t, u;
  start_udp();
  t = tcp_to(ECHO_IP, 7, 0);
  CHECK(t >= 0);
  u = udp_sock(0);
  CHECK_EQ(sendto(u, "dgram", 5, 0, (struct sockaddr *)&to, sizeof(to)), 5);
  CHECK_EQ(recv(u, buf, sizeof(buf), 0), 5);
  /* On this core a lookup leaves the connection running. */
  memset(&h, 0, sizeof(h));
  h.ai_socktype = SOCK_DGRAM;
  CHECK_EQ(getaddrinfo("other.test", "7", &h, &r), 0);
  CHECK(r && ai_ip(r) == OTHER_IP);
  freeaddrinfo(r);
  CHECK(net_resolve("echo.test", buf));
  CHECK_EQ(g_model.bg_stopped, 0);
  CHECK_EQ(send(t, "tcp", 3, 0), 3);
  CHECK_EQ(recv(t, buf, 3, MSG_WAITALL), 3);
  CHECK(!memcmp(buf, "tcp", 3));
  /* HTTP still needs the connection to itself. */
  CHECK(http_get("http://10.0.0.5/") == NULL);
  CHECK_EQ(g_model.bg_stopped, 0);
  sclose(t);
  sclose(u);
  settle_model();
}

static void t_end_status(void) {
  struct addrinfo h, *r;
  char buf[16];
  start(AUDIO_UDP_VERSION);
  int fd = tcp_to(ECHO_IP, 83, 0); /* echoes 10 bytes, then resets */
  send(fd, "0123456789", 10, 0);
  for (int i = 0; i < 100 && g_model.bg; i++)
    host_frames(1);
  /* The lookup writes WifiNetIo.status over the connection's REFUSED. */
  memset(&h, 0, sizeof(h));
  CHECK_EQ(getaddrinfo("other.test", NULL, &h, &r), 0);
  freeaddrinfo(r);
  CHECK_EQ(recv(fd, buf, 10, MSG_WAITALL), 10);
  CHECK(recv(fd, buf, 1, 0) == -1 && errno == ECONNRESET);
  sclose(fd);
}

static void t_udp_old_core(void) {
  start(125);
  CHECK(udp_sock(0) == -1 && errno == EPROTONOSUPPORT);
}

static void t_udp_alive(void) {
  struct sockaddr_in to = addr(ECHO_IP, 13), nohost = addr(NOHOST, 13);
  char c;
  start_udp();
  int fd = udp_sock(0);
  CHECK_EQ(sendto(fd, "x", 1, 0, (struct sockaddr *)&to, sizeof(to)), 1);
  CHECK_EQ(sendto(fd, "y", 1, 0, (struct sockaddr *)&nohost, sizeof(nohost)), 1);
  host_frames(60 * 20);
  CHECK_EQ(g_model.udp_on, 1);
  CHECK_EQ(g_model.udp_alive_drops, 0);
  CHECK_EQ(udp_block()->unsent, 1); /* no ARP answer for that one */
  host_advance(16000000u);
  model_step();
  CHECK_EQ(g_model.udp_alive_drops, 1);
  CHECK(recv(fd, &c, 1, 0) == -1 && errno == ENETDOWN);
  sclose(fd);

  fd = udp_sock(0);
  CHECK_EQ(sendto(fd, "x", 1, 0, (struct sockaddr *)&to, sizeof(to)), 1);
  g_app_sock->leave();
  CHECK_EQ(g_model.udp_on, 0);
  CHECK_EQ(g_model.udp_unbinds, 2);
}

SCENARIOS("sockets", S(t_inet), S(t_nameinfo), S(t_getaddrinfo),
          S(t_socket_args), S(t_echo), S(t_bulk), S(t_http), S(t_errors),
          S(t_old_core), S(t_nonblocking), S(t_timeouts),
          S(t_one_connection), S(t_unread_data), S(t_keepalive),
          S(t_close_kinds), S(t_udp_echo), S(t_udp_connected),
          S(t_udp_limits), S(t_udp_sizes), S(t_udp_poll),
          S(t_udp_beside_tcp), S(t_end_status), S(t_udp_old_core),
          S(t_udp_alive))
