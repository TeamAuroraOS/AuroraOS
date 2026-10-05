#ifndef AURORA_STORE_H
#define AURORA_STORE_H

#include "aurora.h"

/* Decoded store art (banners, the welcome bag, app icons), live only while
 * aShop is open: clear of the 3D Model's right-eye frame (to 0x26DF6500) and
 * the ARM11 mailbox at 0x27000000. */
#define STORE_ARENA_ADDR 0x26E00000u
#define STORE_ARENA_SIZE 0x00200000u

/* aShop, until the user backs out. It needs a linked Aurora account and
 * offers to link one; `owner` is the console owner's name from USER.dat, for
 * the name the console links under. Returns how many apps it put into
 * SD:/Aurora/Apps, so the caller knows to rescan them. */
int store_screen(const char *owner);

#endif
