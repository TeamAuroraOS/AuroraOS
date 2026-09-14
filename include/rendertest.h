#ifndef RENDERTEST_H
#define RENDERTEST_H

#include "aurora.h"

/* GPU Test > R: draws a UI frame for 60 seconds and reports the mean frame
 * rate. On a New 3DS it first switches the ARM11 to 804 MHz. */
void render_test_screen(void);

#endif
