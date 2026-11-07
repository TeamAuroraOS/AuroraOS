#ifndef HOST_RT_H
#define HOST_RT_H

#include <stdint.h>

/* Closes the lid for this long at the next app_lid_wait(). */
extern uint32_t g_host_lid_us;
/* `n` frames of an app_loop() that does nothing else: 16.7 ms each, with
 * HOME and the keep-alive looked at. */
void host_frames(uint32_t n);
/* A FatFs RAM disk of `mb` MB, formatted FAT32 and mounted as 0:. */
void host_ramdisk(uint32_t mb);

#endif
