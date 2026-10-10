/* aurora_app.h - the AuroraOS native app SDK.
 *
 * Everything an app written in C needs from AuroraOS: both screens and
 * drawing, buttons, touch and the circle pad, sound, files, time, system
 * information and the network (HTTP). The C standard library (devkitARM's newlib) works alongside
 * it: printf() and friends write to an on-screen console, malloc() has a
 * 16 MB heap, fopen() and the rest of stdio reach the SD card, and time()
 * reads the console's clock.
 *
 * A minimal app:
 *
 *   #include <aurora_app.h>
 *
 *   int main(void) {
 *     while (app_loop()) {
 *       gfx_clear(SCREEN_TOP, COLOR_BLACK);
 *       gfx_text(SCREEN_TOP, 8, 8, COLOR_WHITE, "Hello from C!");
 *       if (hid_keys_down() & KEY_START)
 *         break;
 *     }
 *     return 0;
 *   }
 *
 * Returning from main(), calling exit() or pressing HOME goes back to the
 * Home Menu. Build with sdk/aurcc.py; see sdk/README.md. */
#ifndef AURORA_APP_H
#define AURORA_APP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AURORA_SDK_VERSION "1.1"

#define AURORA_PRINTF(fmt, args) __attribute__((format(printf, fmt, args)))

#ifndef AURORA_H
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;
#endif

/* One pass of a frame loop: shows what was drawn since the last call, waits
 * for the next frame (60 per second), then reads the buttons, touch and
 * circle pad. HOME goes back to the Home Menu from here. Returns false once
 * app_quit() has been called.
 *
 *   while (app_loop()) { read input, update, draw }                       */
bool app_loop(void);

/* Makes the next app_loop() return false. */
void app_quit(void);

/* Frames shown per second, measured over the last second of app_loop(). */
int app_fps(void);

/* True when the app was started from the Home Menu, the Files app or the
 * Terminal. Only then are the GPU, sound, touch and the circle pad there
 * (the ARM11 core that runs them belongs to the OS), and only then does
 * HOME (or leaving the app) return to the Home Menu. An app booted directly
 * by the launcher as AURORAOS.BIN draws with the CPU and stays silent. */
bool app_from_home(void);

/* The app's file, e.g. "/Aurora/Apps/C/Snake.bin", and the folder for its own
 * data, named after it: "/Aurora/Apps/C/Snake", wherever the file is. Aurora
 * makes the folder when it starts the app, so files can be saved there at
 * once (an app built with --magic AUR1 or AOS1 makes it with fs_mkdir()). */
const char *app_path(void);
const char *app_dir(void);

/* Leaving: return from main() or call exit(). Functions given to atexit()
 * run first, also when the user presses HOME, so that is the place to save.
 * Open stdio files are flushed and closed either way. */

typedef enum {
  SCREEN_TOP = 0,    /* 400 x 240 */
  SCREEN_BOTTOM = 1, /* 320 x 240, the touchscreen */
} Screen;

#define SCREEN_TOP_WIDTH    400
#define SCREEN_BOTTOM_WIDTH 320
#define SCREEN_HEIGHT       240

/* Colours are 0xRRGGBB. */
#define RGB(r, g, b) \
  ((uint32_t)((((r) & 0xFF) << 16) | (((g) & 0xFF) << 8) | ((b) & 0xFF)))
/* Image pixels carry alpha in the top byte: 0 clear, 255 solid. */
#define RGBA(r, g, b, a) (RGB(r, g, b) | ((uint32_t)((a) & 0xFF) << 24))

#ifndef AURORA_APP_NO_COLORS
#define COLOR_BLACK      0x000000u
#define COLOR_WHITE      0xFFFFFFu
#define COLOR_GRAY       0xA0A0A0u
#define COLOR_DARK_GRAY  0x505050u
#define COLOR_RED        0xFF0000u
#define COLOR_GREEN      0x00FF00u
#define COLOR_BLUE       0x0000FFu
#define COLOR_CYAN       0x00FFFFu
#define COLOR_MAGENTA    0xFF00FFu
#define COLOR_YELLOW     0xFFFF00u
#define COLOR_ORANGE     0xFFA000u
#define COLOR_AURORA     0x64E8C8u /* the OS's default accent */
#endif

