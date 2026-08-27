"""Tests for the Auric lexer."""
import unittest

from compiler.errors import AuricError
from compiler.lexer import Lexer, T


def kinds(src: str) -> list[T]:
    return [t.kind for t in Lexer(src).tokenize()]


class LexerTest(unittest.TestCase):
    def test_keywords_and_idents(self):
        toks = Lexer("fn main let x while if else return true false").tokenize()
        self.assertEqual(
            [t.kind for t in toks[:9]],
            [T.FN, T.IDENT, T.LET, T.IDENT, T.WHILE, T.IF, T.ELSE, T.RETURN, T.TRUE],
        )
        self.assertEqual(toks[1].value, "main")

    def test_types_are_keywords(self):
        self.assertEqual(
            kinds("int float bool string")[:4],
            [T.KW_INT, T.KW_FLOAT, T.KW_BOOL, T.KW_STRING],
        )

    def test_numbers(self):
        toks = Lexer("0 42 3.14 0xFF").tokenize()
        self.assertEqual(toks[0].kind, T.INT)
        self.assertEqual(toks[1].value, "42")
        self.assertEqual(toks[2].kind, T.FLOAT)
        self.assertEqual(toks[2].value, "3.14")
        self.assertEqual(toks[3].kind, T.INT)
        self.assertEqual(toks[3].value, "255")  # 0xFF decoded

    def test_two_char_operators(self):
        self.assertEqual(
            kinds("-> == != <= >= && ||")[:7],
            [T.ARROW, T.EQ, T.NE, T.LE, T.GE, T.AND, T.OR],
        )

    def test_single_char_operators(self):
        self.assertEqual(
            kinds("+ - * / % < > ! = ;")[:10],
            [T.PLUS, T.MINUS, T.STAR, T.SLASH, T.PERCENT, T.LT, T.GT,
             T.NOT, T.ASSIGN, T.SEMI],
        )

    def test_string_with_escapes(self):
        toks = Lexer(r'"a\nb\t\"c\""').tokenize()
        self.assertEqual(toks[0].kind, T.STRING)
        self.assertEqual(toks[0].value, 'a\nb\t"c"')

    def test_line_and_block_comments(self):
        src = "fn // line comment\n main /* block\ncomment */ ("
        self.assertEqual(kinds(src)[:3], [T.FN, T.IDENT, T.LPAREN])

    def test_line_col_tracking(self):
        toks = Lexer("fn\n  main").tokenize()
        self.assertEqual((toks[1].line, toks[1].col), (2, 3))

    def test_unterminated_string_errors(self):
        with self.assertRaises(AuricError) as cm:
            Lexer('"oops').tokenize()
        self.assertEqual(cm.exception.stage, "lex")

    def test_unterminated_block_comment_errors(self):
        with self.assertRaises(AuricError):
            Lexer("/* never ends").tokenize()

    def test_unexpected_char_errors(self):
        with self.assertRaises(AuricError):
            Lexer("@").tokenize()

    def test_eof_token(self):
        self.assertEqual(kinds("")[-1], T.EOF)


if __name__ == "__main__":
    unittest.main()
