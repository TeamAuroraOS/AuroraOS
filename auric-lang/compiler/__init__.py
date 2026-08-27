"""The Auric compiler: a tiny statically-typed language that transpiles to
freestanding C for AuroraOS (Nintendo 3DS, ARM9).

Pipeline:  source -> lexer -> parser -> type checker -> C code generator.

`compile_to_c()` runs the whole front end and returns generated C. The `aurc`
driver takes it from there (arm-none-eabi-gcc -> objcopy -> aur_pack).
"""
from __future__ import annotations

from .ast import Program
from .codegen import CodeGen
from .errors import AuricError
from .lexer import Lexer
from .parser import Parser
from .typecheck import TypeChecker

__version__ = "0.1.0"

__all__ = ["AuricError", "Program", "parse_source", "check_source", "compile_to_c",
           "__version__"]


def parse_source(src: str) -> Program:
    """Lex and parse `src` into an AST (no type checking)."""
    tokens = Lexer(src).tokenize()
    return Parser(tokens).parse()


def check_source(src: str) -> Program:
    """Lex, parse, and type-check `src`, returning the annotated AST."""
    program = parse_source(src)
    TypeChecker(program).check()
    return program


def compile_to_c(src: str) -> str:
    """Compile Auric `src` all the way to freestanding C source text."""
    program = check_source(src)
    return CodeGen(program).generate()
