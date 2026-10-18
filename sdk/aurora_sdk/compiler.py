"""Compile the supported Aurora Python subset to the native C SDK.

This is intentionally a small, explicit subset rather than a claim of
CPython compatibility. It keeps generated apps compatible with the existing
AURC loader and runtime without adding a second console ABI.
"""
from __future__ import annotations

import ast
import json
from pathlib import Path


class PythonBuildError(ValueError):
    """Raised when a Python app uses unsupported syntax."""


_CONSTANTS = {
    "SCREEN_TOP": "SCREEN_TOP",
    "SCREEN_BOTTOM": "SCREEN_BOTTOM",
    "COLOR_BLACK": "COLOR_BLACK",
    "COLOR_WHITE": "COLOR_WHITE",
    "COLOR_GRAY": "COLOR_GRAY",
    "COLOR_RED": "COLOR_RED",
    "COLOR_GREEN": "COLOR_GREEN",
    "COLOR_BLUE": "COLOR_BLUE",
    "COLOR_CYAN": "COLOR_CYAN",
    "COLOR_MAGENTA": "COLOR_MAGENTA",
    "COLOR_YELLOW": "COLOR_YELLOW",
    "COLOR_ORANGE": "COLOR_ORANGE",
    "COLOR_AURORA": "COLOR_AURORA",
    "FONT_MONO": "FONT_MONO",
    "FONT_SMALL": "FONT_SMALL",
    "FONT_REGULAR": "FONT_REGULAR",
    "FONT_BOLD": "FONT_BOLD",
    "FONT_TITLE": "FONT_TITLE",
}

_CALLS = {
    "loop": ("bool", "app_loop"),
    "quit": ("void", "app_quit"),
    "present": ("void", "gfx_present"),
    "keys_down": ("int", "hid_keys_down"),
    "keys_held": ("int", "hid_keys_held"),
    "touch_held": ("int", "hid_touch_held"),
    "touch_x": ("int", "hid_touch_x"),
    "touch_y": ("int", "hid_touch_y"),
    "clear": ("void", "gfx_clear"),
    "pixel": ("void", "gfx_pixel"),
    "rect": ("void", "gfx_rect"),
    "line": ("void", "gfx_line"),
    "circle": ("void", "gfx_circle"),
    "text": ("int", "gfx_text"),
    "print": ("int", "gfx_print"),
}


def _error(node: ast.AST, message: str) -> PythonBuildError:
    line = getattr(node, "lineno", 1)
    return PythonBuildError(f"line {line}: {message}")


