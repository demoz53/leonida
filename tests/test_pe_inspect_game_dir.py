"""Regression test for --game-dir validation in tools/pe_inspect.py."""

from __future__ import annotations

import contextlib
import io
import sys
import unittest
from pathlib import Path
from tempfile import TemporaryDirectory

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "tools"))

import pe_inspect  # noqa: E402


class TestPeInspectGameDir(unittest.TestCase):
    def test_missing_game_dir_returns_error(self) -> None:
        with TemporaryDirectory() as tmp:
            missing = Path(tmp) / "no_such_game_dir"
            err = io.StringIO()
            with contextlib.redirect_stderr(err):
                rc = pe_inspect.main([str(Path(tmp) / "img.exe"), "--game-dir", str(missing), "--out", tmp])
            self.assertEqual(rc, 2)
            self.assertIn("does not exist", err.getvalue())


if __name__ == "__main__":
    unittest.main()
