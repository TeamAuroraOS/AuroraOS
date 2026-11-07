#ifndef _POLL_H_
#define _POLL_H_

#ifdef __cplusplus
extern "C" {
#endif

struct pollfd {
  int fd;
  short events;
  short revents;
};

typedef unsigned int nfds_t;

#define POLLIN     0x001
#define POLLPRI    0x002
#define POLLOUT    0x004
#define POLLERR    0x008
#define POLLHUP    0x010
#define POLLNVAL   0x020
#define POLLRDNORM 0x040
#define POLLRDBAND 0x080
#define POLLWRNORM 0x100
#define POLLWRBAND 0x200

/* Sockets, files (always ready) and the console. `timeout` in ms, -1 for
 * none; HOME is noticed while it waits. */
int poll(struct pollfd *fds, nfds_t n, int timeout);

#ifdef __cplusplus
}
#endif

#endif
