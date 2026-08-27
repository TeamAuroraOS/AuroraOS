"""Tests for the Auric parser."""
import unittest

from compiler import ast, parse_source
from compiler.errors import AuricError


class ParserTest(unittest.TestCase):
    def test_empty_function(self):
        prog = parse_source("fn main() {}")
        self.assertEqual(len(prog.functions), 1)
        fn = prog.functions[0]
        self.assertEqual(fn.name, "main")
        self.assertEqual(fn.params, [])
        self.assertEqual(fn.ret_type, "void")
        self.assertEqual(fn.body.statements, [])

    def test_params_and_return_type(self):
        prog = parse_source("fn add(a: int, b: int) -> int { return a + b; }")
        fn = prog.functions[0]
        self.assertEqual([(p.name, p.type) for p in fn.params],
                         [("a", "int"), ("b", "int")])
        self.assertEqual(fn.ret_type, "int")
        ret = fn.body.statements[0]
        self.assertIsInstance(ret, ast.ReturnStmt)
        self.assertIsInstance(ret.value, ast.BinaryExpr)
        self.assertEqual(ret.value.op, "+")

    def test_let_and_assign(self):
        prog = parse_source("fn main() { let x: int = 1; x = 2; }")
        stmts = prog.functions[0].body.statements
        self.assertIsInstance(stmts[0], ast.LetStmt)
        self.assertEqual(stmts[0].decl_type, "int")
        self.assertIsInstance(stmts[1], ast.AssignStmt)
        self.assertEqual(stmts[1].name, "x")

    def test_let_infers_type(self):
        prog = parse_source("fn main() { let x = 1; }")
        self.assertIsNone(prog.functions[0].body.statements[0].decl_type)

    def test_if_else_chain(self):
        prog = parse_source(
            "fn main() { if true {} else if false {} else {} }")
        iff = prog.functions[0].body.statements[0]
        self.assertIsInstance(iff, ast.IfStmt)
        self.assertIsInstance(iff.else_block, ast.IfStmt)
        self.assertIsInstance(iff.else_block.else_block, ast.Block)

    def test_while(self):
        prog = parse_source("fn main() { while x < 3 { x = x + 1; } }")
        w = prog.functions[0].body.statements[0]
        self.assertIsInstance(w, ast.WhileStmt)
        self.assertEqual(w.cond.op, "<")

    def test_call_expression(self):
        prog = parse_source('fn main() { print("hi", 1, 2, 3); }')
        call = prog.functions[0].body.statements[0].expr
        self.assertIsInstance(call, ast.CallExpr)
        self.assertEqual(call.callee, "print")
        self.assertEqual(len(call.args), 4)

    def test_precedence(self):
        # 1 + 2 * 3  parses as  1 + (2 * 3)
        prog = parse_source("fn main() { let x = 1 + 2 * 3; }")
        expr = prog.functions[0].body.statements[0].value
        self.assertEqual(expr.op, "+")
        self.assertIsInstance(expr.right, ast.BinaryExpr)
        self.assertEqual(expr.right.op, "*")

    def test_comparison_below_arithmetic(self):
        # 1 + 2 < 4  parses as  (1 + 2) < 4
        prog = parse_source("fn main() { let b = 1 + 2 < 4; }")
        expr = prog.functions[0].body.statements[0].value
        self.assertEqual(expr.op, "<")
        self.assertEqual(expr.left.op, "+")

    def test_unary(self):
        prog = parse_source("fn main() { let x = -a; let y = !b; }")
        s = prog.functions[0].body.statements
        self.assertEqual(s[0].value.op, "-")
        self.assertEqual(s[1].value.op, "!")

    def test_parenthesized(self):
        prog = parse_source("fn main() { let x = (1 + 2) * 3; }")
        expr = prog.functions[0].body.statements[0].value
        self.assertEqual(expr.op, "*")
        self.assertEqual(expr.left.op, "+")

    def test_missing_semicolon_errors(self):
        with self.assertRaises(AuricError) as cm:
            parse_source("fn main() { let x = 1 }")
        self.assertEqual(cm.exception.stage, "parse")

    def test_top_level_must_be_fn(self):
        with self.assertRaises(AuricError):
            parse_source("let x = 1;")


if __name__ == "__main__":
    unittest.main()
