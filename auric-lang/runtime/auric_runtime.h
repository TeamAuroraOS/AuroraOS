/* The only names generated C uses. Colours are packed 0xRRGGBB; button masks
 * mirror BUTTON_* in include/aurora.h. */
#ifndef AURIC_RUNTIME_H
#define AURIC_RUNTIME_H

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
/* print_int(value, x, y, color): draw a signed decimal number at (x, y). */
void aur_print_int(int value, int x, int y, int color);
/* keys_down(): buttons newly pressed since the last call (edge). */
int aur_keys_down(void);
/* keys_held(): buttons currently held down (level). */
int aur_keys_held(void);
/* buffered(on): when on, drawing accumulates off-screen until present(). */
void aur_buffered(int on);
/* present(): push the off-screen frame to the panel. */
void aur_present(void);
/* rand(n): pseudo-random integer in [0, n). */
int aur_rand(int n);
/* millis(): milliseconds since the app started, from an ARM9 hardware timer. */
int aur_millis(void);

/* load_sound(path): read a WAV file from the SD card, `path` counted from the
 * card's root. Returns a handle for play_sound()/play_music(), or -1 when the
 * file is missing, is not 8/16-bit PCM, does not fit in the 10 MB sound pool,
 * or nothing can play it (an app booted without the Home Menu). */
int aur_load_sound(const char *path);
/* play_sound(handle): play once, on whichever effect voice is free. */
void aur_play_sound(int handle);
/* play_music(handle): loop on the music voice, replacing what was there. */
void aur_play_music(int handle);
/* stop_music(): silence the music voice. */
void aur_stop_music(void);
/* stop_sounds(): silence every effect voice; the music keeps playing. */
void aur_stop_sounds(void);

/* Poll the HOME button; if pressed (and launched from the Home Menu), returns
 * control to AuroraOS and never comes back. Called from every built-in. */
void aur_check_home(void);

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

#endif
