# Auric v0.2: language reference

*"Coding too hard? Try Auric!"*

Auric is a tiny, statically-typed language that **transpiles to freestanding C**
and compiles into a bootable app for [AuroraOS](../../README.md) on the Nintendo
3DS. This document is the complete v0.2 spec, the language is deliberately
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

| Category   | Operators                          | Operand types        | Result  |
|------------|------------------------------------|----------------------|---------|
| Arithmetic | `+` `-` `*` `/` `%`                | int×int or float×float (`%` int only) | same |
| Bitwise    | `&` `\|` `^` `<<` `>>`             | int×int              | `int`   |
| Comparison | `<` `<=` `>` `>=`                  | int×int or float×float | `bool` |
| Equality   | `==` `!=`                          | any matching type    | `bool`  |
| Logical    | `&&` `\|\|` `!`                    | `bool`               | `bool`  |
| Unary      | `-` (negate), `!` (not)            | number / bool        | same    |

Precedence (low → high): `||`, `&&`, `|`, `^`, `&`, equality, comparison,
shifts, `+ -`, `* / %`, unary, the same order as C, so `a & b == c` means
`a & (b == c)`. Parenthesize bit tests: `(flags & MASK) != 0`.

## Built-in functions

These map one-to-one onto AuroraOS's API through the runtime shim
([`../runtime`](../runtime)). All drawing targets the **top screen**
(400×240, origin top-left, 8×8 font).

| Built-in                          | Does                                             | AuroraOS call     |
|-----------------------------------|--------------------------------------------------|-------------------|
| `print(text, x, y, color)`        | draw `text` at pixel `(x, y)`                     | `draw_string`     |
| `print_int(value, x, y, color)`   | draw a signed decimal number                      | `draw_string`     |
| `clear(color)`                    | fill the screen; remembers `color` as text bg     | `clear_screen`    |
| `fill_rect(x, y, w, h, color)`    | fill a rectangle (use it to draw bands/shapes)    | `draw_filled_rect`|
| `wait_key(button)`                | block until `button` is newly pressed             | `get_keys_down`   |
| `keys_down() -> int`              | buttons newly pressed since the last call         | `get_keys_down`   |
| `keys_held() -> int`              | buttons currently held                            | `get_keys`        |
| `buffered(on)`                    | draw off-screen until `present()`                 | `screen_use_backbuffer` |
| `present()`                       | show the off-screen frame                         | `screen_present_top` |
| `rand(n) -> int`                  | pseudo-random int in `[0, n)`                     | seeded from the RTC |
| `millis() -> int`                 | milliseconds since the app started                 | ARM9 hardware timer |
| `delay(cycles)`                   | busy-wait ~`cycles` iterations                     | `delay`           |

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

### The HOME button

You don't handle HOME yourself: the runtime polls it inside **every** built-in
call and, when launched from the AuroraOS home menu, returns there instantly on a
press. Keep a `delay(...)` (or another built-in) in any long-running loop so HOME
stays responsive. See `../../sample/rainbow.aur`.

### App icons

Every compiled app embeds a 32×32 icon that the home menu shows. Supply your own
with `aurc build … --icon my.icon` (a text bitmap: 32 lines, `#` = on); without
`--icon`, a default icon is used.

## Predefined constants

**Colours** (packed `0xRRGGBB`, unpacked to an AuroraOS `Color` by the shim):

`BLACK` `WHITE` `RED` `GREEN` `BLUE` `CYAN` `MAGENTA` `YELLOW` `ORANGE`
`AURORA` `GRAY` `DARK_GRAY`

A colour is just an `int`, so you can also pass a raw literal like `0xFF8800`.

**Buttons** (HID bitmasks, matching `BUTTON_*` in `include/aurora.h`):

`KEY_A` `KEY_B` `KEY_SELECT` `KEY_START` `KEY_UP` `KEY_DOWN` `KEY_LEFT`
`KEY_RIGHT` `KEY_L` `KEY_R`

HOME is not in this list: the runtime handles it for you (see above).

You cannot declare a variable or function that reuses a predefined name.

## Comments

```auric
// line comment
/* block comment */
```

## What Auric still leaves out

Structs, pointers, `for` loops, string operations, float↔int conversion,
multi-dimensional arrays, and passing arrays to functions. The scope stays
deliberately small; these are candidates for later versions.

Flatten a 2D grid by hand until then, `board[row * WIDTH + col]`, as
`Games/Tetris.aur` does.
