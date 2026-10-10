"""Built-in functions and predefined constants, shared by the type checker and
the code generator and backed by ../runtime/auric_runtime.{c,h}.
"""
from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True)
class Builtin:
    params: tuple[str, ...]   # Auric parameter types
    ret: str                  # Auric return type ("void" for statements)
    c_name: str               # function to emit in generated C


# Built-in calls, each lowering to one runtime helper.
BUILTINS: dict[str, Builtin] = {
    # Which screen the drawing built-ins draw on: TOP (default) or BOTTOM.
    "screen":    Builtin(("int",), "void", "aur_screen"),
    "print":     Builtin(("string", "int", "int", "int"), "void", "aur_print"),
    "print_int": Builtin(("int", "int", "int", "int"), "void", "aur_print_int"),
    "clear":     Builtin(("int",), "void", "aur_clear"),
    "fill_rect": Builtin(("int", "int", "int", "int", "int"), "void",
                         "aur_fill_rect"),
    "wait_key":  Builtin(("int",), "void", "aur_wait_key"),
    "delay":     Builtin(("int",), "void", "aur_delay"),
    # Polling input, for games that cannot block on a single button.
    "keys_down": Builtin((), "int", "aur_keys_down"),
    "keys_held": Builtin((), "int", "aur_keys_held"),
    # Touch, in bottom-screen pixels; down/up are edges like keys_down.
    "touch_down": Builtin((), "bool", "aur_touch_down"),
    "touch_held": Builtin((), "bool", "aur_touch_held"),
    "touch_up":   Builtin((), "bool", "aur_touch_up"),
    "touch_x":    Builtin((), "int", "aur_touch_x"),
    "touch_y":    Builtin((), "int", "aur_touch_y"),
    "touch_in":   Builtin(("int", "int", "int", "int"), "bool", "aur_touch_in"),
    # Frame control: buffered(true) defers drawing until present().
    "buffered":  Builtin(("bool",), "void", "aur_buffered"),
    "present":   Builtin((), "void", "aur_present"),
    # Pseudo-random integer in [0, n), seeded from the console clock.
    "rand":      Builtin(("int",), "int", "aur_rand"),
    # Milliseconds since startup, from an ARM9 hardware timer.
    "millis":    Builtin((), "int", "aur_millis"),
    # Sound: WAV files read from the SD card, mixed by the ARM11 core.
    "load_sound":  Builtin(("string",), "int", "aur_load_sound"),
    "play_sound":  Builtin(("int",), "void", "aur_play_sound"),
    "play_music":  Builtin(("int",), "void", "aur_play_music"),
    "stop_music":  Builtin((), "void", "aur_stop_music"),
    "stop_sounds": Builtin((), "void", "aur_stop_sounds"),
    # The network: the Wi-Fi network saved in Settings, plain HTTP. Strings
    # these return stay valid until the next request.
    "net_connect": Builtin((), "bool", "aur_net_connect"),
    "net_online":  Builtin((), "bool", "aur_net_online"),
    "net_error":   Builtin((), "string", "aur_net_error"),
    "net_address": Builtin((), "string", "aur_net_address"),
    "http_get":    Builtin(("string",), "int", "aur_http_get"),
    "http_post":   Builtin(("string", "string"), "int", "aur_http_post"),
    "http_param":  Builtin(("string", "string"), "void", "aur_http_param"),
    "http_param_int": Builtin(("string", "int"), "void", "aur_http_param_int"),
    "http_header": Builtin(("string", "string"), "void", "aur_http_header"),
    "http_text":   Builtin((), "string", "aur_http_text"),
    "http_length": Builtin((), "int", "aur_http_length"),
    "http_lines":  Builtin((), "int", "aur_http_lines"),
    "http_line":   Builtin(("int",), "string", "aur_http_line"),
    "http_save":   Builtin(("string",), "bool", "aur_http_save"),
    "http_download": Builtin(("string", "string"), "bool", "aur_http_download"),
    # Values in the last reply, read as JSON, by path: "list.0.name".
    "json_has":    Builtin(("string",), "bool", "aur_json_has"),
    "json_int":    Builtin(("string",), "int", "aur_json_int"),
    "json_float":  Builtin(("string",), "float", "aur_json_float"),
    "json_bool":   Builtin(("string",), "bool", "aur_json_bool"),
    "json_string": Builtin(("string",), "string", "aur_json_string"),
    "json_count":  Builtin(("string",), "int", "aur_json_count"),
    # What "#" stands for in the paths that follow.
    "json_index":  Builtin(("int",), "void", "aur_json_index"),
}


@dataclass(frozen=True)
class Const:
    type: str      # Auric type of the constant
    c_expr: str    # C expression emitted for it (a macro from auric_runtime.h)


# Predefined constants seeded into the global scope. Colours are packed 0xRRGGBB
# ints (the shim unpacks them into AuroraOS `Color`s); buttons mirror the
# BUTTON_* bits in ../../include/aurora.h.
CONSTANTS: dict[str, Const] = {
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
    "TOP":       Const("int", "AUR_TOP"),
    "BOTTOM":    Const("int", "AUR_BOTTOM"),
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
    "KEY_X":      Const("int", "AUR_KEY_X"),
    "KEY_Y":      Const("int", "AUR_KEY_Y"),
}
