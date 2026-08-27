"""Auric semantic pass: scope resolution and static type checking.

Walks the AST, annotates every expression with a `.type`, and rejects programs
that are ill-typed, use undeclared names, or misuse `void`. Requires a
`fn main()` entry point. Raises AuricError on the first problem found.
"""
from __future__ import annotations

from . import ast
from .errors import AuricError
from .prelude import BUILTINS, CONSTANTS

NUMERIC = {"int", "float"}
ORDERED = {"int", "float"}            # types that support < <= > >=
EQUATABLE = {"int", "float", "bool", "string"}


class Scope:
    """A lexical scope mapping names to Auric types."""

    def __init__(self, parent: "Scope | None" = None) -> None:
        self.parent = parent
        self.vars: dict[str, str] = {}

    def declare(self, name: str, type_: str, line: int) -> None:
        if name in CONSTANTS or name in BUILTINS:
            raise AuricError(f"'{name}' is a predefined name and cannot be "
                             f"used as a variable", line, stage="type")
        if name in self.vars:
            raise AuricError(f"'{name}' is already declared in this scope",
                             line, stage="type")
        self.vars[name] = type_

    def lookup(self, name: str) -> str | None:
        scope: Scope | None = self
        while scope is not None:
            if name in scope.vars:
                return scope.vars[name]
            scope = scope.parent
        return None


