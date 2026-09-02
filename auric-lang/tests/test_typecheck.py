"""Tests for the Auric type checker / semantic pass."""
import unittest

from compiler import check_source
from compiler.errors import AuricError


def ok(src: str) -> None:
    check_source(src)


def bad(src: str) -> AuricError:
    try:
        check_source(src)
    except AuricError as e:
        return e
    raise AssertionError("expected a type error, but checking succeeded")


class TypeCheckTest(unittest.TestCase):
    def test_minimal_main(self):
        ok("fn main() {}")

    def test_arithmetic_and_locals(self):
        ok("fn main() { let x: int = 1 + 2 * 3; let y = x - 4; }")

    def test_float_arithmetic(self):
        ok("fn main() { let x: float = 1.5 + 2.5; }")

    def test_bool_logic(self):
        ok("fn main() { let b: bool = true && (1 < 2) || !false; }")

    def test_function_call_and_return(self):
        ok("fn sq(n: int) -> int { return n * n; } "
           "fn main() { let x = sq(5); }")

    def test_if_while_conditions(self):
        ok("fn main() { let i = 0; while i < 3 { if i == 1 {} i = i + 1; } }")

    def test_builtins(self):
        ok('fn main() { clear(BLACK); print("hi", 1, 2, WHITE); '
           'wait_key(KEY_A); delay(1000); }')

    def test_fill_rect_builtin(self):
        ok("fn main() { fill_rect(0, 0, 400, 240, RED); }")

    def test_fill_rect_wrong_arg_count(self):
        bad("fn main() { fill_rect(0, 0, 400, RED); }")

    def test_fill_rect_wrong_arg_type(self):
        bad('fn main() { fill_rect(0, 0, 400, 240, "red"); }')

    def test_string_equality(self):
        ok('fn main() { let b = "a" == "b"; }')

    def test_no_main(self):
        self.assertIn("main", bad("fn f() {}").message)

    def test_main_with_params(self):
        bad("fn main(x: int) {}")

    def test_main_returning_value(self):
        bad("fn main() -> int { return 1; }")

    def test_undeclared_variable(self):
        self.assertIn("undeclared", bad("fn main() { let x = y; }").message)

    def test_type_mismatch_in_let(self):
        bad('fn main() { let x: int = "hi"; }')

    def test_assign_type_mismatch(self):
        bad("fn main() { let x: int = 0; x = true; }")

    def test_assign_undeclared(self):
        bad("fn main() { x = 1; }")

    def test_mixed_numeric_arithmetic(self):
        bad("fn main() { let x = 1 + 2.0; }")

    def test_modulo_on_float(self):
        bad("fn main() { let x = 1.0 % 2.0; }")

    def test_non_bool_condition(self):
        bad("fn main() { if 1 {} }")

    def test_logic_on_non_bool(self):
        bad("fn main() { let b = 1 && 2; }")

    def test_compare_across_types(self):
        bad('fn main() { let b = 1 < "x"; }')

    def test_wrong_arg_count(self):
        bad('fn main() { clear(); }')

    def test_wrong_arg_type(self):
        bad('fn main() { clear("black"); }')

    def test_call_unknown_function(self):
        bad("fn main() { nope(); }")

    def test_void_call_as_value(self):
        bad("fn main() { let x = delay(1); }")

    def test_redeclare_in_scope(self):
        bad("fn main() { let x = 1; let x = 2; }")

    def test_cannot_shadow_prelude_constant(self):
        bad("fn main() { let WHITE = 1; }")

    def test_duplicate_function(self):
        bad("fn f() {} fn f() {} fn main() {}")

    def test_return_value_from_void(self):
        bad("fn f() { return 1; } fn main() {}")

    def test_return_wrong_type(self):
        bad("fn f() -> int { return true; } fn main() {}")

    def test_missing_return_value(self):
        bad("fn f() -> int { return; } fn main() {}")

    def test_scope_is_local_to_block(self):
        # `t` declared inside the while body is not visible afterwards.
        bad("fn main() { while true { let t = 1; } t = 2; }")


if __name__ == "__main__":
    unittest.main()
