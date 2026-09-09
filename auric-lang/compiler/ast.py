"""Auric AST node definitions.

Plain dataclasses produced by the parser and consumed by the type checker and
code generator. Expression nodes gain a `.type` attribute during type checking
(one of "int", "float", "bool", "string", or "void").
"""
from __future__ import annotations

from dataclasses import dataclass, field


@dataclass
class Expr:
    line: int = field(default=0, kw_only=True)
    # Filled in by the type checker.
    type: str | None = field(default=None, kw_only=True, compare=False)


@dataclass
class IntLit(Expr):
    value: int = 0


@dataclass
class FloatLit(Expr):
    value: float = 0.0


@dataclass
class BoolLit(Expr):
    value: bool = False


@dataclass
class StringLit(Expr):
    value: str = ""


@dataclass
class VarExpr(Expr):
    name: str = ""


@dataclass
class UnaryExpr(Expr):
    op: str = ""
    operand: Expr | None = None


@dataclass
class BinaryExpr(Expr):
    op: str = ""
    left: Expr | None = None
    right: Expr | None = None


@dataclass
class CallExpr(Expr):
    callee: str = ""
    args: list[Expr] = field(default_factory=list)


@dataclass
class IndexExpr(Expr):
    """`name[index]`: reading one element of an array variable."""
    name: str = ""
    index: Expr | None = None


@dataclass
class Stmt:
    line: int = field(default=0, kw_only=True)


@dataclass
class LetStmt(Stmt):
    name: str = ""
    decl_type: str | None = None   # explicit annotation, or None to infer
    value: Expr | None = None
    # Element count for an array declaration (`let a: int[8];`), else None.
    # Arrays have no initializer: they are zero-filled.
    array_size: int | None = None


@dataclass
class AssignStmt(Stmt):
    name: str = ""
    value: Expr | None = None


@dataclass
class IndexAssignStmt(Stmt):
    """`name[index] = value;`"""
    target: IndexExpr | None = None
    value: Expr | None = None


@dataclass
class BreakStmt(Stmt):
    pass


@dataclass
class ContinueStmt(Stmt):
    pass


@dataclass
class IfStmt(Stmt):
    cond: Expr | None = None
    then_block: "Block | None" = None
    else_block: "Block | IfStmt | None" = None


@dataclass
class WhileStmt(Stmt):
    cond: Expr | None = None
    body: "Block | None" = None


@dataclass
class ReturnStmt(Stmt):
    value: Expr | None = None


@dataclass
class ExprStmt(Stmt):
    expr: Expr | None = None


@dataclass
class Block(Stmt):
    statements: list[Stmt] = field(default_factory=list)


# --- declarations --------------------------------------------------------
@dataclass
class Param:
    name: str
    type: str
    line: int = 0


@dataclass
class FnDecl:
    name: str
    params: list[Param]
    ret_type: str          # "void" if none written
    body: Block
    line: int = 0


@dataclass
class Program:
    functions: list[FnDecl] = field(default_factory=list)
    # Module-level `let` declarations, visible to every function.
    globals: list[LetStmt] = field(default_factory=list)
