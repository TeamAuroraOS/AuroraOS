#ifndef AURORA_TERMINAL_H
#define AURORA_TERMINAL_H

/* A Linux-style shell: output on the top screen, a touch keyboard on the
 * bottom one. `user` is the name in the prompt. `launch` runs an Aurora app
 * container and does not return when it succeeds; 0 leaves apps unrunnable.
 * Returns on START or `exit`, leaving both screens for the caller to redraw.
 * The scrollback, history and working folder last until the next app. */
void terminal_screen(const char *user, void (*launch)(const char *path));

#endif
