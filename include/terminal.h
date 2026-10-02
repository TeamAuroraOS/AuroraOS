#ifndef AURORA_TERMINAL_H
#define AURORA_TERMINAL_H

/* `launch` runs an app container and does not return on success; 0 disables
 * apps. Returns on START or `exit`, leaving both screens for the caller to
 * redraw. Scrollback, history and the working folder persist until the next
 * app. */
void terminal_screen(const char *user, void (*launch)(const char *path));

#endif
