"""Tests for the host-side Aurora Python SDK compiler."""
import tempfile
import unittest
from pathlib import Path

import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from aurora_sdk.compiler import PythonBuildError, compile_source
from aurora_sdk.project import create_project


class CompilerTests(unittest.TestCase):
    def test_compiles_supported_app(self):
        c = compile_source(
            "import aurora\n"
            "def main():\n"
            "    x = 2\n"
            "    while aurora.loop():\n"
            "        aurora.clear(aurora.SCREEN_TOP, aurora.COLOR_BLACK)\n"
            "        aurora.circle(aurora.SCREEN_TOP, x, 4, 2, aurora.COLOR_WHITE)\n"
            "        x += 1\n"
            "        if aurora.keys_down():\n"
            "            break\n")
        self.assertIn("#include <aurora_app.h>", c)
        self.assertIn("while (app_loop())", c)
        self.assertIn("gfx_circle(SCREEN_TOP, x, 4, 2, COLOR_WHITE)", c)

    def test_rejects_unsupported_import(self):
        with self.assertRaisesRegex(PythonBuildError, "only 'import aurora'"):
            compile_source("import os\n\ndef main():\n    pass\n")

    def test_rejects_missing_main(self):
        with self.assertRaisesRegex(PythonBuildError, "must define"):
            compile_source("import aurora\n")

    def test_scaffolds_project(self):
        with tempfile.TemporaryDirectory() as directory:
            root = create_project("Demo App", Path(directory))
            self.assertTrue((root / "main.py").exists())
            self.assertTrue((root / "Demo_App.icon").exists())


if __name__ == "__main__":
    unittest.main()
