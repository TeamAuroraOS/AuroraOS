#ifndef _SYS_IOCTL_H_
#define _SYS_IOCTL_H_

#ifdef __cplusplus
extern "C" {
#endif

/* For sockets: FIONBIO takes an int (non-zero for non-blocking), FIONREAD
 * gives the bytes waiting as an int. */
#define FIONREAD 0x541B
#define FIONBIO  0x5421

int ioctl(int fd, unsigned long request, ...);

#ifdef __cplusplus
}
#endif

#endif
