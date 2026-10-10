# Auric v0.5: language reference

*"Coding too hard? Try Auric!"*

Auric is a tiny, statically-typed language that **transpiles to freestanding C**
and compiles into a bootable app for [AuroraOS](../../README.md) on the Nintendo
3DS. This document is the complete v0.5 spec, the language is deliberately
small.

## A whole program

```auric
fn main() {
    clear(BLACK);
    print("Coding too hard? Try Auric!", 40, 108, AURORA);
    print("Press A to exit.", 40, 140, WHITE);
    wait_key(KEY_A);
}
```

Every program needs a `fn main()` that takes no parameters and returns nothing;
it is the entry point the runtime calls.

## Types

| Type      | Meaning                     | Lowers to C     |
|-----------|-----------------------------|-----------------|
| `int`     | 32-bit signed integer       | `int`           |
| `float`   | 32-bit float (soft-float)   | `float`         |
| `bool`    | `true` / `false`            | `int` (0 or 1)  |
| `string`  | immutable text literal      | `const char *`  |
| `T[N]`    | fixed-length array of `T`   | `T name[N]`     |

There are no implicit conversions: you cannot mix `int` and `float` in one
expression, and conditions must be `bool`.

## Globals

A `let` outside any function is a global, visible to every function:

```auric
let score = 0;
let board: int[200];

fn bump() { score = score + 1; }
```

A global's initializer must be a **constant expression**: literals, predefined
constants, and arithmetic on them. It becomes a C file-scope initializer, so
`let n = some_call();` at global scope is rejected.

## Arrays

```auric
let board: int[200];    // always zero-filled; there is no initializer syntax
board[3] = 7;
let v = board[3];
```

Arrays may be global or local, and the index must be `int`. **Arrays cannot be
passed to functions**, declare them global and let functions reach them
directly. There is no bounds checking: indexing past the end corrupts memory,
exactly as in C.

## Functions

```auric
fn name(param: type, ...) -> ret_type {
    ...
    return value;
}
```

* The `-> ret_type` is omitted for functions that return nothing (void).
* Functions may be called before they are defined (forward-declared for you).
* Recursion is allowed.

## Statements

```auric
let x = 1 + 2;        // type inferred (int)
let y: float = 3.5;   // explicit type
x = x + 1;            // assignment (variable must already exist)

if cond { ... } else if other { ... } else { ... }

while cond { ... }
break;                // leave the innermost loop
continue;             // next iteration of the innermost loop

return;               // in a void function
return expr;          // in a value-returning function
```

Conditions are **not** parenthesized (`if x < 3 { ... }`). Blocks always use
braces. Each `{ ... }` block is its own scope.

## Operators

| Category   | Operators               | Operand types                             | Result |
|------------|-------------------------|-------------------------------------------|--------|
| Arithmetic | `+` `-` `*` `/` `%`     | int x int or float x float (`%` int only) | same   |
| Bitwise    | `&` `\|` `^` `<<` `>>`   | int x int                                 | `int`  |
| Comparison | `<` `<=` `>` `>=`       | int x int or float x float                | `bool` |
| Equality   | `==` `!=`               | any matching type; strings by their text  | `bool` |
| Logical    | `&&` `\|\|` `!`           | `bool`                                    | `bool` |
| Unary      | `-` (negate), `!` (not) | number / bool                             | same   |

Precedence (low -> high): `||`, `&&`, `|`, `^`, `&`, equality, comparison,
shifts, `+ -`, `* / %`, unary, the same order as C, so `a & b == c` means
`a & (b == c)`. Parenthesize bit tests: `(flags & MASK) != 0`.

## Built-in functions

These map one-to-one onto AuroraOS's API through the runtime shim
([`../runtime`](../runtime)). Drawing goes to the screen picked with
`screen()`: the **top screen** (400x240) unless the app asks for the bottom one
(320x240). Coordinates start at the top-left corner; text uses the 8x8 font.

