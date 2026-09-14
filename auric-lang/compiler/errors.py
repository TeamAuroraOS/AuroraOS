"""Shared error type: every stage raises AuricError with a 1-based line and
column.
"""
from __future__ import annotations


class AuricError(Exception):
    """A compile-time error in an Auric program.

    Attributes:
        message: human-readable description of what went wrong.
        line:    1-based source line the error points at (0 if unknown).
        col:     1-based source column (0 if unknown).
        stage:   which compiler stage raised it ("lex", "parse", "type", ...).
    """

    def __init__(self, message: str, line: int = 0, col: int = 0,
                 stage: str = "compile") -> None:
        self.message = message
        self.line = line
        self.col = col
        self.stage = stage
        super().__init__(self.format())

    def format(self) -> str:
        where = ""
        if self.line:
            where = f" (line {self.line}" + (f", col {self.col}" if self.col else "") + ")"
        return f"{self.stage} error{where}: {self.message}"
