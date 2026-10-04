#ifndef STATUSBAR_H
#define STATUSBAR_H

#include "aurora.h"

#define STATUS_BAR_HEIGHT 22
#define STATUS_APP_HEIGHT 36

/* Reads the RTC and battery over I2C, so call it only when something changed.
 * Does not present. */
void status_bar_draw(void);

/* The taller bar a full-screen app draws in its place: `title` after the
 * clock, on `bg`. With `icon` (a 16-pixel asset) the Home Menu's grid and that
 * icon follow the battery, the icon underlined as the app in use; UI_NO_ASSET
 * leaves them out. Does not present. */
void status_bar_draw_app(const char *title, Color bg, u32 icon);

#endif