/* Drawing goes to an off-screen copy of each screen and shows at the next
 * app_loop() (or gfx_present()), so a frame never appears half drawn. Every
 * function clips to the screen. Both screens start black. */

int gfx_width(Screen s); /* 400 or 320 */

void gfx_clear(Screen s, uint32_t color);
void gfx_pixel(Screen s, int x, int y, uint32_t color);
uint32_t gfx_get_pixel(Screen s, int x, int y);
void gfx_rect(Screen s, int x, int y, int w, int h, uint32_t color);
/* alpha 0..255: how much of `color` covers what is there. */
void gfx_rect_blend(Screen s, int x, int y, int w, int h, uint32_t color,
                    int alpha);
/* A frame `t` pixels thick, inside the rectangle. */
void gfx_rect_outline(Screen s, int x, int y, int w, int h, int t,
                      uint32_t color);
/* Corners of radius r, smoothed. */
void gfx_round_rect(Screen s, int x, int y, int w, int h, int r,
                    uint32_t color);
/* From `top` at the first row to `bottom` at the last. */
void gfx_gradient(Screen s, int x, int y, int w, int h, uint32_t top,
                  uint32_t bottom);
void gfx_line(Screen s, int x0, int y0, int x1, int y1, uint32_t color);
void gfx_circle(Screen s, int cx, int cy, int r, uint32_t color);
void gfx_circle_outline(Screen s, int cx, int cy, int r, uint32_t color);
void gfx_triangle(Screen s, int x0, int y0, int x1, int y1, int x2, int y2,
                  uint32_t color);

/* Text. FONT_MONO is the built-in 8x8 font and is always there; the others
 * are the OS's Figtree from SD:/Aurora/assets.pak (Latin-1, UTF-8 strings)
 * and fall back to FONT_MONO without it. Text is printf-formatted, has a
 * clear background, and "\n" starts a new line under the first. The
 * functions return the width drawn, in pixels. */
typedef enum {
  FONT_MONO = 0,
  FONT_SMALL,   /* about 11 px high */
  FONT_REGULAR, /* about 14 px */
  FONT_BOLD,    /* about 14 px, bold */
  FONT_TITLE,   /* about 20 px, bold */
} GfxFont;

/* (x, y) is the top left of the text in FONT_MONO. */
int gfx_text(Screen s, int x, int y, uint32_t color, const char *fmt, ...)
    AURORA_PRINTF(5, 6);
/* FONT_MONO with every pixel `scale` x `scale`. */
int gfx_text_scaled(Screen s, int x, int y, int scale, uint32_t color,
                    const char *fmt, ...) AURORA_PRINTF(6, 7);
/* Any font; (x, y) is the top left of the first line. */
int gfx_print(Screen s, int x, int y, GfxFont font, uint32_t color,
              const char *fmt, ...) AURORA_PRINTF(6, 7);
/* The widest line of `text`, and the distance between lines. */
int gfx_text_width(GfxFont font, const char *text);
int gfx_line_height(GfxFont font);
/* False when the Figtree fonts could not be loaded. */
bool gfx_fonts_loaded(void);

/* Images: pixels row by row, each 0xAARRGGBB. Load PNG (with alpha), JPEG
 * (baseline) and BMP files, or make an empty one and fill `pixels`. */
typedef struct {
  int w, h;
  uint32_t *pixels;
} GfxImage;

/* NULL on failure; gfx_image_error() says why. Free with gfx_image_free(). */
GfxImage *gfx_image_load(const char *path);
/* Every pixel clear (0). */
GfxImage *gfx_image_new(int w, int h);
void gfx_image_free(GfxImage *img);
const char *gfx_image_error(void);

/* Draws with alpha blending, at (x, y) for the top left. */
void gfx_image(Screen s, const GfxImage *img, int x, int y);
/* The (sx, sy, sw, sh) part of the image only, e.g. a sprite from a sheet. */
void gfx_image_part(Screen s, const GfxImage *img, int sx, int sy, int sw,
                    int sh, int x, int y);
