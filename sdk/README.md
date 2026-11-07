# AuroraOS native apps in C

Apps for AuroraOS can be written in plain C as well as in
[Auric](../auric-lang/README.md). A C app uses one header,
[`include/aurora_app.h`](include/aurora_app.h), for the screens, input, sound,
files, system information and the network (HTTP), and the whole C standard
library besides:
`printf()` writes to an on-screen console, `malloc()` has a 16 MB heap,
`fopen()` reads and writes the SD card, `time()` reads the console's clock,
`<dirent.h>` lists folders, BSD sockets reach the network, and `<math.h>`,
`qsort()`, `strtod()` and the rest are there.

`aurcc.py` builds it into an app container like the ones Auric produces, with
the magic `AURC` in place of `AUR1`, so Aurora knows it is a C app: the Home
Menu shows "C SDK" under its name, and Aurora makes its data folder before
starting it. C apps go in `SD:/Aurora/Apps/C`, which the Home Menu lists along
with `SD:/Aurora/Apps`, and start from the Home Menu, Files or the Terminal like
any other app. HOME returns to the Home Menu.

To publish one on aShop, upload the `AURC` `.bin` at
account.aurora3ds.xyz/developer as for an Auric app. aShop and the store's
website show "C SDK" under its name, and the console installs it into
`SD:/Aurora/Apps/C`. Only consoles on Beta 6 v0.1.5 or later are offered it,
since older ones refuse `AURC` files.

```c
#include <aurora_app.h>
#include <stdio.h>

int main(void) {
  printf("Hello from C on AuroraOS!\n");
  printf("Press A to quit.\n");
  hid_wait(KEY_A);
  return 0;
}
```

## Contents

