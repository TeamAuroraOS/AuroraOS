"""Tests for the Auric C code generator (text-level checks)."""
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


if __name__ == "__main__":
    unittest.main()
