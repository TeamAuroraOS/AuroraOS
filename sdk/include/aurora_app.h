/* aurora_app.h - the AuroraOS native app SDK.
 *
 * Everything an app written in C needs from AuroraOS: both screens and
 * drawing, buttons, touch and the circle pad, sound, files, time, system
 * information and the network (HTTP and HTTPS). The C standard library
 * (devkitARM's newlib) works alongside it: printf() and friends write to an
 * on-screen console, malloc() has a 16 MB heap, fopen() and the rest of stdio
 * reach the SD card, and time() reads the console's clock.
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
 * Returning from main(), calling exit() or pressing HOME goes back to the Home
 * Menu. L + Down + Select opens the system menu in an app. Build with
 * sdk/aurcc.py; see sdk/README.md. */
#ifndef AURORA_APP_H
#define AURORA_APP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AURORA_SDK_VERSION "1.6"

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
 * circle pad. HOME goes back to the Home Menu; L + Down + Select opens the
 * system menu. Returns false once app_quit() has been called.
 *
 *   while (app_loop()) { read input, update, draw } */
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

/* The entries of a folder, unsorted, without "." and "..". From SDK 1.6
 * <dirent.h> (opendir(), readdir(), scandir(), ...) reads folders too. */
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

/* Since the app started, not counting time with the lid closed. */
uint32_t sys_millis(void);
uint64_t sys_micros(void);
/* Waits, keeping HOME working; it does not show the screens. With the lid
 * closed it waits for it to open, and the time closed does not count. */
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
  SYS_LANG_PORTUGUESE = 3,
} SysLanguage;

/* From the user's settings (the setup wizard and Settings). */
const char *sys_user_name(void); /* "" when not set */
SysLanguage sys_language(void);
uint32_t sys_accent(void); /* the accent colour, 0xRRGGBB */

void sys_power_off(void) __attribute__((noreturn));
void sys_reboot(void) __attribute__((noreturn));

/* The network, over the Wi-Fi network saved in Settings > Wi-Fi: HTTP, and
 * HTTPS (TLS 1.2, the server's certificate checked against the roots the OS
 * trusts) with an AuroraOS from Beta 7 on (ARM11 core 121). Every call waits
 * until it is done. The first net_connect() takes about 25 seconds, a request
 * usually well under one (an https:// one a second or two more for its
 * handshake; at most 15 per wait). Meanwhile the screens keep their last
 * frame and the core can play no new sound or read touch and the circle pad,
 * unless a net_on_wait() handler draws something. HOME leaves once the call is
 * over. Needs app_from_home() and ARM11 core 115 or later (121 for
 * https://).
 *
 *   HttpResponse *r = net_connect() ? http_get("http://example.com/") : NULL;
 *   if (r) {
 *     printf("%d, %u bytes\n", r->status, (unsigned)r->length);
 *     http_free(r);
 *   } else {
 *     printf("%s\n", net_error());
 *   } */

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
 * 1020 KB), or -1. For more than one exchange, BSD sockets
 * (<sys/socket.h>, <netdb.h>, from SDK 1.6) keep a TCP connection open, and
 * from ARM11 core 126 send and receive UDP; sdk/README.md "Sockets". */
int net_tcp_exchange(const char *host, int port, const void *data,
                     size_t length, void *reply, size_t max);

typedef struct {
  int status;     /* 200, 404, ...: whatever the server answered */
  char *body;     /* `length` bytes, with a 0 byte after them */
  size_t length;
  char *headers;  /* the status line and headers, "\r\n" between lines */
} HttpResponse;

/* One HTTP request to an http:// or https:// address (a port and a query
 * may follow the host). Redirects are followed, up to five, to either kind;
 * Authorization and Cookie lines go only to the first URL's host, port and
 * scheme.
 * NULL when no reply came; for https:// that includes a certificate that does
 * not check out, with net_error() saying why. The request, with
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

/* Fetching a large reply a piece at a time while the app goes on drawing and
 * playing: GETs over one connection the ARM11 core keeps open in the background.
 * http_stream_poll() returns one of these; nothing else here waits. */
enum {
  HTTP_STREAM_FAILED = -1, /* net_error() says why */
  HTTP_STREAM_IDLE = 0,
  HTTP_STREAM_WAITING,
  HTTP_STREAM_BODY,
  HTTP_STREAM_DONE,
};
/* `headers` (may be NULL): extra lines, each ended with CR LF, at most 511 bytes.
 * Redirects are followed, with http_request()'s rule for Authorization and Cookie.
 * A reply with no length ends when the server closes, and a chunked one fails. Keep
 * reading: 15 s with nothing read counts as the server having stopped. */
