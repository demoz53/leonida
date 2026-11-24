"""Tests for VOP3 clamp on v_sad_u8, v_sad_hi_u8, v_sad_u16 (SHADER-845)."""

import os
import pathlib
import struct
import sys
import tempfile
import unittest
import warnings

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "tools"))
import rdna2spirv  # noqa: E402

FSSM = bytearray(0x148)
struct.pack_into("<4I", FSSM, 0x138, 0x88, 32, 1, 1)
FSSM = bytes(FSSM)


def _translate_cs(body: list[str]) -> str:
    with tempfile.TemporaryDirectory() as tmp:
        os.makedirs(os.path.join(tmp, "fssm"))
        os.makedirs(os.path.join(tmp, "disasm"))
        with open(os.path.join(tmp, "fssm", "cs_sad.fssm"), "wb") as f:
            f.write(FSSM)
        src = os.path.join(tmp, "disasm", "cs_sad.s")
        with open(src, "w", encoding="utf-8") as f:
            f.write("; cs_sad\n" + "".join(f"\t{ln}\n" for ln in body) + "\ts_endpgm\n")
        with warnings.catch_warnings():
            warnings.simplefilter("ignore", ResourceWarning)
            return rdna2spirv.translate(src)


class SadClampTests(unittest.TestCase):
    def test_sad_clamp_emits_unsigned_saturation(self):
        for op in ("v_sad_u8", "v_sad_hi_u8", "v_sad_u16", "v_sad_u32", "v_msad_u8"):
            with self.subTest(op=op):
                spv = _translate_cs([
                    "v_mov_b32 v1, 0xffffffff",
                    "v_mov_b32 v2, 0",
                    "v_mov_b32 v3, 0xfffffff0",
                    f"{op} v0, v1, v2, v3 clamp",
                ])
                self.assertIn("OpULessThan %bool", spv)
                self.assertIn("OpConstant %uint 4294967295", spv)

    def test_sad_without_clamp_has_no_saturation(self):
        spv = _translate_cs([
            "v_mov_b32 v1, 1",
            "v_mov_b32 v2, 0",
            "v_mov_b32 v3, 2",
            "v_sad_u8 v0, v1, v2, v3",
        ])
        self.assertNotIn("OpULessThan %bool", spv)


if __name__ == "__main__":
    unittest.main()
