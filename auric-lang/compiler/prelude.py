"""The Auric v0.1 prelude: built-in functions and predefined constants.

Kept in one place so the type checker and the code generator agree on names,
signatures, and the C spellings they lower to. Everything here is backed by the
runtime shim in ../runtime/auric_runtime.{c,h}, which in turn calls AuroraOS's
own draw_string / clear_screen / get_keys_down / delay.
"""
from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True)
class Builtin:
    params: tuple[str, ...]   # Auric parameter types
    ret: str                  # Auric return type ("void" for statements)
    c_name: str               # function to emit in generated C


# Built-in calls. Each maps directly onto one AuroraOS API call via the shim.
#   print(text, x, y, color)  -> draw_string(top screen, ...)
#   clear(color)              -> clear_screen(top screen, ...)
#   wait_key(button)          -> loop on get_keys_down()
#   delay(cycles)             -> delay()
BUILTINS: dict[str, Builtin] = {
    "print":     Builtin(("string", "int", "int", "int"), "void", "aur_print"),
    "clear":     Builtin(("int",), "void", "aur_clear"),
    "fill_rect": Builtin(("int", "int", "int", "int", "int"), "void",
                         "aur_fill_rect"),
    "wait_key":  Builtin(("int",), "void", "aur_wait_key"),
    "delay":     Builtin(("int",), "void", "aur_delay"),
}


@dataclass(frozen=True)
class Const:
    type: str      # Auric type of the constant
    c_expr: str    # C expression emitted for it (a macro from auric_runtime.h)


# Predefined constants seeded into the global scope. Colours are packed 0xRRGGBB
# ints (the shim unpacks them into AuroraOS `Color`s); buttons mirror the
# BUTTON_* bits in ../../include/aurora.h.
CONSTANTS: dict[str, Const] = {
    # colours (macros defined in auric_runtime.h)
    "BLACK":     Const("int", "AUR_BLACK"),
    "WHITE":     Const("int", "AUR_WHITE"),
    "RED":       Const("int", "AUR_RED"),
    "GREEN":     Const("int", "AUR_GREEN"),
    "BLUE":      Const("int", "AUR_BLUE"),
    "CYAN":      Const("int", "AUR_CYAN"),
    "MAGENTA":   Const("int", "AUR_MAGENTA"),
    "YELLOW":    Const("int", "AUR_YELLOW"),
    "ORANGE":    Const("int", "AUR_ORANGE"),
    "AURORA":    Const("int", "AUR_AURORA"),
    "GRAY":      Const("int", "AUR_GRAY"),
    "DARK_GRAY": Const("int", "AUR_DARK_GRAY"),
    # buttons
    "KEY_A":      Const("int", "AUR_KEY_A"),
    "KEY_B":      Const("int", "AUR_KEY_B"),
    "KEY_SELECT": Const("int", "AUR_KEY_SELECT"),
    "KEY_START":  Const("int", "AUR_KEY_START"),
    "KEY_RIGHT":  Const("int", "AUR_KEY_RIGHT"),
    "KEY_LEFT":   Const("int", "AUR_KEY_LEFT"),
    "KEY_UP":     Const("int", "AUR_KEY_UP"),
    "KEY_DOWN":   Const("int", "AUR_KEY_DOWN"),
    "KEY_R":      Const("int", "AUR_KEY_R"),
    "KEY_L":      Const("int", "AUR_KEY_L"),
}
