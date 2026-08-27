"""Run the whole Auric compiler test suite with the standard library only.

    python tests/run_tests.py         # from the auric-lang/ directory
    python run_tests.py               # from the tests/ directory

Uses unittest discovery; no pytest or other dependency required. Exits non-zero
if anything fails.
"""
import sys
import unittest
from pathlib import Path

AURIC_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(AURIC_ROOT))   # so `import compiler` works from anywhere


def main() -> int:
    loader = unittest.TestLoader()
    suite = loader.discover(start_dir=str(AURIC_ROOT / "tests"),
                            top_level_dir=str(AURIC_ROOT))
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