| Built-in                          | Does                                             | AuroraOS call     |
|-----------------------------------|--------------------------------------------------|-------------------|
| `screen(which)`                   | draw on `TOP` or `BOTTOM` from now on             | `VRAM_TOP_LA` / `VRAM_BOT_A` |
| `print(text, x, y, color)`        | draw `text` at pixel `(x, y)`                     | `draw_string`     |
| `print_int(value, x, y, color)`   | draw a signed decimal number                      | `draw_string`     |
| `clear(color)`                    | fill the screen; remembers `color` as its text bg | `clear_screen`    |
| `fill_rect(x, y, w, h, color)`    | fill a rectangle (use it to draw bands/shapes)    | `draw_filled_rect`|
| `wait_key(button)`                | block until `button` is newly pressed             | `get_keys_down`   |
| `keys_down() -> int`              | buttons newly pressed since the last call         | `get_keys_down`   |
| `keys_held() -> int`              | buttons currently held                            | `get_keys`        |
| `touch_down() -> bool`            | a touch began since the last call                 | `touch_read`      |
| `touch_held() -> bool`            | the bottom screen is being touched                | `touch_read`      |
| `touch_up() -> bool`              | the stylus lifted since the last call             | `touch_read`      |
| `touch_x() -> int`                | x of the touch, or of the last one (-1: none yet) | `touch_read`      |
| `touch_y() -> int`                | y of the touch, or of the last one (-1: none yet) | `touch_read`      |
| `touch_in(x, y, w, h) -> bool`    | that position lies inside the rectangle           | `touch_in`        |
| `buffered(on)`                    | draw off-screen until `present()`                 | `screen_use_backbuffer` |
| `present()`                       | show the off-screen frame on both screens         | `screen_present_top` / `_bottom` |
| `rand(n) -> int`                  | pseudo-random int in `[0, n)`                     | seeded from the RTC |
| `millis() -> int`                 | milliseconds since the app started                 | ARM9 hardware timer |
| `delay(cycles)`                   | busy-wait ~`cycles` iterations                     | `delay`           |
| `load_sound(path) -> int`         | read a WAV from the SD card; `-1` if it cannot     | `wav_load`        |
| `play_sound(handle)`              | play once, on a free effect voice                  | ARM11 voice       |
| `play_music(handle)`              | loop on the music voice                            | ARM11 voice       |
| `stop_music()`                    | silence the music                                  | ARM11 voice       |
| `stop_sounds()`                   | silence every effect                               | ARM11 voice       |
| `net_connect() -> bool`           | join the Wi-Fi network saved in Settings           | ARM11 Wi-Fi       |
| `net_online() -> bool`            | the network link is up                             | ARM11 Wi-Fi       |
| `net_error() -> string`           | why the last network call failed                   | `src/os/AppNet.c` |
| `net_address() -> string`         | the console's address on the network               | ARM11 Wi-Fi       |
| `http_get(url) -> int`            | an HTTP GET; the reply's status, 0 if none         | ARM11 TCP         |
| `http_post(url, body) -> int`     | an HTTP POST                                       | ARM11 TCP         |
| `http_param(name, value)`         | a parameter for the next request (`_int`: a number) | `src/os/AppNet.c` |
| `http_header(name, value)`        | a header for the next request                      | `src/os/AppNet.c` |
| `http_text() -> string`           | the last reply's body                              | in place          |
| `http_length() -> int`            | its size in bytes                                  | in place          |
| `http_lines() -> int`             | how many lines it has                              | in place          |
| `http_line(n) -> string`          | line `n`, from 0                                   | in place          |
| `http_save(path) -> bool`         | write the last reply to the SD card                | FatFs             |
| `http_download(url, path) -> bool` | save a file of any size, in pieces                | ARM11 TCP, FatFs  |
| `json_string(path) -> string`     | a value of the last reply, read as JSON            | `src/os/Json.c`   |
| `json_int` / `json_float` / `json_bool(path)` | the same as a number or a truth value | `src/os/Json.c`   |
| `json_has(path) -> bool`          | the value is there                                 | `src/os/Json.c`   |
| `json_count(path) -> int`         | the elements of an array, members of an object     | `src/os/Json.c`   |
| `json_index(n)`                   | what `#` stands for in the paths after it          |                   |

