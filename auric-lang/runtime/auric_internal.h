/* Shared by the runtime's own files (auric_runtime.c, auric_net.c); generated
 * code never sees it. */
#ifndef AURIC_INTERNAL_H
#define AURIC_INTERNAL_H

#include "aurora.h"

/* Launched by the Home Menu rather than booted directly. */
int aur_from_home(void);
/* The ARM9 timer, about 65 ticks per millisecond. */
u32 aur_ticks(void);
#define AUR_TICKS_PER_MS 65u
/* The card, mounted once and left mounted. */
int aur_mount(void);
/* A HOME press since the MCU was last asked. */
int aur_home_pressed(void);
/* Back to the Home Menu, sounds stopped; does not return. */
void aur_leave(void) __attribute__((noreturn));

#endif
