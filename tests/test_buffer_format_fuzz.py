"""Unit tests for tools/buffer_format_fuzz.py."""

from __future__ import annotations

import subprocess
import sys
import unittest
from fractions import Fraction
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "tools"))

import buffer_format_fuzz as bff  # noqa: E402


class TestBufferFormatFuzz(unittest.TestCase):
    def test_parse_name_and_format_tables(self) -> None:
        self.assertEqual(bff.parse_name("2_10_10_10_UNORM"), ((10, 10, 10, 2), "UNORM"))
        self.assertEqual(bff.parse_name("10_10_10_2_SINT"), ((2, 10, 10, 10), "SINT"))
        self.assertEqual(len(bff.GFX10_NAMES), 77)
        self.assertEqual(len(bff.GFX11_CODES), 63)

    def test_frac_to_f32(self) -> None:
        self.assertEqual(bff.frac_to_f32(Fraction(0)), 0)
        self.assertEqual(bff.frac_to_f32(Fraction(1)), bff.f32_bits(1.0))
        self.assertEqual(bff.frac_to_f32(Fraction(1, 2)), bff.f32_bits(0.5))
        self.assertEqual(bff.frac_to_f32(Fraction(255, 255)), bff.f32_bits(1.0))

    def test_cli_self_test_passes(self) -> None:
        proc = subprocess.run(
            [sys.executable, "-I", str(REPO / "tools" / "buffer_format_fuzz.py"), "--self-test"],
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("self-test: ok", proc.stdout)


if __name__ == "__main__":
    unittest.main()