* [Quick start](#quick-start)
* [Writing an app](#writing-an-app)
* [The API at a glance](#the-api-at-a-glance)
* [The C library](#the-c-library)
* [Memory](#memory)
* [Building](#building)
* [Examples](#examples)
* [How it works](#how-it-works)
* [Limits](#limits)
* [Licences](#licences)

## Quick start

You need **devkitARM** (`arm-none-eabi-gcc` on `PATH`; get it from
<https://devkitpro.org/wiki/Getting_Started>) and **Python 3.8 or newer**. On
Windows use the `py` launcher or another Windows Python: devkitPro's MSYS
`python`, which is often first on `PATH`, cannot read `C:\...` paths.

```
py sdk/aurcc.py new MyGame        # a folder with main.c and MyGame.icon
py sdk/aurcc.py build MyGame -o MyGame.bin
```

Copy `MyGame.bin` to `SD:/Aurora/Apps/C/` (make the `C` folder the first
time), boot Aurora and pick **MyGame** on the Home Menu, or type `MyGame` in
the Terminal. The first build compiles the SDK runtime (about 30 files);
later builds only compile the app.

`py sdk/aurcc.py version` checks that the compiler is found.

## Writing an app

### The frame loop

Most apps are a loop around `app_loop()`:

```c
int main(void) {
  while (app_loop()) {           /* show the last frame, wait, read input */
    if (hid_keys_down() & KEY_START)
      break;                     /* leaving main() goes back to the Home Menu */
    gfx_clear(SCREEN_TOP, COLOR_BLACK);
    gfx_print(SCREEN_TOP, 10, 10, FONT_TITLE, COLOR_WHITE, "Score %d", score);
  }
  return 0;
}
```

Each pass of `app_loop()` shows what was drawn since the last one, waits for
the next frame (60 per second), reads the buttons, the touchscreen and the
circle pad, and checks HOME. Drawing goes to an off-screen copy of each
screen, so a frame never shows half drawn. `app_quit()` makes the next
`app_loop()` return false.

A loop of your own can call `hid_scan()` once per pass and `gfx_present()` to
show the screens; `hid_wait(keys)` shows the screens and waits for a button.

### Leaving

Returning from `main()`, calling `exit()` and pressing HOME all go back to the
Home Menu. Functions registered with `atexit()` run first in each case, and
open stdio files are flushed and closed, so `atexit()` is the place to save a
game:

```c
static void save(void) { fs_write_file(path, &state, sizeof(state)); }
...
atexit(save);
```

Pressing L + Down + Select opens Aurora's system menu in apps launched from
the Home Menu. The resident ARM11 system service owns the chord and draws the
overlay independently of the app's input and rendering loops. Updated C
runtimes hide pad input from the app while the menu is open. Return to Home is
delivered to this runtime at SDK boundaries: at start the runtime tells the
core it handles it (`app_caps`), and the core then passes it on, after which
the app leaves through `exit()` as HOME does. An app built with an older SDK
gets a "press HOME" notice instead, so rebuild existing `.bin` files with
`sdk/aurcc.py`. Power off and Cancel are handled by the resident service
itself.

HOME is noticed in `app_loop()`, `hid_scan()`, `hid_wait()` and `sys_sleep()`;
a long computation that calls none of them does not respond to it until it
does.

Closing the lid turns the screens off (the resident ARM11 core does that for
every app) and, in apps built with SDK 1.3 or later, pauses the app where it
next reads the buttons or waits: the `hid_` reads (so every `app_loop()`),
`sys_sleep()` and the C library's `sleep`, `usleep` and `nanosleep`. App
time stops meanwhile: `sys_millis()`, `sys_micros()`, `clock()`, the frame
pacing and `app_fps()` leave out the time the lid was closed, so a game
resumes where it was and a `sys_sleep(1000)` still sleeps one second of open
time; `time()` and `gettimeofday()` follow the real clock. Buttons still
held as the lid opens do not count as new presses. Sound already playing
keeps playing; stop it in your own code if that matters. The same blocking
rule as HOME applies: a loop that never reads the buttons or waits keeps
running in the dark. Apps built with SDK 1.2 or older keep running behind
the dark screens until they are rebuilt. See
[`../docs/power.md`](../docs/power.md), "Closing the lid".

### Your app's files

`app_dir()` is the folder for an app's own data: `SD:/Aurora/Apps/C/<name>`,
where `<name>` is the app's file name without `.bin`. `Bricks.bin` gets
`/Aurora/Apps/C/Bricks`, wherever `Bricks.bin` is on the card, so its data
stays put if the app is started from Files somewhere else. Aurora makes the
folder each time it starts an `AURC` app, so an app can save there from the
start and read the files back next time. Sounds, pictures and save files go
there. Renaming the `.bin` gives the app a new, empty folder.

```c
char path[300];
snprintf(path, sizeof(path), "%s/save.txt", app_dir());
FILE *f = fopen(path, "w");
```

An app built with `--magic AUR1` or `AOS1` gets no folder made for it; call
`fs_mkdir(app_dir())` before saving. `app_path()` is the app file itself.

Paths may be written `/Aurora/x.txt`, `Aurora/x.txt`, `sdmc:/Aurora/x.txt` or
`0:/Aurora/x.txt`; `.` and `..` work. The card is FAT, so names are
case-insensitive.

### The network

Apps reach the internet over the Wi-Fi network saved in Settings > Wi-Fi, by
HTTP or, from SDK 1.4, HTTPS: an `https://` address goes over TLS 1.2 (BearSSL
on the ARM9, [`../docs/tls.md`](../docs/tls.md)), with the server's
certificate checked against the roots the OS trusts, and redirects to either
kind are followed; your `Authorization` and `Cookie` headers are not sent on
once a redirect changes the host, the port or drops TLS. HTTPS needs an AuroraOS
from Beta 7 on (ARM11 core 121 or later); on
an older one an `https://` request fails and `net_error()` says so. A
certificate that does not check out (expired, for another name, from an
authority not trusted) fails the request, with the reason in `net_error()`.
Apps built with SDK 1.3 or older have no TLS: rebuild them.

```c
HttpResponse *r = net_connect() ? http_get("https://example.com/") : NULL;
if (r) {
  printf("%d, %u bytes\n%s\n", r->status, (unsigned)r->length, r->body);
  http_free(r);
} else {
  printf("%s\n", net_error());   /* "no network saved: pick one in ..." */
}
```

* `net_connect()` joins the saved network unless the console is on it already
  (aShop, the Terminal or another app may have joined it; a join stays up until
  the console is turned off or the network drops). The first join takes about
  25 seconds.
* `http_get()`, `http_post()` and `http_request()` (any method, extra header
  lines) return the response whatever its status, or NULL when none came, and
  follow redirects. The request, with its headers and body, must fit in 2 KB,
  the response in 1020 KB. `http_download()` saves a file of any size, asking
  for it in pieces of up to 512 KB (Range requests) sized to take about three
  seconds each, and reports progress after each.
* `net_resolve()`, `net_ping()` and `net_address()` look up names, ping a
  host and give the console's own address. `net_tcp_exchange()` is plain TCP,
  once: it sends up to 2 KB and reads until the server closes the connection.
* **Every call waits until it is done.** The ARM11 core that runs the Wi-Fi
  also presents frames, plays sounds and reads touch and the circle pad, so
  while a call runs the screens keep their last frame, no new sound starts
  (`snd_play()` does nothing then) and touch and the circle pad do not
  change. A request usually takes well under a second, at most 15 per wait;
  an `https://` one adds its handshake, a second or two.
* `net_on_wait(fn)` gives a function to call about ten times a second while a
  call waits: it can draw (a spinner, the seconds) and `gfx_present()`, which
  the CPU then copies to the screens. It must not make network calls itself.
* HOME pressed during a call leaves the app once the call is over.

The `net` example joins with a spinner on the bottom screen, fetches a page,
pings a host and downloads the page into its data folder.

### Sockets

From SDK 1.6 the usual BSD calls work too, for code that wants its own
protocol over TCP, or UDP, rather than HTTP: `socket()`, `connect()`,
`send()`, `recv()`, `sendto()`, `recvfrom()`, `close()`, `getaddrinfo()`,
`poll()`, `select()` and the rest of the subset in [The C library](#the-c-library),
from `<sys/socket.h>`, `<netinet/in.h>`, `<arpa/inet.h>`, `<netdb.h>` and
`<poll.h>`. A socket descriptor works with `read()`, `write()` and
`fdopen()` as well.

```c
struct addrinfo hints = {0}, *res;
hints.ai_socktype = SOCK_STREAM;
if (getaddrinfo("example.com", "80", &hints, &res) == 0) {
  int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
  if (connect(fd, res->ai_addr, res->ai_addrlen) == 0) {
    send(fd, request, strlen(request), 0);
    while ((n = recv(fd, buf, sizeof(buf), 0)) > 0)
      ...
  }
  close(fd);
  freeaddrinfo(res);
}
```

What the console's network allows, all of it run by the OS's ARM11 core:

* **TCP** (ARM11 core 124 or later): one connection at a time, for the
  whole app. `connect()` on a second socket fails with `EBUSY` until the first
  is closed; `close()` sends the FIN and returns at once, and the next
  `connect()` or network call waits for the core to finish it (4.5 s at
  most, then it resets the connection).
  The core keeps 16 KB of what you send and 32 KB of what comes in; a
  connection with nothing acknowledged for 20 s ends with `ETIMEDOUT`.
* **UDP** (ARM11 core 126 or later): one bound port at a time, so one UDP
  socket at a time. It is bound at its first `sendto()`, `recv()` or
  `connect()` if `bind()` did not, to a port from 49152 on. A datagram sent
  carries at most 524 bytes (`EMSGSIZE` above that); one received at most
  1472, and one that came in pieces (an IP fragment) is dropped. The core
  keeps 4 KB of datagrams to send and 8 KB received; what comes in while
  those 8 KB are full is dropped. Broadcasts to 255.255.255.255 need
  `SO_BROADCAST`. A datagram to a host on your own network whose address the
  core has not seen waits up to a second for its ARP answer, then is
  dropped. On an older core `socket(AF_INET, SOCK_DGRAM, 0)` fails with
  `EPROTONOSUPPORT`.
* **While a socket holds the TCP connection**, the other network calls
  (`http_get()` and the rest, `net_ping()`, a stream) fail, with
  "a socket holds the network connection: close it first" in `net_error()`,
  rather than take the connection away. Names already looked up still
  resolve; new ones, and UDP, work beside it from core 126. `net_connected()`
  says yes.
* **The first socket call joins** the saved network if the console is not on
  it yet, as `net_connect()` does (about 25 seconds), so ported code works
  unchanged; calling `net_connect()` first lets you show a spinner.
* **Blocking calls wait in the runtime**, which notices HOME, a closed lid and
  an ARM11 fault meanwhile and calls a `net_on_wait()` function ten times a
  second; unlike the `http_` calls, the screens, sound and touch keep working,
  since the core is free. `SO_RCVTIMEO` and `SO_SNDTIMEO` bound a wait;
  `O_NONBLOCK` (`fcntl()`, `SOCK_NONBLOCK`, `ioctl(FIONBIO)`), `MSG_DONTWAIT`,
  `poll()` and `select()` work as usual, a non-blocking `connect()` returning
  `EINPROGRESS` with `SO_ERROR` after. A non-blocking `connect()` still waits
  for its first packet to go out, which an ARP lookup can stretch to 3 s.
* **Keep it alive**: the core drops a socket the app has not touched for 15 s.
  `app_loop()`, `hid_scan()`, `sys_sleep()`, every socket call and a closed lid
  count as touching it, so this only bites a loop that does none of them.
  HOME and `exit()` close sockets (a FIN, then a reset after 1 s).
* **Not there**: `listen()`/`accept()` (`EOPNOTSUPP`; nothing can connect to
  the console), IPv6 (`EAFNOSUPPORT`), raw sockets, `sendmsg()`/`recvmsg()`,
  `SIGPIPE` (`send()` gives `EPIPE`), reverse lookups (`getnameinfo()` gives
  numbers), TLS on a socket (use `https://` with the `http_` calls),
  `getsockname()`'s port for TCP (0). `shutdown(SHUT_WR)` stops `send()` but
  the FIN waits for `close()`, since the core ends a connection soon after its
  FIN. TCP options are accepted and do nothing but `SO_LINGER` with 0 s, which
  makes `close()` reset the connection; `TCP_NODELAY` is how it works anyway.

The `sockets` example fetches a page over TCP, asks an NTP server for the time
over UDP and lists its data folder with `scandir()`.

### When the app was not started from the Home Menu

The GPU, sound, touch and the circle pad are run by the OS's ARM11 core, which
only an app the Home Menu (or Files or the Terminal) started has behind it.
`app_from_home()` says which. An app booted directly by the launcher as
`AURORAOS.BIN` (build with `--magic AOS1`) still draws, reads buttons and uses
the card, but draws with the CPU, stays silent, has no network and ignores
HOME.

## The API at a glance

Everything is declared, with comments, in
[`include/aurora_app.h`](include/aurora_app.h). Colours are `0xRRGGBB`
(`RGB(r, g, b)`), image pixels `0xAARRGGBB`.

| Area | Functions |
|------|-----------|
| App | `app_loop`, `app_quit`, `app_fps`, `app_from_home`, `app_path`, `app_dir` |
| Screens | `SCREEN_TOP` (400 x 240), `SCREEN_BOTTOM` (320 x 240, touch); `gfx_width`, `gfx_present`, `gfx_framebuffer`, `gfx_mark` |
| Shapes | `gfx_clear`, `gfx_pixel`, `gfx_get_pixel`, `gfx_rect`, `gfx_rect_blend`, `gfx_rect_outline`, `gfx_round_rect`, `gfx_gradient`, `gfx_line`, `gfx_circle`, `gfx_circle_outline`, `gfx_triangle` |
| Text | `gfx_text` (8x8), `gfx_text_scaled`, `gfx_print` (any font), `gfx_text_width`, `gfx_line_height`, `gfx_fonts_loaded`; fonts `FONT_MONO`, `FONT_SMALL`, `FONT_REGULAR`, `FONT_BOLD`, `FONT_TITLE` |
| Images | `gfx_image_load` (PNG with alpha, JPEG, BMP), `gfx_image_new`, `gfx_image_free`, `gfx_image`, `gfx_image_part` (sprite sheets), `gfx_image_scaled`, `gfx_image_error` |
| Console | `printf` and stdout/stderr, `con_init`, `con_clear`, `con_color`, `con_move`, `con_cols`, `con_rows`; ANSI `ESC[2J`, `ESC[K`, `ESC[row;colH` and colours |
| Input | `hid_scan`, `hid_keys_down`, `hid_keys_held`, `hid_keys_up`, `hid_wait`, `hid_touch_held`/`down`/`up`, `hid_touch_x`/`y`, `hid_touch_in`, `hid_cpad`; `KEY_A` ... `KEY_Y`, `KEY_TOUCH`, `KEY_CPAD_*` |
| Sound | `snd_load` (WAV), `snd_play`, `snd_play_volume`, `snd_music`, `snd_music_volume`, `snd_stop_music`, `snd_stop_sounds`, `snd_unload_all`, `snd_available` |
| Files | stdio, plus `fs_read_file`, `fs_write_file`, `fs_exists`, `fs_is_dir`, `fs_mkdir` (with parents), `fs_dir_open`/`read`/`close`, `fs_available` |
| System | `sys_millis`, `sys_micros`, `sys_sleep`, `sys_time`, `sys_battery`, `sys_battery_tenths`, `sys_battery_temperature`, `sys_battery_millivolts`, `sys_charging`, `sys_is_new3ds`, `sys_user_name`, `sys_language`, `sys_accent`, `sys_power_off`, `sys_reboot` |
| Network | `net_available`, `net_connect`, `net_connected`, `net_error`, `net_address`, `net_resolve`, `net_ping`, `net_on_wait`, `net_tcp_exchange`; `http_get`, `http_post`, `http_request`, `http_header`, `http_free`, `http_download`, `http_url_encode`; and BSD sockets ([Sockets](#sockets)) |

Notes:

* The Figtree fonts are the OS's, read from `SD:/Aurora/assets.pak` the first
  time one is used. Without the pack, or with a pack from an OS build with a
  different set of art, text falls back to the 8x8 font
  (`gfx_fonts_loaded()` says which).
* `hid_cpad()` gives -127..127 on each axis, 0 in a small dead zone;
  `KEY_CPAD_*` act as buttons when the pad is pushed about a third of the way.
* Sounds are WAV files, 8 or 16-bit PCM at any rate, mono or stereo (mixed
  down). They play on the core's voices: seven for effects, one for looped
  music. They share a 10 MB pool and stay loaded until `snd_unload_all()`.
* The console starts on the top screen at the first output; `con_init()`
  moves it. It draws into the screen like the `gfx_` functions, wraps at 50
  columns (40 on the bottom) and scrolls. Text shows at the next
  `app_loop()`, `hid_wait()` or `gfx_present()`. It is 8x8 ASCII: other
  characters show as a blank cell.

## The C library

The SDK links devkitARM's **newlib** and provides the system calls under it:

| | |
|--|--|
| `printf`, `puts`, `putchar`, stdout, stderr | the console |
| `fopen`, `fread`, `fwrite`, `fprintf`, `fscanf`, `fgets`, `fseek`, `remove`, `rename`, `stat`, `mkdir`, `rmdir`, `open`/`read`/`write`/`lseek`/`close` | the SD card (FatFs); 16 files open at once |
| `malloc`, `calloc`, `realloc`, `free` | a 16 MB heap |
| `time`, `gettimeofday`, `localtime`, `strftime` | the console clock as the Home Menu shows it (Settings > Clock included), counted on by the ARM9 timer |
| `clock`, `usleep`, `nanosleep`, `sleep` | the ARM9 timer, less any time the lid was closed |
| `rand`, `srand`, `qsort`, `bsearch`, `strtol`, `strtod`, `<string.h>`, `<ctype.h>`, `<math.h>` | as usual |
| `exit`, `atexit`, `abort`, `assert` | `exit` goes back to the Home Menu; `abort` and a failed `assert` show the message on the console and wait for HOME |
| `opendir`, `readdir`, `readdir_r`, `closedir`, `rewinddir`, `telldir`, `seekdir`, `scandir`, `alphasort` (SDK 1.6) | folders on the SD card, unsorted but for `scandir()`, without `.` and `..`; `d_type` is `DT_DIR` or `DT_REG` |
| `socket`, `connect`, `send`, `recv`, `sendto`, `recvfrom`, `bind`, `shutdown`, `close`, `getsockopt`, `setsockopt`, `getpeername`, `getsockname` (SDK 1.6) | IPv4 TCP and UDP through the ARM11 core, 8 sockets open at once; [Sockets](#sockets) says what the core allows |
| `getaddrinfo`, `freeaddrinfo`, `gai_strerror`, `gethostbyname`, `getnameinfo`, `inet_pton`, `inet_ntop`, `inet_aton`, `inet_addr`, `inet_ntoa`, `htons` and friends (SDK 1.6) | names through the network's DNS server; numbers only the other way |
| `poll`, `select`, `fcntl` (`O_NONBLOCK`), `ioctl` (`FIONBIO`, `FIONREAD`) (SDK 1.6) | sockets; files and the console count as always ready |

Not available: reading `stdin` (it is always at end of file), threads,
signals other than `abort`, listening sockets and IPv6. `rename()` replaces
an existing file, as POSIX says, and `localtime()` is the console's own time
(there are no time zones).

## Memory

| Range | Use |
|-------|-----|
| `0x22000000` up to `0x22F00000` | the app: code, data and `.bss` (15 MB); the build stops if it does not fit |
| `0x22F00000`-`0x23000000` | the stack, 1 MB, growing down from `0x23000000` |
| `0x23200000`, 1020 KB | the ARM11 core's network replies, copied out by `http_` calls |
| `0x233F0000`-`0x233FF140` | the core's TCP rings (sockets, HTTPS, streams), then its UDP rings |
| `0x23400000`, 10 MB | the sound pool (`snd_load`) |
| `0x23E00000` | the Wi-Fi firmware, staged there while `net_connect()` joins |
| `0x23E80000`, `0x23F00000` | the off-screen copies of the top and bottom screens |
| `0x23F40000` | the fonts, when used |
| `0x24000000`-`0x25000000` | the `malloc()` heap, 16 MB |
| `0x25100000`-`0x26900000` | scratch for loading images |

Big arrays belong in `malloc()` or `static` storage, not on the stack. The OS
keeps a copy of itself at `0x26000000` and the HOME return code at
`0x25004000`; an app that writes there through a wild pointer cannot go back
to the Home Menu (the console has to be restarted).

The ARM9 runs at 134 MHz with no floating-point unit and no divide
instruction: `float` and `/` work (in software) but cost more than on a PC, and
integer or fixed-point maths is faster in inner loops.

## Building

```
py sdk/aurcc.py build SOURCES... [-o APP.bin] [options]
```

`SOURCES` are `.c` (and `.s`) files, or folders: a folder brings every `.c`
inside it, at any depth (except folders named `build`), and its first
`*.icon` as the icon. Without `-o` the app is named after the first source or
folder.

| Option | |
|--------|--|
| `-o FILE` | the app file; its name is the app's name on the Home Menu |
| `--icon FILE` | the 32 x 32 Home Menu icon: a text `.icon` (32 lines of 32 characters, `#` lit, `;` comments, as in Auric) or a 32 x 32 `.png` (light, solid pixels lit) |
| `-I DIR`, `-D NAME[=VALUE]` | include folders and macros |
| `-O 0/1/2/3/s/g` | optimisation (default 2) |
| `--cflags "..."` | other compiler flags |
| `--magic AUR1` | an `AUR1` container instead of `AURC`, for an OS from before `AURC` (Beta v0.1.4 and earlier), which refuses `AURC` ("Not an AUR1 app") |
| `--magic AOS1` | an `AOS1` container, to boot as `AURORAOS.BIN` from the launcher |
| `--build-dir DIR` | where objects go (default `sdk/build`, git-ignored) |
| `--rebuild` | compile the SDK runtime again |
| `-v` | show each compiler command |

Apps compile with the OS's ARM9 flags (`-mcpu=arm946e-s -march=armv5te
-marm -O2`), `-Wall -Wextra`, and link with `--gc-sections`, so only the
runtime code an app uses ends up in it: a `printf` hello world is about 87 KB,
the Bricks example 167 KB. The link map is left beside the objects
(`sdk/build/apps/NAME/NAME.map`).

`py sdk/aurcc.py new NAME [--dir DIR]` starts a project: `NAME/main.c` (a
small program to change) and `NAME/NAME.icon`.

The tool's own tests: `py sdk/tests/test_aurcc.py` (the build test runs when
`arm-none-eabi-gcc` is on `PATH`). The runtime's own tests run its real code
on a PC against a model of the ARM11 core: `py sdk/tests/host/run.py`
([`tests/host/README.md`](tests/host/README.md)).

## Examples

| Folder | |
|--------|--|
| [`examples/hello`](examples/hello/main.c) | `printf` on the console: the app's path, the user's name, the console model |
| [`examples/demo`](examples/demo/main.c) | a tour: shapes, all fonts, a ball the circle pad pushes, the clock and battery, painting with the stylus, **X** saves the picture as a BMP with stdio; `sprite.png` and `bounce.wav` in its data folder are used when there |
| [`examples/bricks`](examples/bricks/main.c) | a breakout game: D-pad, circle pad or touch; the best score saved through `atexit()`; optional `paddle.wav`, `brick.wav`, `lost.wav` |
| [`examples/net`](examples/net/main.c) | the network: joins with a spinner from `net_on_wait()`, fetches a page (A), pings a host (Y), downloads the page into its data folder (X) |
| [`examples/system_menu`](examples/system_menu/main.c) | checks L + Down + Select while the app uses Down and Select for gameplay |
| [`examples/sockets`](examples/sockets/main.c) | BSD sockets and `<dirent.h>`: a page over a TCP socket (A), the time from an NTP server over UDP (X), the data folder through `scandir()` (Y) |

```
py sdk/aurcc.py build sdk/examples/bricks -o Bricks.bin
```

## How it works

An app is built from four parts: the app's objects; the SDK runtime in
[`runtime/`](runtime); the OS's own code it reuses (`src/screen.c` for
drawing, `src/os/Gpu9.c` for presents, `src/os/Touch9.c`, `src/os/Timer9.c`,
`src/i2c.c` and `src/power.c` for HOME, the clock and the battery, FatFs and
the SD driver, `src/image.c` and `src/jpeg.c`, `src/wavload.c`,
`src/assets.c` for the fonts, `src/os/crash.c` for the crash screen, and
`src/os/AppNet.c` with `WiFiJoin.c` and `Crypto.c` for the network); and
newlib. `aurcc.py` puts a generated header first (a branch over the
`AURICON1` icon block the Home Menu reads, see
[`../docs/apps.md`](../docs/apps.md)), links at `0x22000000` with
[`runtime/app.ld`](runtime/app.ld) and packs the result as `AURC`.

Before it starts an `AURC` app, the OS makes `SD:/Aurora/Apps/C/<name>`
(`aurora_c_data_make()` in `src/container.c`). The runtime's `app_dir()` uses
the same function to name the folder, so the two cannot disagree.

At start (`runtime/app_start.s`, then `app_entry()` in `runtime/app_main.c`):

1. The stack is set and `.bss` cleared.
2. The ARM9 exception vectors are pointed at the crash handler linked into the
   app; they still pointed into the OS the app replaced.
3. The timer and I2C start; the app's path is read from the launch block the
   OS writes at `0x25008100` (`AURORA_LAUNCH_INFO_ADDR` in
   `include/loader.h`). An app booted directly by the launcher, or built
   with `--magic AUR1` and run by an OS from before C apps (which lists only
   `SD:/Aurora/Apps`), has no block, so then the path is made from the name
   `aurcc.py` stored: `/Aurora/Apps/<name>.bin` if that file is there,
   otherwise `/Aurora/Apps/C/<name>.bin`.
4. Whatever buttons are held (the A that started the app) are not new presses.
5. With the ARM11 core there, `gpu_init()`; both screens are cleared to black
   and shown.
6. Constructors run, then `main()`; its return goes to `exit()`.

**Presents** copy each screen drawn on to the panel with the GPU, which
switches framebuffers at the vertical blank (both screens together when both
changed), so there is no tearing and `app_loop()` is paced by the display.
Without the GPU the CPU copies and the ARM9 timer paces the loop.

**HOME** is a flag the MCU raises; `hid_scan()` reads it over I2C once a frame
and calls `exit(0)`. `_exit()` closes files, stops the app's voices, puts the
newest frame back in framebuffer A and jumps to the OS's return stub, which
restores the Home Menu from its copy at `0x26000000`.

**A crash** (a data abort, a bad instruction) shows the OS's blue crash screen
with the registers. In an app it reads "HOME: back. Power off in 10s": HOME
returns to the Home Menu (`g_crash_poll` in `include/crash.h`), otherwise the
console powers off as it does after an OS crash. An ARM11 fault reported while
the app runs shows the same screen.

**The network** is `src/os/AppNet.c`, the OS's network client for apps,
shared with Auric. The app has replaced the OS, so it posts the core's Wi-Fi
commands itself (`AUDIO_CMD_WIFI_NET` for DNS, ping and one TCP exchange,
`AUDIO_CMD_WIFI_BOOT` to join) and waits for the core to acknowledge each. A
join reads `SD:/Aurora/wifi/network.txt`, stages the firmware at
`0x23E00000` and hands the core the network's name and WPA2 key
(`src/os/WiFiJoin.c`, which the OS uses too). HTTP is HTTP/1.0 with
`Connection: close`; the core reads the response into its 1020 KB buffer at
`0x23200000`, where it is parsed (chunked bodies undone, redirects followed)
and then copied into one `malloc()` block per response.
`runtime/app_net.c` keeps everything else off the core meanwhile: presents
go to framebuffer A by the CPU while a `net_on_wait()` handler runs, sounds
are dropped, and leaving waits for the call.

**System calls** for newlib are in `runtime/app_syscalls.c`: file descriptors
0-2 are the console, 3 to 18 are FatFs files (`runtime/app_fs.c`), 19 to 26
sockets (`runtime/app_socket.c`), `_sbrk` hands out the heap, and the locks do
nothing (one thread). The socket code is reached through a table the first
`socket()` fills in, so an app that makes none does not link it.

**Sockets** move bytes through rings the ARM9 and the core share: a TCP socket
is a `WIFI_NETOP_TCP_BG` connection the core keeps going between its other work
(`appnet_tcp_open` in `src/os/AppNet.c`, `src/os/WiFiTcp.c`), and a UDP socket
the core's one UDP port (`appnet_udp_bind`, `src/os/WiFiUdp.c`); see
[`../docs/wifi.md`](../docs/wifi.md), *A connection in the background* and
*A UDP port for apps*. `app_poll_home()`, which the frame loop and every wait
call, tells the core the app is still using them. newlib's headers have no
sockets, so `include/` carries `<sys/socket.h>`, `<netinet/in.h>`,
`<netinet/tcp.h>`, `<arpa/inet.h>`, `<netdb.h>`, `<poll.h>` and
`<sys/ioctl.h>`.

**Folders** through `<dirent.h>` use devkitARM's header, whose `DIR` is
libsysbase's; libsysbase is not linked, so `runtime/app_dirent.c` fills that
`DIR` in over `fs_dir_open()`.

| File | |
|------|--|
| `include/aurora_app.h` | the public API |
| `runtime/app_start.s` | crt0, `_init`/`_fini`, the cache helpers `Gpu9.c` uses |
| `runtime/app.ld` | the link: header and `_start` first, `.bss`, the size check |
| `runtime/app_main.c` | start-up, `app_loop`, HOME, leaving, the crash-screen hook, posts to the ARM11 core, `USER.dat` |
| `runtime/app_gfx.c` | screens, presents, shapes, text and fonts, images |
| `runtime/app_console.c` | the console behind stdout and stderr |
| `runtime/app_hid.c` | buttons, touch (with the user's calibration), the circle pad |
| `runtime/app_sound.c` | WAV loading and the core's voices |
| `runtime/app_fs.c` | paths, the file descriptor table, `fs_` functions |
| `runtime/app_sys.c` | time, the clock, battery, the user's settings, power |
| `runtime/app_net.c` | the network: `net_` and `http_` over `src/os/AppNet.c`, CPU presents while a call waits |
| `runtime/app_socket.c` | BSD sockets, `poll`, `select`, `fcntl`, `ioctl`, `getaddrinfo` and the address helpers |
| `runtime/app_dirent.c` | `<dirent.h>` |
| `runtime/app_syscalls.c` | newlib's system calls, `assert`, `abort` |
| `aurcc.py` | the build tool |

## Video

`py sdk/aurcc.py build MyApp --video` adds the video engine, about 2 MB: H.264 and
AAC decoded on the ARM11's other cores, from fragmented MP4 with video and sound
in separate files. See `video_*` in `aurora_app.h`, [`docs/video.md`](../docs/video.md)
and `examples/videotest`. The first such build also compiles FFmpeg
([`lib/ffmpeg`](../lib/ffmpeg/README.md), LGPL), about two minutes. `json_*` and
`http_stream_*` are for every app.

## Limits

* One screen layer each, 24-bit colour, no 3D or stereoscopic drawing yet
  (the OS's PICA200 3D pipeline is not exposed).
* The network is HTTP and HTTPS (TLS 1.2 only) over the saved Wi-Fi network,
  one call at a time, requests up to 2 KB and responses up to 1020 KB
  (`http_download()` for bigger files), and BSD sockets: one TCP connection
  and one UDP port at a time, UDP datagrams sent up to 524 bytes, no
  listening, no IPv6, no TLS on a socket or `net_tcp_exchange()`
  ([Sockets](#sockets)). The sockets are new: they were run on a PC against
  a model of the core, not yet on a console.
* The New 3DS's extra buttons (ZL, ZR, C-stick) are not read.
* One thread; no interrupts.
* `stdin` is empty; there is no on-screen keyboard API yet.
* `AURC` apps need this version of the OS; older ones refuse them (build
  with `--magic AUR1` for those).
* The SDK is new. It was checked on a PC (the runtime run against stubbed
  hardware, a RAM disk and real FatFs, with the frames looked at), and one C
  app, the CPU stress test in `../cpu-stress`, has run on a New 3DS; the
  examples have not been tried on a console yet.
  [`../docs/apps.md`](../docs/apps.md) says what was checked.

## Licences

The runtime and the OS code an app links are part of AuroraOS: GPL-3.0, with
`src/os/Gpu9.c` under GPL-2.0, and the network code (`src/os/AppNet.c`,
`WiFiJoin.c`, `Crypto.c`, `WiFiTcp.c`, part of the Wi-Fi driver) under
GPL-2.0 too, as is `WiFiUdp.c` (see the main [README](../README.md#license)). BearSSL
(`../lib/bearssl`, for HTTPS) is under the MIT licence.
newlib is under its own BSD-style licences. An app built with the SDK
contains that code, so its distribution follows those licences.
