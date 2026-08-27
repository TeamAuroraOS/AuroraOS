/* Auric runtime shim -- public surface for generated programs.
 *
 * Generated C (`#include "auric_runtime.h"`) only ever touches the names here:
 * the four built-in helpers and the predefined colour/button constants. The
 * implementation (auric_runtime.c) maps them onto AuroraOS's own screen/input
 * API (draw_string, clear_screen, get_keys_down, delay).
 *
 * Colours are packed 0xRRGGBB and unpacked into AuroraOS `Color`s by the shim.
 * Button masks mirror the BUTTON_* bits in ../../include/aurora.h.
 */
#ifndef AURIC_RUNTIME_H
#define AURIC_RUNTIME_H

/* ---- built-ins ---------------------------------------------------------- */
/* print(text, x, y, color): draw text on the top screen at (x, y). */
void aur_print(const char *text, int x, int y, int color);
/* clear(color): fill the top screen and remember `color` as the text bg. */
void aur_clear(int color);
/* fill_rect(x, y, w, h, color): fill a rectangle on the top screen. */
void aur_fill_rect(int x, int y, int w, int h, int color);
/* wait_key(button): block until `button` is newly pressed. */
void aur_wait_key(int button);
/* delay(cycles): busy-wait for roughly `cycles` iterations. */
void aur_delay(int cycles);

/* Poll the HOME button; if pressed (and launched from the Home Menu), returns
 * control to AuroraOS and never comes back. Called from every built-in. */
void aur_check_home(void);

/* ---- predefined constants (colours: packed 0xRRGGBB) -------------------- */
#define AUR_BLACK     0x000000
#define AUR_WHITE     0xFFFFFF
#define AUR_RED       0xFF0000
#define AUR_GREEN     0x00FF00
#define AUR_BLUE      0x0000FF
#define AUR_CYAN      0x00FFFF
#define AUR_MAGENTA   0xFF00FF
#define AUR_YELLOW    0xFFFF00
#define AUR_ORANGE    0xFFA000
#define AUR_AURORA    0x64E8C8
#define AUR_GRAY      0xA0A0A0
#define AUR_DARK_GRAY 0x505050

/* ---- predefined constants (buttons: HID bitmasks) ---------------------- */
#define AUR_KEY_A      (1 << 0)
#define AUR_KEY_B      (1 << 1)
#define AUR_KEY_SELECT (1 << 2)
#define AUR_KEY_START  (1 << 3)
#define AUR_KEY_RIGHT  (1 << 4)
#define AUR_KEY_LEFT   (1 << 5)
#define AUR_KEY_UP     (1 << 6)
#define AUR_KEY_DOWN   (1 << 7)
#define AUR_KEY_R      (1 << 8)
#define AUR_KEY_L      (1 << 9)

#endif /* AURIC_RUNTIME_H */
