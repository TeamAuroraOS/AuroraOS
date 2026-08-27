"""Auric parser: token stream -> AST.

A straightforward recursive-descent parser with a conventional operator
precedence climb for expressions. It only ever looks one token ahead.
"""
from __future__ import annotations

from . import ast
from .errors import AuricError
from .lexer import T, Token

TYPE_KEYWORDS = {
    T.KW_INT: "int",
    T.KW_FLOAT: "float",
    T.KW_BOOL: "bool",
    T.KW_STRING: "string",
}


class Parser:
    def __init__(self, tokens: list[Token]) -> None:
        self.tokens = tokens
        self.pos = 0

    # -- token helpers ------------------------------------------------------
    @property
    def _cur(self) -> Token:
        return self.tokens[self.pos]

    def _at(self, kind: T) -> bool:
        return self._cur.kind == kind

    def _advance(self) -> Token:
        tok = self.tokens[self.pos]
        if tok.kind != T.EOF:
            self.pos += 1
        return tok

    def _expect(self, kind: T, what: str) -> Token:
        if not self._at(kind):
            raise AuricError(
                f"expected {what}, found {self._describe(self._cur)}",
                self._cur.line, self._cur.col, stage="parse")
        return self._advance()

    @staticmethod
    def _describe(tok: Token) -> str:
        if tok.kind == T.EOF:
            return "end of file"
        return f"{tok.value!r}"

    # -- entry --------------------------------------------------------------
    def parse(self) -> ast.Program:
        prog = ast.Program()
        while not self._at(T.EOF):
            if self._at(T.FN):
                prog.functions.append(self._fn_decl())
            else:
                raise AuricError(
                    f"expected a function declaration, found "
                    f"{self._describe(self._cur)}",
                    self._cur.line, self._cur.col, stage="parse")
        return prog

    # -- declarations -------------------------------------------------------
    def _type(self) -> str:
        tok = self._cur
        if tok.kind in TYPE_KEYWORDS:
            self._advance()
            return TYPE_KEYWORDS[tok.kind]
        raise AuricError(f"expected a type, found {self._describe(tok)}",
                         tok.line, tok.col, stage="parse")

    def _fn_decl(self) -> ast.FnDecl:
        kw = self._expect(T.FN, "'fn'")
        name = self._expect(T.IDENT, "function name")
        self._expect(T.LPAREN, "'('")
        params: list[ast.Param] = []
        if not self._at(T.RPAREN):
            params.append(self._param())
            while self._at(T.COMMA):
                self._advance()
                params.append(self._param())
        self._expect(T.RPAREN, "')'")
        ret_type = "void"
        if self._at(T.ARROW):
            self._advance()
            ret_type = self._type()
        body = self._block()
        return ast.FnDecl(name.value, params, ret_type, body, line=kw.line)

    def _param(self) -> ast.Param:
        name = self._expect(T.IDENT, "parameter name")
        self._expect(T.COLON, "':'")
        ptype = self._type()
        return ast.Param(name.value, ptype, name.line)

    # -- statements ---------------------------------------------------------
    def _block(self) -> ast.Block:
        lb = self._expect(T.LBRACE, "'{'")
        stmts: list[ast.Stmt] = []
        while not self._at(T.RBRACE) and not self._at(T.EOF):
            stmts.append(self._stmt())
        self._expect(T.RBRACE, "'}'")
        return ast.Block(stmts, line=lb.line)

    def _stmt(self) -> ast.Stmt:
        if self._at(T.LET):
            return self._let_stmt()
        if self._at(T.IF):
            return self._if_stmt()
        if self._at(T.WHILE):
            return self._while_stmt()
        if self._at(T.RETURN):
            return self._return_stmt()
        # assignment (IDENT '=' ...) or a bare expression statement
        if self._at(T.IDENT) and self.tokens[self.pos + 1].kind == T.ASSIGN:
            return self._assign_stmt()
        return self._expr_stmt()

    def _let_stmt(self) -> ast.LetStmt:
        kw = self._advance()
        name = self._expect(T.IDENT, "variable name")
        decl_type: str | None = None
        if self._at(T.COLON):
            self._advance()
            decl_type = self._type()
        self._expect(T.ASSIGN, "'='")
        value = self._expr()
        self._expect(T.SEMI, "';'")
        return ast.LetStmt(name.value, decl_type, value, line=kw.line)

    def _assign_stmt(self) -> ast.AssignStmt:
        name = self._advance()
        self._expect(T.ASSIGN, "'='")
        value = self._expr()
        self._expect(T.SEMI, "';'")
        return ast.AssignStmt(name.value, value, line=name.line)

    def _if_stmt(self) -> ast.IfStmt:
        kw = self._advance()
        cond = self._expr()
        then_block = self._block()
        else_block: ast.Block | ast.IfStmt | None = None
        if self._at(T.ELSE):
            self._advance()
            else_block = self._if_stmt() if self._at(T.IF) else self._block()
        return ast.IfStmt(cond, then_block, else_block, line=kw.line)

    def _while_stmt(self) -> ast.WhileStmt:
        kw = self._advance()
        cond = self._expr()
        body = self._block()
        return ast.WhileStmt(cond, body, line=kw.line)

    def _return_stmt(self) -> ast.ReturnStmt:
        kw = self._advance()
        value: ast.Expr | None = None
        if not self._at(T.SEMI):
            value = self._expr()
        self._expect(T.SEMI, "';'")
        return ast.ReturnStmt(value, line=kw.line)

    def _expr_stmt(self) -> ast.ExprStmt:
        expr = self._expr()
        self._expect(T.SEMI, "';'")
        return ast.ExprStmt(expr, line=expr.line)

    # -- expressions (precedence climbing) ---------------------------------
    # Each tier parses the next-higher tier, then folds left-associatively.
    _BINARY_TIERS: list[dict[T, str]] = [
        {T.OR: "||"},
        {T.AND: "&&"},
        {T.EQ: "==", T.NE: "!="},
        {T.LT: "<", T.LE: "<=", T.GT: ">", T.GE: ">="},
        {T.PLUS: "+", T.MINUS: "-"},
        {T.STAR: "*", T.SLASH: "/", T.PERCENT: "%"},
    ]

    def _expr(self) -> ast.Expr:
        return self._binary(0)

    def _binary(self, tier: int) -> ast.Expr:
        if tier >= len(self._BINARY_TIERS):
            return self._unary()
        ops = self._BINARY_TIERS[tier]
        left = self._binary(tier + 1)
        while self._cur.kind in ops:
            op_tok = self._advance()
            right = self._binary(tier + 1)
            left = ast.BinaryExpr(ops[op_tok.kind], left, right, line=op_tok.line)
        return left

    def _unary(self) -> ast.Expr:
        if self._at(T.NOT) or self._at(T.MINUS):
            op_tok = self._advance()
            operand = self._unary()
            return ast.UnaryExpr(op_tok.value, operand, line=op_tok.line)
        return self._primary()

    def _primary(self) -> ast.Expr:
        tok = self._cur
        if tok.kind == T.INT:
            self._advance()
            return ast.IntLit(int(tok.value), line=tok.line)
        if tok.kind == T.FLOAT:
            self._advance()
            return ast.FloatLit(float(tok.value), line=tok.line)
        if tok.kind == T.STRING:
            self._advance()
            return ast.StringLit(tok.value, line=tok.line)
        if tok.kind in (T.TRUE, T.FALSE):
            self._advance()
            return ast.BoolLit(tok.kind == T.TRUE, line=tok.line)
        if tok.kind == T.LPAREN:
            self._advance()
            inner = self._expr()
            self._expect(T.RPAREN, "')'")
            return inner
        if tok.kind == T.IDENT:
            self._advance()
            if self._at(T.LPAREN):
                return self._call(tok.value, tok.line)
            return ast.VarExpr(tok.value, line=tok.line)
        raise AuricError(f"expected an expression, found {self._describe(tok)}",
                         tok.line, tok.col, stage="parse")

    def _call(self, callee: str, line: int) -> ast.CallExpr:
        self._expect(T.LPAREN, "'('")
        args: list[ast.Expr] = []
        if not self._at(T.RPAREN):
            args.append(self._expr())
            while self._at(T.COMMA):
                self._advance()
                args.append(self._expr())
        self._expect(T.RPAREN, "')'")
        return ast.CallExpr(callee, args, line=line)
