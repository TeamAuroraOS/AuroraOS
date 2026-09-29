#ifndef AURORA_SCREENSHOT_H
#define AURORA_SCREENSHOT_H

/* Both screens as one 400x480 BMP in SD:/Aurora/Screenshots, the top screen
 * above the bottom one, named from the clock. get_keys_down() calls this when
 * L and R are pressed together, so it works on any screen that polls keys.
 * Blocks for about a second and leaves both panels as they were. */
void screenshot_take(void);

#endif