### Animation and games

`wait_key` blocks, which is fine for a slideshow but useless for a game. Poll
with `keys_down()` (edge, fires once per press) and `keys_held()` (level, true
while held), testing the `KEY_*` masks with `(k & KEY_A) != 0`.

Call `buffered(true)` once at startup and `present()` at the end of each frame.
Without it every drawing call goes straight to the panel and the player watches
the frame being built. When the app was launched from the Home Menu, `present()`
is a **GPU blit**; the ARM11 core is still resident, so a full-screen present
costs one DMA rather than a 288 KB CPU copy. A directly booted app falls back to
the CPU copy.

Buffered mode also means **the backbuffer keeps what you drew last frame**, so
draw fixed chrome once and repaint only what moves.

**Never pace a game with `delay()`.** Drive it with `millis()`, which reads an
ARM9 hardware timer:

```auric
let next_step = millis() + 500;
while true {
    let now = millis();
    if now >= next_step {
        next_step = now + 500;
        // ... one step of the simulation ...
    }
    draw();
    present();
}
```

That way the speed of the game is fixed in real time and the frame rate only
affects how smooth it looks. See `../../Games/Tetris_Source/Tetris.aur` for a
complete game built this way.

### Two screens

An app starts out drawing on the top screen. `screen(BOTTOM)` sends every
drawing built-in after it (`clear`, `print`, `print_int`, `fill_rect`) to the
bottom screen, and `screen(TOP)` sends them back:

```auric
screen(TOP);
clear(BLACK);
print("Score", 8, 8, WHITE);

screen(BOTTOM);
clear(DARK_GRAY);
fill_rect(120, 100, 80, 40, AURORA);   // a button
print("Start", 140, 116, BLACK);
```

* The bottom screen is 320x240, so a line of the 8x8 font holds 40 characters
  there instead of 50. Drawing past an edge is clipped, on either screen.
* Each screen keeps its own text background: `clear()` sets it for the screen
  it fills.
* Until an app draws on the bottom screen, it keeps the Home Menu's last
  picture (the "Launch app" card). Clear it at startup if the app uses it.
* In buffered mode `present()` shows both screens, but copies only the ones
  drawn on since the previous `present()`: a game that redraws only the top
  each frame pays for one copy.

### Touch

The bottom screen is a touchscreen. Positions are in its pixels, `x` from 0 to
319 and `y` from 0 to 239, corrected by the user's Settings > Touch Calibration.

`touch_down()` and `touch_up()` are edges, like `keys_down()`: each is true once
when a touch begins or the stylus lifts, counted since that built-in was last
called. `touch_held()` is the level: true while the stylus is down.
`touch_x()` and `touch_y()` give where the stylus is, or where it was when it
lifted (both `-1` before the first touch), and `touch_in(x, y, w, h)` tests
that position against a rectangle:

```auric
// A button that acts on the tap:
if touch_down() && touch_in(120, 100, 80, 40) {
    start_game();
}

// Or only when the stylus lifts on it, so a touch can slide off to cancel:
if touch_up() && touch_in(120, 100, 80, 40) {
    start_game();
}

// Drawing while the stylus is down:
if touch_held() {
    screen(BOTTOM);
    fill_rect(touch_x() - 1, touch_y() - 1, 3, 3, WHITE);
}
```

* A touch already on the screen when the app first asks is ignored until the
  stylus lifts, so the tap that launched the app does not press anything.
* The touchscreen is sampled, so a quick stroke arrives as points some pixels
  apart. Join them to draw a line, as `../examples/paint.aur` does.
* The first touch built-in an app calls reads the calibration from
  `SD:/Aurora/USER.dat`, once.
