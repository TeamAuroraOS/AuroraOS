# AuroraOS native apps in C

Apps for AuroraOS can be written in plain C as well as in
[Auric](../auric-lang/README.md). A C app uses one header,
[`include/aurora_app.h`](include/aurora_app.h), for the screens, input, sound,
files, system information and the network (HTTP), and the whole C standard
library besides:
`printf()` writes to an on-screen console, `malloc()` has a 16 MB heap,
`fopen()` reads and writes the SD card, `time()` reads the console's clock,
and `<math.h>`, `qsort()`, `strtod()` and the rest are there.

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

HOME is noticed in `app_loop()`, `hid_scan()`, `hid_wait()` and `sys_sleep()`;
a long computation that calls none of them does not respond to it until it
does.

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
plain HTTP. AuroraOS has no TLS yet, so `https://` addresses fail, and a site
that sends `http://` visitors on to `https://` answers with its redirect (the
response comes back as it is, and `net_error()` says so).

```c
HttpResponse *r = net_connect() ? http_get("http://example.com/") : NULL;
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
  change. A request usually takes well under a second, at most 15.
* `net_on_wait(fn)` gives a function to call about ten times a second while a
  call waits: it can draw (a spinner, the seconds) and `gfx_present()`, which
  the CPU then copies to the screens. It must not make network calls itself.
* HOME pressed during a call leaves the app once the call is over.

The `net` example joins with a spinner on the bottom screen, fetches a page,
pings a host and downloads the page into its data folder.

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
| Network | `net_available`, `net_connect`, `net_connected`, `net_error`, `net_address`, `net_resolve`, `net_ping`, `net_on_wait`, `net_tcp_exchange`; `http_get`, `http_post`, `http_request`, `http_header`, `http_free`, `http_download`, `http_url_encode` |

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
| `clock`, `usleep`, `nanosleep`, `sleep` | the ARM9 timer |
| `rand`, `srand`, `qsort`, `bsearch`, `strtol`, `strtod`, `<string.h>`, `<ctype.h>`, `<math.h>` | as usual |
| `exit`, `atexit`, `abort`, `assert` | `exit` goes back to the Home Menu; `abort` and a failed `assert` show the message on the console and wait for HOME |

Not available: reading `stdin` (it is always at end of file), threads,
BSD sockets (the network is the `net_` and `http_` functions), signals
other than `abort`, and directories through `<dirent.h>` (use
`fs_dir_open()`). `rename()` replaces an existing file, as POSIX says, and
`localtime()` is the console's own time (there are no time zones).

## Memory

| Range | Use |
|-------|-----|
| `0x22000000` up to `0x22F00000` | the app: code, data and `.bss` (15 MB); the build stops if it does not fit |
| `0x22F00000`-`0x23000000` | the stack, 1 MB, growing down from `0x23000000` |
| `0x23200000`, 1020 KB | the ARM11 core's network replies, copied out by `http_` calls |
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
`arm-none-eabi-gcc` is on `PATH`).

## Examples

| Folder | |
|--------|--|
| [`examples/hello`](examples/hello/main.c) | `printf` on the console: the app's path, the user's name, the console model |
| [`examples/demo`](examples/demo/main.c) | a tour: shapes, all fonts, a ball the circle pad pushes, the clock and battery, painting with the stylus, **X** saves the picture as a BMP with stdio; `sprite.png` and `bounce.wav` in its data folder are used when there |
| [`examples/bricks`](examples/bricks/main.c) | a breakout game: D-pad, circle pad or touch; the best score saved through `atexit()`; optional `paddle.wav`, `brick.wav`, `lost.wav` |
| [`examples/net`](examples/net/main.c) | the network: joins with a spinner from `net_on_wait()`, fetches a page (A), pings a host (Y), downloads the page into its data folder (X) |

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
0-2 are the console, 3 on are FatFs files (`runtime/app_fs.c`), `_sbrk` hands
out the heap, and the locks do nothing (one thread).

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
| `runtime/app_syscalls.c` | newlib's system calls, `assert`, `abort` |
| `aurcc.py` | the build tool |

## Limits

* One screen layer each, 24-bit colour, no 3D or stereoscopic drawing yet
  (the OS's PICA200 3D pipeline is not exposed).
* The network is plain HTTP over the saved Wi-Fi network: no HTTPS (no TLS
  yet), no BSD sockets, no listening or UDP, one call at a time, requests up
  to 2 KB and responses up to 1020 KB (`http_download()` for bigger files).
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
`WiFiJoin.c`, `Crypto.c`, part of the Wi-Fi driver) under GPL-2.0 too (see
the main [README](../README.md#license)).
newlib is under its own BSD-style licences. An app built with the SDK
contains that code, so its distribution follows those licences.
