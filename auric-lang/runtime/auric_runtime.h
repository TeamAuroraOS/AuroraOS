/* The only names generated C uses. Colours are packed 0xRRGGBB; button masks
 * mirror BUTTON_* in include/aurora.h. */
#ifndef AURIC_RUNTIME_H
#define AURIC_RUNTIME_H

/* screen(which): where the drawing built-ins draw from now on, AUR_TOP
 * (400x240, the default) or AUR_BOTTOM (320x240). */
void aur_screen(int which);
void aur_print(const char *text, int x, int y, int color);
/* clear(color): fill the selected screen and remember `color` as its text bg. */
void aur_clear(int color);
void aur_fill_rect(int x, int y, int w, int h, int color);
/* wait_key(button): block until `button` is newly pressed. */
void aur_wait_key(int button);
/* delay(cycles): busy-wait for roughly `cycles` iterations. */
void aur_delay(int cycles);
void aur_print_int(int value, int x, int y, int color);
/* keys_down(): buttons newly pressed since the last call (edge). */
int aur_keys_down(void);
/* keys_held(): buttons currently held down (level). */
int aur_keys_held(void);
/* buffered(on): when on, drawing accumulates off-screen until present(). */
void aur_buffered(int on);
/* present(): push each screen drawn on since the last present() to its panel. */
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
void aur_stop_music(void);
/* stop_sounds(): silence every effect voice; the music keeps playing. */
void aur_stop_sounds(void);

/* Touch on the bottom screen, in its pixels (x 0..319, y 0..239), calibrated
 * as in Settings > Touch Calibration. Needs the ARM11 core, so an app booted
 * without the Home Menu never sees a touch. A touch already on the screen when
 * the app first asks is ignored until the stylus lifts. */
/* touch_down(): a new touch began since the last call (edge). */
int aur_touch_down(void);
/* touch_held(): the screen is being touched (level). */
int aur_touch_held(void);
/* touch_up(): the stylus lifted since the last call (edge). */
int aur_touch_up(void);
/* touch_x(), touch_y(): where the stylus is, or where it last was; -1 before
 * the first touch. */
int aur_touch_x(void);
int aur_touch_y(void);
/* touch_in(x, y, w, h): that position lies inside the rectangle. */
int aur_touch_in(int x, int y, int w, int h);

/* Poll the HOME button; if pressed (and launched from the Home Menu), returns
 * control to AuroraOS and never comes back. Called from every built-in. */
void aur_check_home(void);

/* `==` and `!=` on strings: the same text. */
int aur_str_eq(const char *a, const char *b);

/* The network (src/os/AppNet.c): the Wi-Fi network saved in Settings > Wi-Fi,
 * plain HTTP. Each call waits until it is done; the screens keep their last
 * frame meanwhile and HOME leaves once it is over. A string these return
 * stays valid until the next request (http_get, http_post, http_download). */
/* net_connect(): joins the saved network unless already on one. */
int aur_net_connect(void);
/* net_online(): the Wi-Fi link is up. */
int aur_net_online(void);
/* net_error(): why the last network call failed, "" if it did not. */
const char *aur_net_error(void);
/* net_address(): the console's address, "" when not connected. */
const char *aur_net_address(void);
/* http_get(url), http_post(url, body): the reply's status (200, 404, ...),
 * or 0 when none came. */
int aur_http_get(const char *url);
int aur_http_post(const char *url, const char *body);
/* For the next request: a query parameter (URL-encoded here), or a header. A
 * post with an empty body sends the parameters as its body, a form. */
void aur_http_param(const char *name, const char *value);
void aur_http_param_int(const char *name, int value);
void aur_http_header(const char *name, const char *value);
/* The last reply's body as text, its length in bytes, its lines. */
const char *aur_http_text(void);
int aur_http_length(void);
int aur_http_lines(void);
const char *aur_http_line(int n);
/* http_save(path): the last reply's body into a file. */
int aur_http_save(const char *path);
/* http_download(url, path): a file of any size, in pieces. */
int aur_http_download(const char *url, const char *path);
/* The last reply as JSON, by a path of member names and array indices
 * separated by dots ("items.0.name"; "" is the whole document). */
int aur_json_has(const char *path);
int aur_json_int(const char *path);
float aur_json_float(const char *path);
int aur_json_bool(const char *path);
const char *aur_json_string(const char *path);
int aur_json_count(const char *path);
/* json_index(n): what a "#" in the paths that follow stands for. */
void aur_json_index(int n);

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

#define AUR_TOP    0
#define AUR_BOTTOM 1

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
#define AUR_KEY_X      (1 << 10)
#define AUR_KEY_Y      (1 << 11)

#endif