/* Stretched or shrunk to w x h, nearest pixel. */
void gfx_image_scaled(Screen s, const GfxImage *img, int x, int y, int w,
                      int h);

/* Shows both screens now; app_loop() does this itself. */
void gfx_present(void);

/* For drawing your own way: the off-screen copy of a screen, 3 bytes per
 * pixel in B, G, R order, stored by column and turned: pixel (x, y) is at
 * ((x * 240) + (239 - y)) * 3. Call gfx_mark(s) after writing to it. */
uint8_t *gfx_framebuffer(Screen s);
void gfx_mark(Screen s);

/* The console starts on the top screen at the first output. It draws into the
 * screen like the gfx_ functions (so gfx_clear() erases it) and shows at the
 * next app_loop(), hid_wait() or gfx_present(). It wraps long lines and
 * scrolls; "\f" clears it. stdin reads nothing. */

/* Puts the console on a screen, cleared, cursor top left. */
void con_init(Screen s);
void con_clear(void);
/* Colours for the text written from now on; the background also fills the
 * screen on con_clear() and the line a scroll opens. */
void con_color(uint32_t fg, uint32_t bg);
/* Moves the cursor to a column and row (from 0). */
void con_move(int col, int row);
int con_cols(void); /* 50 on the top screen, 40 on the bottom */
int con_rows(void); /* 30 */

#define KEY_A      (1u << 0)
#define KEY_B      (1u << 1)
#define KEY_SELECT (1u << 2)
#define KEY_START  (1u << 3)
#define KEY_RIGHT  (1u << 4)
#define KEY_LEFT   (1u << 5)
#define KEY_UP     (1u << 6)
#define KEY_DOWN   (1u << 7)
#define KEY_R      (1u << 8)
#define KEY_L      (1u << 9)
#define KEY_X      (1u << 10)
#define KEY_Y      (1u << 11)
/* Touching the bottom screen. */
#define KEY_TOUCH  (1u << 20)
/* The circle pad pushed most of the way in a direction. */
#define KEY_CPAD_RIGHT (1u << 28)
#define KEY_CPAD_LEFT  (1u << 29)
#define KEY_CPAD_UP    (1u << 30)
#define KEY_CPAD_DOWN  (1u << 31)
#define KEY_DPAD (KEY_RIGHT | KEY_LEFT | KEY_UP | KEY_DOWN)
#define KEY_CPAD (KEY_CPAD_RIGHT | KEY_CPAD_LEFT | KEY_CPAD_UP | KEY_CPAD_DOWN)

/* Reads every input once. app_loop() calls it; a loop of your own should
 * call it once per pass, which also keeps HOME working. */
void hid_scan(void);

/* As of the last scan: pressed since the scan before, held down, and let go
 * since the scan before. */
uint32_t hid_keys_down(void);
uint32_t hid_keys_held(void);
uint32_t hid_keys_up(void);

/* Touch, in bottom-screen pixels (x 0..319, y 0..239), calibrated as in
 * Settings > Touch Calibration. A touch already on the screen when the app
 * starts is ignored until it lifts. */
bool hid_touch_held(void);
bool hid_touch_down(void);
bool hid_touch_up(void);
/* Where the stylus is, or where it last was; -1 before the first touch. */
int hid_touch_x(void);
int hid_touch_y(void);
bool hid_touch_in(int x, int y, int w, int h);

/* The circle pad, -127..127 on each axis, +x right and +y up, 0 inside a
 * small dead zone. */
void hid_cpad(int *x, int *y);

/* Shows the screens, then waits until one of `keys` is pressed (or touched,
 * with KEY_TOUCH) and returns which. HOME still works while it waits. */
uint32_t hid_wait(uint32_t keys);

/* WAV files, 8 or 16-bit PCM, any rate (above 32 kHz they load at half
 * rate), mono or stereo (mixed to mono). Sounds stay loaded until
 * snd_unload_all() and share a 10 MB pool. Sound needs app_from_home(). */