* Like sound, touch needs the ARM11 core, so it works only when the app was
  launched from the Home Menu. In a directly booted app the screen is never
  touched: `touch_held()` stays false and `touch_x()` stays `-1`.

`../examples/paint.aur` is a small drawing app that uses both screens and the
stylus.

### Sound

Sounds are WAV files on the SD card, loaded once and then played by handle:

```auric
let jump = -1;
let theme = -1;

fn main() {
    jump = load_sound("Aurora/Apps/MYGAME/JUMP.WAV");
    theme = load_sound("Aurora/Apps/MYGAME/THEME.WAV");
    play_music(theme);
    // ... and on an event:
    play_sound(jump);
}
```

* The path is counted from the root of the card. Long file names work, and
  letter case does not matter. Keep an app's files in a folder beside it, named
  after it (see `docs/apps.md`).
* Any uncompressed 8 or 16-bit PCM WAV works, mono or stereo. Stereo is mixed to
  mono, and a file above 32 kHz loads at half its rate, which halves the memory
  it takes. `tools/sound_prep.py` converts files ahead of time to mono 22,050 Hz,
  which also makes them quicker to load.
* Every sound shares one 10 MB pool, nearly four minutes of 22 kHz mono, and at
  most 32 can be loaded.
* `load_sound` returns `-1` when a file is missing or unusable, and playing `-1`
  does nothing, so a game still runs without its sounds.
* Up to seven effects play at once alongside the music; another takes over one
  of their voices. `play_music` loops, replacing any music already playing, at
  about 60% volume so effects carry over it.
* Sound needs the ARM11 core, so, like the GPU present, it works only when the
  app was launched from the Home Menu. In a directly booted app every
  `load_sound` returns `-1`.
* HOME stops every sound before the Home Menu comes back.

Loading reads the whole file, so a long track takes a moment: draw a message and
`present()` first, as `Games/Tetris_Source/Tetris.aur` does.

### The network

An app can reach the internet over the Wi-Fi network saved in Settings > Wi-Fi.
It speaks plain HTTP: AuroraOS has no TLS yet, so `https://` addresses do not
work (many sites send `http://` visitors to `https://`; such a reply comes back
as its 301 or 302, with `net_error()` saying so).

```auric
fn main() {
    print("Connecting...", 8, 8, WHITE);
    if !net_connect() {
        print(net_error(), 8, 24, RED);
        wait_key(KEY_A);
        return;
    }
    if http_get("http://ip-api.com/json/") == 200 {
        print(json_string("city"), 8, 24, WHITE);
    } else {
        print(net_error(), 8, 24, RED);
    }
    wait_key(KEY_A);
}
```

* `net_connect()` joins the saved network, unless the console is on it
  already (aShop, the Terminal or another app may have joined it, and a join
  stays up until the console is turned off or the network drops). The first
  join takes about 25
  seconds, so draw a message before it. It returns `false` when it cannot, and
  `net_error()` says why, e.g. "no network saved: pick one in Settings >
  Wi-Fi".
* Every network built-in waits until it is done, and the screens keep their
  last frame meanwhile. A request usually takes well under a second, and at
  most 15.
* `http_get(url)` and `http_post(url, body)` return the reply's status (200,
  404, ...), or `0` when no reply came (not connected, no such host, no
  answer; `net_error()` says which). Redirects are followed. The address may
  carry a port and a query: `"http://example.com:8080/api?q=1"`.
* The reply stays until the next request: `http_text()` is its body as text,
  `http_length()` its size in bytes, `http_lines()` and `http_line(n)` its
  lines (from 0, without their line ends), and `http_save(path)` writes it to
  the SD card. A reply can be up to 1020 KB. `http_download(url, path)` saves
  a file of any size, in pieces, without keeping it as the reply.
