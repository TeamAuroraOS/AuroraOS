#ifndef AURORA_FWDUMP_H
#define AURORA_FWDUMP_H

#include "aurora.h"

/* Copying the Wi-Fi firmware from this console's NWM system module to
 * SD:/Aurora/wifi, so nobody needs a PC for it. See docs/wifi.md. */

/* 1 when SD:/Aurora/wifi holds every file the Wi-Fi driver loads. Mounts the
 * card for the call. */
int fwdump_present(void);

/* The NAND warning and its button code, then the copy, its progress and its
 * result. Slides in, and back for the caller to redraw. 1 if the firmware is
 * on the card afterwards. */
int fwdump_run(void);

/* The copy itself, without drawing: `step` (may be 0) is told each step's
 * number, 0 to 4, as it starts. 0 on success, else the reason. */
const char *fwdump_copy(void (*step)(int n));

#endif