/* A handle for the play functions, or -1 when the file cannot be read, is
 * not PCM, does not fit, or nothing can play it. */
int snd_load(const char *path);
/* Once, on whichever of the seven effect voices is free. volume 0..100. */
void snd_play(int sound);
void snd_play_volume(int sound, int volume);
/* Looped on the music voice, replacing what was playing there. */
void snd_music(int sound);
void snd_music_volume(int sound, int volume);
void snd_stop_music(void);
/* Silences the effect voices; the music keeps playing. */
void snd_stop_sounds(void);
/* Stops everything and frees the pool; earlier handles become invalid. */
void snd_unload_all(void);
bool snd_available(void);

/* Paths are on the SD card: "/Aurora/x.txt", "Aurora/x.txt", "sdmc:/..." and
 * "0:/..." all mean the same file, and "." and ".." work. The card is mounted
 * at the first use. fopen(), remove(), rename(), stat(), mkdir() and rmdir()
 * from the C library use these too. */

#define FS_NAME_MAX 256

typedef struct {
  char name[FS_NAME_MAX]; /* UTF-8 */
  uint32_t size;          /* bytes; 0 for a folder */
  bool is_dir;
} FsEntry;

typedef struct FsDir FsDir;

/* The entries of a folder, unsorted, without "." and "..". */
FsDir *fs_dir_open(const char *path);
bool fs_dir_read(FsDir *dir, FsEntry *entry); /* false at the end */
void fs_dir_close(FsDir *dir);

/* A whole file in memory from malloc(), with a 0 byte after the end so text
 * can be used as a string. NULL when it cannot be read. */
void *fs_read_file(const char *path, size_t *size);
/* Creates or replaces the file. */
bool fs_write_file(const char *path, const void *data, size_t size);
bool fs_exists(const char *path);
bool fs_is_dir(const char *path);
/* Creates the folder and any missing parents; true if it exists after. */
bool fs_mkdir(const char *path);
/* True when the SD card can be read. */
bool fs_available(void);

/* Since the app started. */
uint32_t sys_millis(void);
uint64_t sys_micros(void);
/* Waits, keeping HOME working; it does not show the screens. */
void sys_sleep(uint32_t ms);

typedef struct {
  int year;    /* e.g. 2026 */
  int month;   /* 1..12 */
  int day;     /* 1..31 */
  int hour;    /* 0..23 */
  int minute;
  int second;
  int weekday; /* 0 Sunday .. 6 Saturday */
} SysTime;

/* The clock as the Home Menu shows it (Settings > Clock included). False if
 * the clock could not be read. time() and localtime() give the same. */
bool sys_time(SysTime *t);

/* Battery charge 0..100, or -1 when unknown. */
int sys_battery(void);
/* The same in tenths of a percent (0..1000), or -1. */
int sys_battery_tenths(void);
bool sys_charging(void);
/* The battery's temperature in degrees C: the only temperature the 3DS
 * reports (it has no CPU sensor software can read). SYS_TEMP_UNKNOWN when
 * the power chip does not answer. */
#define SYS_TEMP_UNKNOWN (-1000)
int sys_battery_temperature(void);
/* The system voltage in millivolts (measured on the load side, so a little
 * under the battery's own), or -1. */
int sys_battery_millivolts(void);
/* New 3DS, New 3DS XL or New 2DS XL. Needs app_from_home(). */
bool sys_is_new3ds(void);

typedef enum {
  SYS_LANG_ENGLISH = 0,
  SYS_LANG_SPANISH = 1,
  SYS_LANG_FRENCH = 2,
} SysLanguage;

/* From the user's settings (the setup wizard and Settings). */
const char *sys_user_name(void); /* "" when not set */
SysLanguage sys_language(void);
uint32_t sys_accent(void); /* the accent colour, 0xRRGGBB */

void sys_power_off(void) __attribute__((noreturn));
void sys_reboot(void) __attribute__((noreturn));