class _Compiler(ast.NodeVisitor):
    def __init__(self) -> None:
        self.lines: list[str] = []
        self.indent = 1
        self.variables: set[str] = set()

    def emit(self, line: str) -> None:
        self.lines.append("  " * self.indent + line)

    def visit_Module(self, node: ast.Module) -> None:
        functions = [n for n in node.body if isinstance(n, ast.FunctionDef)]
        imports = [n for n in node.body if isinstance(n, ast.Import)]
        if any(n.name != "main" for n in functions):
            extra = next(n for n in functions if n.name != "main")
            raise _error(extra, "only main() is supported")
        if any(not (isinstance(n, (ast.FunctionDef, ast.Import, ast.Expr))
                    and (not isinstance(n, ast.Expr) or
                         isinstance(n.value, ast.Constant)))
                   for n in node.body):
            raise _error(node, "only imports, a main() function, and a docstring are allowed")
        if not any(n.name == "main" for n in functions):
            raise _error(node, "the app must define def main():")
        for item in imports:
            if (len(item.names) != 1 or item.names[0].name != "aurora"
                    or item.names[0].asname not in (None, "aurora")):
                raise _error(item, "only 'import aurora' is supported")
        main = next(n for n in functions if n.name == "main")
        if main.args.args or main.args.vararg or main.args.kwarg:
            raise _error(main, "main() must not take arguments")
        self.visit_all(main.body)

    def visit_all(self, nodes: list[ast.stmt]) -> None:
        for node in nodes:
            self.visit(node)

    def visit_FunctionDef(self, node: ast.FunctionDef) -> None:
        raise _error(node, "only main() is supported")

    def visit_Expr(self, node: ast.Expr) -> None:
        if isinstance(node.value, ast.Constant) and isinstance(node.value.value, str):
            return
        self.emit(self.call(node.value, statement=True) + ";")

    def visit_Assign(self, node: ast.Assign) -> None:
        if len(node.targets) != 1 or not isinstance(node.targets[0], ast.Name):
            raise _error(node, "only assignment to one local name is supported")
        target = node.targets[0].id
        value = self.expr(node.value)
        if target in self.variables:
            self.emit(f"{target} = {value};")
        else:
            self.variables.add(target)
            self.emit(f"long {target} = {value};")

    def visit_AugAssign(self, node: ast.AugAssign) -> None:
        if not isinstance(node.target, ast.Name):
            raise _error(node, "augmented assignment needs a local name")
        if node.target.id not in self.variables:
            raise _error(node, f"unknown local {node.target.id!r}")
        op = self.operator(node.op)
        self.emit(f"{node.target.id} {op}= {self.expr(node.value)};")

    def visit_If(self, node: ast.If) -> None:
        self.emit(f"if ({self.expr(node.test)}) {{")
        self.indent += 1
        self.visit_all(node.body)
        self.indent -= 1
        if node.orelse:
            self.emit("} else {")
            self.indent += 1
            self.visit_all(node.orelse)
            self.indent -= 1
        self.emit("}")

    def visit_While(self, node: ast.While) -> None:
        self.emit(f"while ({self.expr(node.test)}) {{")
        self.indent += 1
        self.visit_all(node.body)
        self.indent -= 1
        self.emit("}")

    def visit_Break(self, node: ast.Break) -> None:
        self.emit("break;")

    def visit_Return(self, node: ast.Return) -> None:
        if node.value is not None:
            raise _error(node, "main() cannot return a value")
        self.emit("return 0;")

    def generic_visit(self, node: ast.AST) -> None:
        raise _error(node, f"unsupported syntax: {type(node).__name__}")

    def expr(self, node: ast.AST) -> str:
        if isinstance(node, ast.Constant):
            if isinstance(node.value, bool):
                return "1" if node.value else "0"
            if isinstance(node.value, int):
                return str(node.value)
            if isinstance(node.value, str):
                return json.dumps(node.value)
            raise _error(node, "only integer, boolean, and string literals are supported")
        if isinstance(node, ast.Name):
            if node.id in _CONSTANTS:
                return _CONSTANTS[node.id]
            if node.id in self.variables:
                return node.id
            raise _error(node, f"unknown name {node.id!r}")
        if (isinstance(node, ast.Attribute) and isinstance(node.value, ast.Name)
                and node.value.id == "aurora" and node.attr in _CONSTANTS):
            return _CONSTANTS[node.attr]
        if isinstance(node, ast.UnaryOp) and isinstance(node.op, (ast.USub, ast.UAdd, ast.Not)):
            op = {ast.USub: "-", ast.UAdd: "+", ast.Not: "!"}[type(node.op)]
            return f"({op}{self.expr(node.operand)})"
        if isinstance(node, ast.BinOp):
            return f"({self.expr(node.left)} {self.operator(node.op)} {self.expr(node.right)})"
        if isinstance(node, ast.BoolOp):
            op = " && " if isinstance(node.op, ast.And) else " || "
            return "(" + op.join(self.expr(v) for v in node.values) + ")"
        if isinstance(node, ast.Compare):
            if len(node.ops) != 1 or len(node.comparators) != 1:
                raise _error(node, "chained comparisons are not supported")
            return (f"({self.expr(node.left)} {self.operator(node.ops[0])} "
                    f"{self.expr(node.comparators[0])})")
        if isinstance(node, ast.Call):
            return self.call(node)
        raise _error(node, f"unsupported expression: {type(node).__name__}")

    def call(self, node: ast.AST, statement: bool = False) -> str:
        if not isinstance(node, ast.Call) or not isinstance(node.func, ast.Attribute):
            raise _error(node, "calls must use the aurora.<function>(...) API")
        if not isinstance(node.func.value, ast.Name) or node.func.value.id != "aurora":
            raise _error(node, "calls must use the aurora.<function>(...) API")
        spec = _CALLS.get(node.func.attr)
        if spec is None:
            raise _error(node, f"unsupported aurora API: {node.func.attr}")
        if node.keywords:
            raise _error(node, "keyword arguments are not supported")
        args = ", ".join(self.expr(arg) for arg in node.args)
        result = f"{spec[1]}({args})"
        if statement and spec[0] != "void":
            return f"(void){result}"
        return result

    @staticmethod
    def operator(node: ast.AST) -> str:
        operators = {
            ast.Add: "+", ast.Sub: "-", ast.Mult: "*", ast.Div: "/",
            ast.Mod: "%", ast.BitAnd: "&", ast.BitOr: "|",
            ast.Lt: "<", ast.LtE: "<=", ast.Gt: ">", ast.GtE: ">=",
            ast.Eq: "==", ast.NotEq: "!=", ast.And: "&&", ast.Or: "||",
        }
        try:
            return operators[type(node)]
        except KeyError as exc:
            raise PythonBuildError(f"unsupported operator: {type(node).__name__}") from exc


def compile_source(source: str, *, name: str = "AuroraPython") -> str:
    """Translate a Python app into a C translation unit."""
    try:
        tree = ast.parse(source, filename=f"{name}.py")
    except SyntaxError as exc:
        raise PythonBuildError(f"line {exc.lineno}: {exc.msg}") from exc
    compiler = _Compiler()
    compiler.visit(tree)
    body = "\n".join(compiler.lines) or "  return 0;"
    return "#include <aurora_app.h>\n\nint main(void) {\n" + body + "\n}\n"


def compile_file(path: Path, output: Path) -> None:
    output.write_text(compile_source(path.read_text(encoding="utf-8"),
                                      name=path.stem), encoding="utf-8")