bool http_stream_get(const char *url, const char *headers);
int http_stream_poll(void);
size_t http_stream_read(void *buf, size_t max);
int http_stream_status(void);
/* 0 until known, or when the reply has no length. */
size_t http_stream_length(void);
void http_stream_close(void);

/* JSON, the same tokenizer the OS uses: a flat list of tokens in document order.
 * An object's members are its key tokens, each followed by its value's tokens.
 * Token indices are ints; -1 means "not there". */
#ifndef AURORA_JSON_H
#define AURORA_JSON_H
enum {
  JSON_OBJ = 1,
  JSON_ARR,
  JSON_STR,
  JSON_NUM,
  JSON_TRUE,
  JSON_FALSE,
  JSON_NULL,
};
typedef struct {
  uint8_t type;
  uint32_t start, end; /* the text, quotes excluded for a string */
  uint32_t next;       /* the token after this one's whole subtree */
  uint32_t count;      /* members of an object, elements of an array */
} JsonTok;
typedef struct {
  const char *src;
  JsonTok *tok;
  uint32_t n, cap;
} Json;
/* 1 on success. `tok` holds up to `cap` tokens; the document must fit. */
int json_parse(Json *j, const char *src, uint32_t len, JsonTok *tok, uint32_t cap);
int json_get(const Json *j, int obj, const char *key);
int json_at(const Json *j, int arr, uint32_t i);
uint32_t json_count(const Json *j, int t);
int json_int(const Json *j, int t, int def);
double json_num(const Json *j, int t, double def);
int json_bool(const Json *j, int t, int def);
int json_is(const Json *j, int t, const char *s);
/* The string, escapes resolved and anything outside ASCII as '?'. */
void json_str(const Json *j, int t, char *out, uint32_t max);
/* The same for text to show: line breaks kept, and letters up to U+00FF as UTF-8. */
void json_text(const Json *j, int t, char *out, uint32_t max);
#endif

/* Video, for apps built with `aurcc build --video`, which adds about 2 MB. The
 * ARM11's other cores decode it: H.264 (8-bit 4:2:0, Main or High profile) and AAC,
 * from fragmented MP4 with video and sound in separate files, the form DASH uses.
 * A URL's server must answer Range requests with 206.
 * While it plays the top screen is the video's: do not draw there. The bottom
 * screen is free, but present it only a few times a second. */
typedef struct {
  const char *url;
  const char *headers;  /* extra request lines, each ended with CR LF; may be NULL */
  uint64_t size;        /* the whole stream's bytes */
  uint32_t index_end;   /* where the moov and sidx end, inclusive */
} VideoTrackUrl;

typedef struct {
  int state;            /* 0 no engine; see video_engine_start */
  bool playing, buffering, ended;
  uint32_t width, height;
  int32_t clock_ms;     /* the position on the speakers */
  int32_t buffered_ms;  /* fetched ahead of that */
  uint32_t decoded, shown, dropped, underruns;
  uint32_t fps_x10;     /* frames shown a second since it started, times ten */
  uint32_t decode_us, convert_us; /* time the first core spent on each, in total */
  uint32_t heartbeat;   /* moves while the engine runs */
  int cores;            /* cores decoding */
} VideoStats;

/* The tallest video worth asking this console for: 240 on an Old 3DS, or on a New
 * one with Settings > Full power off; else 480. */
int video_height_limit(void);
/* Starts the engine: about a second. false with video_error() saying why. */
bool video_engine_start(void);
void video_engine_stop(void);
bool video_engine_running(void);
const char *video_error(void);
int video_cores(void);
/* Opens a stream, the engine already running; it starts by itself once enough is
 * buffered. URLs need the console on a network, and block for their headers. */
bool video_open_files(const char *video_path, const char *audio_path);
bool video_open_urls(const VideoTrackUrl *video, const VideoTrackUrl *audio);
void video_close(void);
/* Fetches and feeds: call it every pass of the app's loop. false at the end of the
 * video or on a failure (video_error() is empty at the end). */
bool video_pump(void);
void video_pause(bool on);
/* To the nearest segment start at or before `ms`. */
bool video_seek(int32_t ms);
void video_stats(VideoStats *out);

#ifdef __cplusplus
}
#endif

#endif