/* The network, over the Wi-Fi network saved in Settings > Wi-Fi. Plain HTTP
 * only: AuroraOS has no TLS yet, so https:// addresses fail. Every call waits
 * until it is done. The first net_connect() takes about 25 seconds, a request
 * usually well under one (at most 15). Meanwhile the screens keep their last
 * frame and the core can play no new sound or read touch and the circle pad,
 * unless a net_on_wait() handler draws something. HOME leaves once the call is
 * over. Needs app_from_home() and an AuroraOS as new as this SDK (ARM11
 * core 115 or later).
 *
 *   HttpResponse *r = net_connect() ? http_get("http://example.com/") : NULL;
 *   if (r) {
 *     printf("%d, %u bytes\n", r->status, (unsigned)r->length);
 *     http_free(r);
 *   } else {
 *     printf("%s\n", net_error());
 *   }                                                                    */

/* Whether this app can use the network at all; net_error() says why not. */
bool net_available(void);
/* Joins the saved network, unless the console is on it already (the Home
 * Menu, the Terminal or another app may have joined it). */
bool net_connect(void);
/* Asks the Wi-Fi chip whether the link is still up. */
bool net_connected(void);
/* Why the last call failed, in a short line: "no network saved: pick one in
 * Settings > Wi-Fi", "example.com did not answer in time", ... */
const char *net_error(void);
/* The console's address, "192.168.1.20", or "" when not connected. */
const char *net_address(void);
/* A host name to its address in text ("93.184.215.14"); `ip` holds 16. */
bool net_resolve(const char *host, char ip[16]);
/* One ping: the round trip in milliseconds, or -1. */
int net_ping(const char *host);
/* Called about ten times a second while a call waits, with the milliseconds
 * waited so far. It may draw and gfx_present() (the CPU copies the frames
 * then) and call hid_scan() for the buttons, but not make network calls or
 * play sounds. NULL: none. */
void net_on_wait(void (*fn)(uint32_t ms));

/* Plain TCP, once: connects to host:port, sends `length` bytes (2 KB at
 * most) and reads what comes back until the server closes the connection.
 * Copies up to `max` bytes to `reply` and returns how many came (at most
 * 1020 KB), or -1. */
int net_tcp_exchange(const char *host, int port, const void *data,
                     size_t length, void *reply, size_t max);

typedef struct {
  int status;     /* 200, 404, ...: whatever the server answered */
  char *body;     /* `length` bytes, with a 0 byte after them */
  size_t length;
  char *headers;  /* the status line and headers, "\r\n" between lines */
} HttpResponse;

/* One HTTP request to an http:// address (a port and a query may follow the
 * host). Redirects are followed, up to five; one to https:// comes back as it
 * is, with net_error() saying so. NULL when no reply came. The request, with
 * its headers and body, must fit in 2 KB; the reply in 1020 KB (use
 * http_download() for bigger files). Free the result with http_free(). */
HttpResponse *http_get(const char *url);
/* `content_type` NULL is a form, "application/x-www-form-urlencoded". */
HttpResponse *http_post(const char *url, const char *content_type,
                        const void *data, size_t length);
/* Any method. `headers` (may be NULL) are extra lines such as
 * "Authorization: Bearer x\r\n"; Content-Length is added for a body. */
HttpResponse *http_request(const char *method, const char *url,
                           const char *headers, const void *data,
                           size_t length);
/* A header's value (name without the colon, any case) copied into `value`. */
bool http_header(const HttpResponse *r, const char *name, char *value,
                 size_t max);
void http_free(HttpResponse *r);

/* Saves what `url` gives to a file, of any size: in pieces of up to 512 KB
 * when the server takes Range requests, as most do (otherwise the file must
 * fit in 1020 KB). It goes to `path`.part first and replaces `path` at the
 * end. `progress` (may be NULL) is called after each piece, with total 0 while
 * the size is unknown; it may draw and present. */
bool http_download(const char *url, const char *path,
                   void (*progress)(size_t done, size_t total));

/* `text` percent-encoded for a query or a form ("a b&c" -> "a%20b%26c");
 * returns the length written, cut to fit `max` with its 0 byte. */
size_t http_url_encode(char *out, size_t max, const char *text);

#ifdef __cplusplus
}
#endif

#endif
