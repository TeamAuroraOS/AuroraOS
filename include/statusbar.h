#ifndef STATUSBAR_H
#define STATUSBAR_H

#include "aurora.h"

#define STATUS_BAR_HEIGHT 22

/* Reads the RTC and battery over I2C, so call it only when something changed.
 * Does not present. */
void status_bar_draw(void);

#endif
