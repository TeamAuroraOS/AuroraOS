import unittest

from compiler import compile_to_c


class CodeGenTest(unittest.TestCase):
    def test_includes_runtime_header(self):
        c = compile_to_c("fn main() {}")
        self.assertIn('#include "auric_runtime.h"', c)

    def test_main_becomes_au_main(self):
        c = compile_to_c("fn main() {}")
        self.assertIn("void au_main(void) {", c)

    def test_forward_declarations_emitted(self):
        c = compile_to_c("fn helper() -> int { return 1; } fn main() {}")
        self.assertIn("int au_helper(void);", c)
        self.assertIn("void au_main(void);", c)

    def test_user_names_are_mangled(self):
        c = compile_to_c("fn sq(n: int) -> int { return n * n; } fn main() {}")
        self.assertIn("int au_sq(int au_n)", c)
        self.assertIn("(au_n * au_n)", c)

    def test_builtins_lower_to_shim(self):
        c = compile_to_c('fn main() { print("hi", 1, 2, WHITE); }')
        self.assertIn('aur_print("hi", 1, 2, AUR_WHITE)', c)

    def test_constants_lower_to_macros(self):
        c = compile_to_c("fn main() { clear(BLACK); wait_key(KEY_B); }")
        self.assertIn("aur_clear(AUR_BLACK)", c)
        self.assertIn("aur_wait_key(AUR_KEY_B)", c)

    def test_fill_rect_lowers_to_shim(self):
        c = compile_to_c("fn main() { fill_rect(0, 10, 400, 34, RED); }")
        self.assertIn("aur_fill_rect(0, 10, 400, 34, AUR_RED)", c)

    def test_bool_type_and_literals(self):
        c = compile_to_c("fn main() { let b: bool = true; let c: bool = false; }")
        self.assertIn("int au_b = 1;", c)
        self.assertIn("int au_c = 0;", c)

    def test_string_type(self):
        c = compile_to_c('fn main() { let s: string = "x"; }')
        self.assertIn('const char * au_s = "x";', c)

    def test_float_literal_suffixed(self):
        c = compile_to_c("fn main() { let f: float = 1.5; }")
        self.assertIn("float au_f = 1.5f;", c)

    def test_if_else(self):
        c = compile_to_c("fn main() { if true { clear(BLACK); } else { clear(WHITE); } }")
        self.assertIn("if (1) {", c)
        self.assertIn("} else {", c)

    def test_while(self):
        c = compile_to_c("fn main() { let i = 0; while i < 3 { i = i + 1; } }")
        self.assertIn("while ((au_i < 3)) {", c)
        self.assertIn("au_i = (au_i + 1);", c)

    def test_string_escaping(self):
        c = compile_to_c(r'fn main() { print("a\"b\n", 0, 0, WHITE); }')
        self.assertIn(r'"a\"b\n"', c)


class SoundCodegenTest(unittest.TestCase):
    def test_sound_builtins_lower_to_shim(self):
        c = compile_to_c('fn main() { let s = load_sound("SND/A.WAV"); '
                         'play_sound(s); play_music(s); stop_music(); stop_sounds(); }')
        self.assertIn('aur_load_sound("SND/A.WAV")', c)
        self.assertIn("aur_play_sound(au_s)", c)
        self.assertIn("aur_play_music(au_s)", c)
        self.assertIn("aur_stop_music()", c)
        self.assertIn("aur_stop_sounds()", c)


class ScreenTouchCodegenTest(unittest.TestCase):
    def test_screen_lowers_to_shim(self):
        c = compile_to_c("fn main() { screen(BOTTOM); screen(TOP); }")
        self.assertIn("aur_screen(AUR_BOTTOM)", c)
        self.assertIn("aur_screen(AUR_TOP)", c)

    def test_touch_builtins_lower_to_shim(self):
        c = compile_to_c("fn main() { if touch_down() && touch_in(1, 2, 3, 4) "
                         "{ let x = touch_x(); let y = touch_y(); } "
                         "let h = touch_held(); let u = touch_up(); }")
        self.assertIn("(aur_touch_down() && aur_touch_in(1, 2, 3, 4))", c)
        self.assertIn("int au_x = aur_touch_x();", c)
        self.assertIn("int au_y = aur_touch_y();", c)
        self.assertIn("int au_h = aur_touch_held();", c)
        self.assertIn("int au_u = aur_touch_up();", c)


class NetCodegenTest(unittest.TestCase):
    def test_net_builtins_lower_to_shim(self):
        c = compile_to_c('fn main() { if net_connect() { '
                         'http_param_int("score", 5); '
                         'let st = http_post("http://x/p", ""); '
                         'let name: string = json_string("a.0.b"); '
                         'let t: float = json_float("t"); } }')
        self.assertIn("if (aur_net_connect()) {", c)
        self.assertIn('aur_http_param_int("score", 5);', c)
        self.assertIn('int au_st = aur_http_post("http://x/p", "");', c)
        self.assertIn('const char * au_name = aur_json_string("a.0.b");', c)
        self.assertIn('float au_t = aur_json_float("t");', c)

    def test_string_equality_compares_text(self):
        c = compile_to_c('fn main() { let s = net_error(); '
                         'let a = s == "x"; let b = s != "y"; let n = 1 == 2; }')
        self.assertIn('int au_a = (aur_str_eq(au_s, "x"));', c)
        self.assertIn('int au_b = (!aur_str_eq(au_s, "y"));', c)
        self.assertIn("int au_n = (1 == 2);", c)

    def test_x_and_y_buttons(self):
        c = compile_to_c("fn main() { wait_key(KEY_X); wait_key(KEY_Y); }")
        self.assertIn("aur_wait_key(AUR_KEY_X)", c)
        self.assertIn("aur_wait_key(AUR_KEY_Y)", c)


if __name__ == "__main__":
    unittest.main()
