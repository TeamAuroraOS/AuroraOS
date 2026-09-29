#ifndef AURORA_KEYBOARD_H
#define AURORA_KEYBOARD_H

#include "aurora.h"

/* On-screen keyboard (src/os/os_setup.c), shared by setup and the File
 * Explorer. */

enum {
  KB_NAME = 0,     /* letters, digits and space */
  KB_FILENAME = 1, /* adds - _ and . */
};

/* Edits `buf`, a `size`-byte buffer including the NUL, in place on the bottom
 * screen. `hint` shows while it is empty; NULL uses the user-name prompt.
 * OK or START returns 1. SELECT returns 0 and leaves what was typed in `buf`,
 * so a caller that can cancel should pass a copy. */
int keyboard_edit(char *buf, int size, int mode, const char *hint,
                  Color accent);

#endif
