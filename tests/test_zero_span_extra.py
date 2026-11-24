"""Additional unit tests for tools/zero_span.py (memoryview, inverted slices, multi-window)."""

from __future__ import annotations

import sys
import unittest
from pathlib import Path
from unittest import mock

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "tools"))

import zero_span  # noqa: E402


class TestZeroSpanExtra(unittest.TestCase):
    def test_memoryview_and_bytearray(self) -> None:
        buf = bytearray(b"\x00\x00\x00\x01\x00\x00")
        mv = memoryview(buf)
        self.assertTrue(zero_span.all_zero(mv, 0, 3))
        self.assertFalse(zero_span.all_zero(mv, 0, 4))
        self.assertEqual(zero_span.gap_split(mv, 4, 6), (2, 0))
        self.assertEqual(zero_span.gap_split(mv, 2, 5), (0, 3))

    def test_inverted_and_negative_slices(self) -> None:
        buf = b"\x00\x00\x01\x00"
        self.assertTrue(zero_span.all_zero(buf, 3, 1))
        self.assertEqual(zero_span.gap_split(buf, 3, 1), (0, 0))
        self.assertEqual(zero_span.gap_split(buf, -1, 10), (1, 0))

    def test_multi_window_iteration(self) -> None:
        buf = bytearray(32)
        buf[25] = 0xFF
        with mock.patch.object(zero_span, "WINDOW", 8), mock.patch.object(zero_span, "_ZEROS", bytes(8)):
            self.assertTrue(zero_span.all_zero(buf, 0, 24))
            self.assertFalse(zero_span.all_zero(buf, 0, 32))
            self.assertEqual(zero_span.gap_split(buf, 0, 24), (24, 0))


if __name__ == "__main__":
    unittest.main()