* `http_param(name, value)` and `http_param_int(name, value)` add a parameter
  to the next request's address (`?name=value&...`, encoded for you), and
  `http_header(name, value)` a header (an API key, say). They count for that
  one request. `http_post` with an empty body sends the parameters as the
  body, as a web form does; a body that starts with `{` or `[` is sent as
  JSON, anything else as a form, unless a `Content-Type` header says
  otherwise.
* A request, with its address, headers and body, must fit in 2 KB.
* `net_online()` asks whether the link is still up, and `net_address()` is
  the console's address on the network (`""` when not connected).
* Like sound, the network needs the ARM11 core, so it works only in an app
  launched from the Home Menu. HOME pressed during a network built-in takes
  effect when it is done.

#### JSON

Most web APIs answer in JSON. `json_string(path)`, `json_int(path)`,
`json_float(path)`, `json_bool(path)`, `json_has(path)` and `json_count(path)`
read the last reply as JSON. A path names members of objects and elements of
arrays, separated by dots: `"items.0.name"` is the `name` of the first element
of `items`, and `""` is the whole reply. A value that is not there gives `""`,
`0`, `0.0` or `false` (and `json_has` says `false`).

```auric
// {"city": "Paris", "temp": 21.5, "items": [{"name": "one"}, {"name": "two"}]}
print(json_string("city"), 8, 8, WHITE);
let t = json_float("temp");
let n = json_count("items");
let i = 0;
while i < n {
    json_index(i);
    print(json_string("items.#.name"), 8, 24 + i * 10, WHITE);
    i = i + 1;
}
```

* `json_index(n)` sets what a `#` in the paths after it stands for, to walk
  through an array.
* `json_int` rounds, and reads a number written as a string (`"17"`) too.
  `json_string` of a number, `true` or a whole object gives it as written.
* Text outside ASCII shows as `?`, as the 8x8 font has no other letters.
* **Strings from a reply** (`http_text`, `http_line`, `json_string`,
  `net_error`) **change at the next request**, even one kept in a variable:
  print or compare them before asking again, and keep numbers with `json_int`.

`../examples/net.aur` joins, asks ip-api.com where the console is online and
sends a score to httpbin.org with `http_param` and `http_post`.

### The HOME button

You don't handle HOME yourself: the runtime polls it inside **every** built-in
call and, when launched from the AuroraOS home menu, returns there instantly on a
press. Keep a `delay(...)` (or another built-in) in any long-running loop so HOME
stays responsive. See `../../sample/rainbow.aur`.

### App icons

Every compiled app embeds a 32x32 icon that the home menu shows. Supply your own
with `aurc build ... --icon my.icon` (a text bitmap: 32 lines, `#` = on); without
`--icon`, a default icon is used.

## Predefined constants

**Colours** (packed `0xRRGGBB`, unpacked to an AuroraOS `Color` by the shim):

`BLACK` `WHITE` `RED` `GREEN` `BLUE` `CYAN` `MAGENTA` `YELLOW` `ORANGE`
`AURORA` `GRAY` `DARK_GRAY`

A colour is just an `int`, so you can also pass a raw literal like `0xFF8800`.

**Buttons** (HID bitmasks, matching `BUTTON_*` in `include/aurora.h`):

`KEY_A` `KEY_B` `KEY_X` `KEY_Y` `KEY_SELECT` `KEY_START` `KEY_UP` `KEY_DOWN`
`KEY_LEFT` `KEY_RIGHT` `KEY_L` `KEY_R`

HOME is not in this list: the runtime handles it for you (see above).

**Screens** (for `screen()`): `TOP` `BOTTOM`

You cannot declare a variable or function that reuses a predefined name.

## Comments

```auric
// line comment
/* block comment */
```

## What Auric still leaves out

Structs, pointers, `for` loops, string operations (joining, slicing; `==`
compares), float<->int conversion, multi-dimensional arrays, and passing
arrays to functions. The scope stays
deliberately small; these are candidates for later versions.

Flatten a 2D grid by hand until then, `board[row * WIDTH + col]`, as
`Games/Tetris_Source/Tetris.aur` does.
