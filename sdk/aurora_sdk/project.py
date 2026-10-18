"""Project scaffolding for Aurora Python apps."""
from __future__ import annotations

from pathlib import Path

from .compiler import PythonBuildError

_ICON = """\
; {name}: 32 x 32, # is a lit pixel
 ##############################
################################
##                            ##
##          ########          ##
##        ############        ##
##       ####      ####       ##
##      ###          ###      ##
##     ###                    ##
##     ###                    ##
##    ###                     ##
##    ###                     ##
##    ###                     ##
##    ###                     ##
##    ###                     ##
##    ###                     ##
##    ###                     ##
##    ###                     ##
##    ###                     ##
##    ###                     ##
##     ###                    ##
##      ###          ###      ##
##       ####      ####       ##
##        ############        ##
##          ########          ##
##                            ##
##                            ##
##                            ##
##                            ##
################################
 ##############################
"""

_MAIN = """\
import aurora


def main():
    while aurora.loop():
        aurora.clear(aurora.SCREEN_TOP, aurora.COLOR_BLACK)
        aurora.text(aurora.SCREEN_TOP, 12, 10, aurora.COLOR_WHITE,
                    "Hello from Python on AuroraOS!")
        if aurora.keys_down():
            break
"""


def create_project(name: str, directory: Path | None = None) -> Path:
    safe = "".join(c if c.isalnum() or c in "_.-" else "_" for c in name).strip("._")
    if not safe:
        raise PythonBuildError("project name must contain a letter or digit")
    root = (directory or Path.cwd()) / safe
    if root.exists() and any(root.iterdir()):
        raise PythonBuildError(f"{root} exists and is not empty")
    root.mkdir(parents=True, exist_ok=True)
    (root / "main.py").write_text(_MAIN, encoding="utf-8")
    (root / f"{safe}.icon").write_text(_ICON.format(name=safe), encoding="ascii")
    return root