class TypeChecker:
    def __init__(self, program: ast.Program) -> None:
        self.program = program
        self.functions: dict[str, ast.FnDecl] = {}
        self.current_ret: str = "void"

    # -- entry --------------------------------------------------------------
    def check(self) -> None:
        for fn in self.program.functions:
            if fn.name in self.functions:
                raise AuricError(f"function '{fn.name}' is defined more than once",
                                 fn.line, stage="type")
            if fn.name in BUILTINS:
                raise AuricError(f"'{fn.name}' is a built-in and cannot be "
                                 f"redefined", fn.line, stage="type")
            self.functions[fn.name] = fn

        main = self.functions.get("main")
        if main is None:
            raise AuricError("program has no 'fn main()' entry point", stage="type")
        if main.params:
            raise AuricError("'fn main' must take no parameters", main.line,
                             stage="type")
        if main.ret_type != "void":
            raise AuricError("'fn main' must not return a value", main.line,
                             stage="type")

        for fn in self.program.functions:
            self._check_fn(fn)

    # -- declarations -------------------------------------------------------
    def _check_fn(self, fn: ast.FnDecl) -> None:
        self.current_ret = fn.ret_type
        scope = Scope()
        for p in fn.params:
            scope.declare(p.name, p.type, p.line)
        self._check_block(fn.body, scope)

    def _check_block(self, block: ast.Block, parent: Scope) -> None:
        scope = Scope(parent)
        for stmt in block.statements:
            self._check_stmt(stmt, scope)

    # -- statements ---------------------------------------------------------
    def _check_stmt(self, stmt: ast.Stmt, scope: Scope) -> None:
        if isinstance(stmt, ast.LetStmt):
            vtype = self._check_expr(stmt.value, scope)
            if vtype == "void":
                raise AuricError("cannot bind a void value to a variable",
                                 stmt.line, stage="type")
            if stmt.decl_type is not None and stmt.decl_type != vtype:
                raise AuricError(
                    f"variable '{stmt.name}' declared as {stmt.decl_type} but "
                    f"initialized with {vtype}", stmt.line, stage="type")
            scope.declare(stmt.name, stmt.decl_type or vtype, stmt.line)

        elif isinstance(stmt, ast.AssignStmt):
            declared = scope.lookup(stmt.name)
            if declared is None:
                raise AuricError(f"assignment to undeclared variable "
                                 f"'{stmt.name}'", stmt.line, stage="type")
            vtype = self._check_expr(stmt.value, scope)
            if vtype != declared:
                raise AuricError(
                    f"cannot assign {vtype} to '{stmt.name}' of type {declared}",
                    stmt.line, stage="type")

        elif isinstance(stmt, ast.IfStmt):
            self._require_bool(stmt.cond, scope, "if condition")
            self._check_block(stmt.then_block, scope)
            if isinstance(stmt.else_block, ast.IfStmt):
                self._check_stmt(stmt.else_block, scope)
            elif isinstance(stmt.else_block, ast.Block):
                self._check_block(stmt.else_block, scope)

        elif isinstance(stmt, ast.WhileStmt):
            self._require_bool(stmt.cond, scope, "while condition")
            self._check_block(stmt.body, scope)

        elif isinstance(stmt, ast.ReturnStmt):
            if stmt.value is None:
                if self.current_ret != "void":
                    raise AuricError(
                        f"return without a value in a function returning "
                        f"{self.current_ret}", stmt.line, stage="type")
            else:
                vtype = self._check_expr(stmt.value, scope)
                if self.current_ret == "void":
                    raise AuricError("return with a value in a void function",
                                     stmt.line, stage="type")
                if vtype != self.current_ret:
                    raise AuricError(
                        f"returning {vtype} from a function declared to return "
                        f"{self.current_ret}", stmt.line, stage="type")

        elif isinstance(stmt, ast.ExprStmt):
            self._check_expr(stmt.expr, scope)

        elif isinstance(stmt, ast.Block):
            self._check_block(stmt, scope)

        else:  # pragma: no cover - guards against a new node type
            raise AuricError(f"internal: unhandled statement {type(stmt).__name__}",
                             getattr(stmt, "line", 0), stage="type")

    def _require_bool(self, expr: ast.Expr, scope: Scope, what: str) -> None:
        t = self._check_expr(expr, scope)
        if t != "bool":
            raise AuricError(f"{what} must be bool, found {t}", expr.line,
                             stage="type")

    # -- expressions --------------------------------------------------------
    def _check_expr(self, expr: ast.Expr, scope: Scope) -> str:
        t = self._infer(expr, scope)
        expr.type = t
        return t

    def _infer(self, expr: ast.Expr, scope: Scope) -> str:
        if isinstance(expr, ast.IntLit):
            return "int"
        if isinstance(expr, ast.FloatLit):
            return "float"
        if isinstance(expr, ast.BoolLit):
            return "bool"
        if isinstance(expr, ast.StringLit):
            return "string"

        if isinstance(expr, ast.VarExpr):
            declared = scope.lookup(expr.name)
            if declared is not None:
                return declared
            const = CONSTANTS.get(expr.name)
            if const is not None:
                return const.type
            raise AuricError(f"use of undeclared name '{expr.name}'", expr.line,
                             stage="type")

        if isinstance(expr, ast.UnaryExpr):
            ot = self._check_expr(expr.operand, scope)
            if expr.op == "!":
                if ot != "bool":
                    raise AuricError(f"'!' expects bool, found {ot}", expr.line,
                                     stage="type")
                return "bool"
            if expr.op == "-":
                if ot not in NUMERIC:
                    raise AuricError(f"unary '-' expects a number, found {ot}",
                                     expr.line, stage="type")
                return ot
            raise AuricError(f"unknown unary operator '{expr.op}'", expr.line,
                             stage="type")

        if isinstance(expr, ast.BinaryExpr):
            return self._infer_binary(expr, scope)

        if isinstance(expr, ast.CallExpr):
            return self._infer_call(expr, scope)

        raise AuricError(f"internal: unhandled expression "
                         f"{type(expr).__name__}", expr.line, stage="type")

    def _infer_binary(self, expr: ast.BinaryExpr, scope: Scope) -> str:
        op = expr.op
        lt = self._check_expr(expr.left, scope)
        rt = self._check_expr(expr.right, scope)

        if op in ("&&", "||"):
            if lt != "bool" or rt != "bool":
                raise AuricError(f"'{op}' expects bool operands, found "
                                 f"{lt} and {rt}", expr.line, stage="type")
            return "bool"

        if op in ("==", "!="):
            if lt != rt or lt not in EQUATABLE:
                raise AuricError(f"'{op}' cannot compare {lt} and {rt}",
                                 expr.line, stage="type")
            return "bool"

        if op in ("<", "<=", ">", ">="):
            if lt != rt or lt not in ORDERED:
                raise AuricError(f"'{op}' expects two ints or two floats, "
                                 f"found {lt} and {rt}", expr.line, stage="type")
            return "bool"

        # arithmetic: + - * / %
        if lt != rt or lt not in NUMERIC:
            raise AuricError(f"'{op}' expects two ints or two floats, found "
                             f"{lt} and {rt}", expr.line, stage="type")
        if op == "%" and lt != "int":
            raise AuricError("'%' is only defined for int", expr.line,
                             stage="type")
        return lt

    def _infer_call(self, expr: ast.CallExpr, scope: Scope) -> str:
        name = expr.callee
        builtin = BUILTINS.get(name)
        if builtin is not None:
            params, ret = builtin.params, builtin.ret
        elif name in self.functions:
            fn = self.functions[name]
            params = tuple(p.type for p in fn.params)
            ret = fn.ret_type
        else:
            raise AuricError(f"call to unknown function '{name}'", expr.line,
                             stage="type")

        if len(expr.args) != len(params):
            raise AuricError(
                f"'{name}' expects {len(params)} argument(s), got "
                f"{len(expr.args)}", expr.line, stage="type")
        for i, (arg, ptype) in enumerate(zip(expr.args, params)):
            at = self._check_expr(arg, scope)
            if at != ptype:
                raise AuricError(
                    f"argument {i + 1} of '{name}' expects {ptype}, found {at}",
                    arg.line, stage="type")
        return ret
