# Auric v0.1 — language reference

*"Coding too hard? Try Auric!"*

Auric is a tiny, statically-typed language that **transpiles to freestanding C**
and compiles into a bootable app for [AuroraOS](../../README.md) on the Nintendo
3DS. This document is the complete v0.1 spec — the language is deliberately
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

| Type     | Meaning                     | Lowers to C     |
|----------|-----------------------------|-----------------|
| `int`    | 32-bit signed integer       | `int`           |
| `float`  | 32-bit float (soft-float)   | `float`         |
| `bool`   | `true` / `false`            | `int` (0 or 1)  |
| `string` | immutable text literal      | `const char *`  |

There are no implicit conversions: you cannot mix `int` and `float` in one
expression, and conditions must be `bool`.

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

return;               // in a void function
return expr;          // in a value-returning function
```

Conditions are **not** parenthesized (`if x < 3 { ... }`). Blocks always use
braces. Each `{ ... }` block is its own scope.

## Operators

| Category   | Operators                          | Operand types        | Result  |
|------------|------------------------------------|----------------------|---------|
| Arithmetic | `+` `-` `*` `/` `%`                | int×int or float×float (`%` int only) | same |
| Comparison | `<` `<=` `>` `>=`                  | int×int or float×float | `bool` |
| Equality   | `==` `!=`                          | any matching type    | `bool`  |
| Logical    | `&&` `\|\|` `!`                    | `bool`               | `bool`  |
| Unary      | `-` (negate), `!` (not)            | number / bool        | same    |

Precedence (low → high): `||`, `&&`, equality, comparison, `+ -`, `* / %`,
unary. Use parentheses to override.

## Built-in functions

These map one-to-one onto AuroraOS's API through the runtime shim
([`../runtime`](../runtime)). All drawing targets the **top screen**
(400×240, origin top-left, 8×8 font).

| Built-in                          | Does                                             | AuroraOS call     |
|-----------------------------------|--------------------------------------------------|-------------------|
| `print(text, x, y, color)`        | draw `text` at pixel `(x, y)`                     | `draw_string`     |
| `clear(color)`                    | fill the screen; remembers `color` as text bg     | `clear_screen`    |
| `fill_rect(x, y, w, h, color)`    | fill a rectangle (use it to draw bands/shapes)    | `draw_filled_rect`|
| `wait_key(button)`                | block until `button` is newly pressed             | `get_keys_down`   |
| `delay(cycles)`                   | busy-wait ~`cycles` iterations                     | `delay`           |

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

You cannot declare a variable or function that reuses a predefined name.

## Comments

```auric
// line comment
/* block comment */
```

## What v0.1 deliberately leaves out

Arrays, structs, pointers, global variables, `for` loops, `break`/`continue`,
string operations, and float↔int conversion. The scope is intentionally minimal;
these are candidates for later versions.
