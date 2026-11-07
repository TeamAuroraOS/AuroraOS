// Sockets: BSD sockets and <dirent.h>. Fetches a page over a TCP socket of
// its own, asks an NTP server for the time over UDP, and lists the app's data
// folder (SD:/Aurora/Apps/C/Sockets/) with scandir(). The bottom screen shows
// a spinner while a call waits, through net_on_wait().
//
// A: HTTP over TCP   X: the time over UDP (NTP)   Y: list the folder
// START: quit
#include <aurora_app.h>

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define WEB_HOST "example.com"
#define NTP_HOST "pool.ntp.org"
/* NTP counts from 1900, Unix time from 1970. */
#define NTP_TO_UNIX 2208988800u

static const char *doing = "";

static void bottom(const char *line, uint32_t ms) {
  gfx_clear(SCREEN_BOTTOM, RGB(24, 26, 32));
  gfx_print(SCREEN_BOTTOM, 16, 16, FONT_BOLD, COLOR_WHITE, "%s", line);
  if (ms) {
    int a = (int)(ms / 100u) % 8;
    for (int i = 0; i < 8; i++)
      gfx_circle(SCREEN_BOTTOM, 160 + (i % 4 - 2) * 18 + 9,
                 120 + (i / 4) * 18 - 9, 6,
                 i == a ? COLOR_AURORA : RGB(70, 74, 84));
  }
  gfx_present();
}

static void waiting(uint32_t ms) { bottom(doing, ms); }

/* The first address getaddrinfo() gives for host:service, as a socket of
 * `type`, connected for TCP. -1 with the reason printed. */
static int open_to(const char *host, const char *service, int type,
                   struct sockaddr_in *where) {
  struct addrinfo hints, *res;
  int fd, e;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = type;
  e = getaddrinfo(host, service, &hints, &res);
  if (e) {
    printf("%s: %s (%s)\n\n", host, gai_strerror(e), net_error());
    return -1;
  }
  *where = *(struct sockaddr_in *)res->ai_addr;
  fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
  if (fd >= 0 && type == SOCK_STREAM &&
      connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
    printf("connect: %s\n\n", strerror(errno));
    close(fd);
    fd = -1;
  }
  freeaddrinfo(res);
  return fd;
}

static void http_over_tcp(void) {
  static char reply[4096];
  const char req[] = "GET / HTTP/1.0\r\nHost: " WEB_HOST
                     "\r\nUser-Agent: AuroraOS sockets example\r\n\r\n";
  struct sockaddr_in where;
  struct timeval tv = {10, 0};
  size_t got = 0, total = 0;
  ssize_t n;
  uint32_t t0 = sys_millis();
  int fd;

  doing = "TCP: " WEB_HOST ":80";
  fd = open_to(WEB_HOST, "80", SOCK_STREAM, &where);
  if (fd < 0)
    return;
  printf("Connected to %s:80\n", inet_ntoa(where.sin_addr));
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  if (send(fd, req, sizeof(req) - 1, 0) < 0) {
    printf("send: %s\n\n", strerror(errno));
    close(fd);
    return;
  }
  // Read until the server closes, keeping the first 4 KB.
  while ((n = recv(fd, reply + got, sizeof(reply) - 1 - got, 0)) > 0) {
    total += (size_t)n;
    got += (size_t)n;
    if (got == sizeof(reply) - 1)
      got = sizeof(reply) - 1 - 1024; // keep reading, drop the tail
  }
  if (n < 0)
    printf("recv: %s\n", strerror(errno));
  close(fd);
  reply[got] = 0;
  char *eol = strstr(reply, "\r\n");
  if (eol)
    *eol = 0;
  printf("%s\n%u bytes in %u ms\n\n", reply, (unsigned)total,
         (unsigned)(sys_millis() - t0));
}

static void ntp_over_udp(void) {
  unsigned char pkt[48];
  struct sockaddr_in where, from;
  socklen_t fl = sizeof(from);
  struct timeval tv = {3, 0};
  ssize_t n;
  int fd;

  doing = "UDP: " NTP_HOST ":123";
  fd = open_to(NTP_HOST, "123", SOCK_DGRAM, &where);
  if (fd < 0)
    return;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  memset(pkt, 0, sizeof(pkt));
  pkt[0] = 0x23; // version 4, client
  if (sendto(fd, pkt, sizeof(pkt), 0, (struct sockaddr *)&where,
             sizeof(where)) < 0) {
    printf("sendto: %s\n\n", strerror(errno));
    close(fd);
    return;
  }
  n = recvfrom(fd, pkt, sizeof(pkt), 0, (struct sockaddr *)&from, &fl);
  close(fd);
  if (n < 48) {
    printf("No answer from %s: %s\n\n", NTP_HOST,
           n < 0 ? strerror(errno) : "too short");
    return;
  }
  // The transmit time: seconds since 1900 at bytes 40..43.
  uint32_t secs = (uint32_t)pkt[40] << 24 | (uint32_t)pkt[41] << 16 |
                  (uint32_t)pkt[42] << 8 | pkt[43];
  time_t t = (time_t)(secs - NTP_TO_UNIX);
  struct tm *u = gmtime(&t);
  printf("NTP from %s:\n  %04d-%02d-%02d %02d:%02d:%02d UTC\n\n",
         inet_ntoa(from.sin_addr), u->tm_year + 1900, u->tm_mon + 1,
         u->tm_mday, u->tm_hour, u->tm_min, u->tm_sec);
}

static void list_folder(void) {
  struct dirent **names;
  int n = scandir(app_dir(), &names, NULL, alphasort);
  if (n < 0) {
    printf("%s: %s\n\n", app_dir(), strerror(errno));
    return;
  }
  printf("%s: %d entries\n", app_dir(), n);
  for (int i = 0; i < n; i++) {
    printf("  %s%s\n", names[i]->d_name, names[i]->d_type == DT_DIR ? "/" : "");
    free(names[i]);
  }
  free(names);
  printf("\n");
}

int main(void) {
  printf("Sockets: TCP, UDP and folders\n\n");
  if (!net_available()) {
    printf("%s\n\n", net_error());
  } else {
    net_on_wait(waiting);
    doing = "Joining the Wi-Fi network";
    if (net_connect())
      printf("This console is %s\n\n", net_address());
    else
      printf("Not connected: %s\n\n", net_error());
  }
  printf("A: HTTP over TCP\nX: the time over UDP\nY: list %s\n"
         "START: quit\n\n",
         app_dir());
  bottom("Ready", 0);
  while (app_loop()) {
    uint32_t k = hid_keys_down();
    if (k & KEY_A)
      http_over_tcp();
    if (k & KEY_X)
      ntp_over_udp();
    if (k & KEY_Y)
      list_folder();
    if (k & (KEY_A | KEY_X | KEY_Y))
      bottom("Ready", 0);
    if (k & KEY_START)
      break;
  }
  return 0;
}
