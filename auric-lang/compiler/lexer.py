"""Auric lexer: source text -> token stream.

Hand-written scanner, deliberately small to match the v0.1 language scope. It
tracks 1-based line/column on every token so later stages can report precise
errors. Whitespace and `//` / `/* */` comments are skipped.
"""
from __future__ import annotations

from dataclasses import dataclass
from enum import Enum, auto

from .errors import AuricError


class T(Enum):
    """Token kinds."""
    # literals / names
    INT = auto()
    FLOAT = auto()
    STRING = auto()
    IDENT = auto()
    # keywords
    FN = auto()
    LET = auto()
    IF = auto()
    ELSE = auto()
    WHILE = auto()
    RETURN = auto()
    TRUE = auto()
    FALSE = auto()
    KW_INT = auto()
    KW_FLOAT = auto()
    KW_BOOL = auto()
    KW_STRING = auto()
    # punctuation
    LPAREN = auto()
    RPAREN = auto()
    LBRACE = auto()
    RBRACE = auto()
    COMMA = auto()
    COLON = auto()
    SEMI = auto()
    ARROW = auto()      # ->
    # operators
    ASSIGN = auto()     # =
    PLUS = auto()
    MINUS = auto()
    STAR = auto()
    SLASH = auto()
    PERCENT = auto()
    EQ = auto()         # ==
    NE = auto()         # !=
    LT = auto()
    LE = auto()
    GT = auto()
    GE = auto()
    AND = auto()        # &&
    OR = auto()         # ||
    NOT = auto()        # !
    EOF = auto()


KEYWORDS = {
    "fn": T.FN,
    "let": T.LET,
    "if": T.IF,
    "else": T.ELSE,
    "while": T.WHILE,
    "return": T.RETURN,
    "true": T.TRUE,
    "false": T.FALSE,
    "int": T.KW_INT,
    "float": T.KW_FLOAT,
    "bool": T.KW_BOOL,
    "string": T.KW_STRING,
}


@dataclass
class Token:
    kind: T
    value: str          # raw lexeme (for STRING: the decoded text)
    line: int
    col: int

    def __repr__(self) -> str:  # pragma: no cover - debug aid
        return f"Token({self.kind.name}, {self.value!r}, {self.line}:{self.col})"


class Lexer:
    def __init__(self, src: str) -> None:
        self.src = src
        self.i = 0
        self.line = 1
        self.col = 1

    # -- low-level cursor helpers ------------------------------------------
    def _peek(self, ahead: int = 0) -> str:
        j = self.i + ahead
        return self.src[j] if j < len(self.src) else ""

    def _advance(self) -> str:
        c = self.src[self.i]
        self.i += 1
        if c == "\n":
            self.line += 1
            self.col = 1
        else:
            self.col += 1
        return c

    def _err(self, msg: str) -> AuricError:
        return AuricError(msg, self.line, self.col, stage="lex")

    # -- whitespace + comments ---------------------------------------------
    def _skip_trivia(self) -> None:
        while self.i < len(self.src):
            c = self._peek()
            if c in " \t\r\n":
                self._advance()
            elif c == "/" and self._peek(1) == "/":
                while self.i < len(self.src) and self._peek() != "\n":
                    self._advance()
            elif c == "/" and self._peek(1) == "*":
                self._advance()
                self._advance()
                while self.i < len(self.src) and not (
                        self._peek() == "*" and self._peek(1) == "/"):
                    self._advance()
                if self.i >= len(self.src):
                    raise self._err("unterminated block comment")
                self._advance()  # *
                self._advance()  # /
            else:
                break

    # -- token producers ----------------------------------------------------
    def _number(self) -> Token:
        line, col = self.line, self.col
        start = self.i
        while self._peek().isdigit():
            self._advance()
        is_float = False
        if self._peek() == "." and self._peek(1).isdigit():
            is_float = True
            self._advance()
            while self._peek().isdigit():
                self._advance()
        text = self.src[start:self.i]
        # 0x.. hex integers, matching aos_pack.py's int(v, 0) convenience.
        if not is_float and text == "0" and self._peek() in ("x", "X"):
            self._advance()
            hstart = self.i
            while self._peek() and self._peek() in "0123456789abcdefABCDEF":
                self._advance()
            if self.i == hstart:
                raise self._err("malformed hex literal")
            return Token(T.INT, str(int(self.src[hstart:self.i], 16)), line, col)
        return Token(T.FLOAT if is_float else T.INT, text, line, col)

    def _string(self) -> Token:
        line, col = self.line, self.col
        self._advance()  # opening quote
        out: list[str] = []
        while True:
            if self.i >= len(self.src):
                raise self._err("unterminated string literal")
            c = self._advance()
            if c == '"':
                break
            if c == "\\":
                esc = self._advance()
                out.append({"n": "\n", "t": "\t", "r": "\r", "\\": "\\",
                            '"': '"', "0": "\0"}.get(esc, esc))
            elif c == "\n":
                raise self._err("newline in string literal")
            else:
                out.append(c)
        return Token(T.STRING, "".join(out), line, col)

    def _ident(self) -> Token:
        line, col = self.line, self.col
        start = self.i
        while self._peek().isalnum() or self._peek() == "_":
            self._advance()
        text = self.src[start:self.i]
        return Token(KEYWORDS.get(text, T.IDENT), text, line, col)

    # -- main entry ---------------------------------------------------------
    def tokenize(self) -> list[Token]:
        tokens: list[Token] = []
        # Two-char operators checked before their single-char prefixes.
        two = {
            "->": T.ARROW, "==": T.EQ, "!=": T.NE, "<=": T.LE, ">=": T.GE,
            "&&": T.AND, "||": T.OR,
        }
        one = {
            "(": T.LPAREN, ")": T.RPAREN, "{": T.LBRACE, "}": T.RBRACE,
            ",": T.COMMA, ":": T.COLON, ";": T.SEMI, "=": T.ASSIGN,
            "+": T.PLUS, "-": T.MINUS, "*": T.STAR, "/": T.SLASH,
            "%": T.PERCENT, "<": T.LT, ">": T.GT, "!": T.NOT,
        }
        while True:
            self._skip_trivia()
            if self.i >= len(self.src):
                tokens.append(Token(T.EOF, "", self.line, self.col))
                return tokens
            c = self._peek()
            if c.isdigit():
                tokens.append(self._number())
            elif c == '"':
                tokens.append(self._string())
            elif c.isalpha() or c == "_":
                tokens.append(self._ident())
            else:
                pair = c + self._peek(1)
                if pair in two:
                    line, col = self.line, self.col
                    self._advance()
                    self._advance()
                    tokens.append(Token(two[pair], pair, line, col))
                elif c in one:
                    line, col = self.line, self.col
                    self._advance()
                    tokens.append(Token(one[c], c, line, col))
                else:
                    raise self._err(f"unexpected character {c!r}")
