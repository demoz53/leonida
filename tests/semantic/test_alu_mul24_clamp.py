from __future__ import annotations

"""Semantic interpreter tests for RDNA2 24-bit VALU multiply / multiply-add with VOP3 clamp.

Covers:
  - v_mul_i32_i24(_e64) [clamp]
  - v_mul_u32_u24(_e64) [clamp]
  - v_mad_i32_i24 [clamp]
  - v_mad_u32_u24 [clamp]

Each 24-bit operand takes bits [23:0] (sign-extended for i24, zero-extended for u24).
Without clamp, the low 32 bits of the exact product (+ src2 for mad) wrap modulo 2^32.
With VOP3 clamp, the exact widened 64-bit value saturates to [INT32_MIN, INT32_MAX] (i24)
or [0, UINT32_MAX] (u24), including cases where a >32-bit product cancels back into range
when added to a signed 32-bit addend.
"""
import os
import random
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
TOOLS_DIR = os.environ.get("LEONIDA_TOOLS_DIR") or os.path.join(ROOT, "tools")

sys.dont_write_bytecode = True
sys.path.insert(0, TOOLS_DIR)
sys.path.insert(0, HERE)

import rdna2spirv  # noqa: E402
import spirv_mini_interp as si  # noqa: E402

M32 = 0xFFFFFFFF
STUB_DXBC = b"DXBC" + bytes(24) + struct.pack("<I", 0) + bytes(4)

_TMP_DIR = None
_SRC_PATH = None


def setUpModule():
    global _TMP_DIR, _SRC_PATH
    _TMP_DIR = tempfile.TemporaryDirectory()
    disasm_dir = os.path.join(_TMP_DIR.name, "disasm")
    os.mkdir(disasm_dir)
    _SRC_PATH = os.path.join(disasm_dir, "ps_mul24_clamp.s")
    with open(os.path.join(_TMP_DIR.name, "ps_mul24_clamp.dxbc"), "wb") as f:
        f.write(STUB_DXBC)


def tearDownModule():
    global _TMP_DIR, _SRC_PATH
    if _TMP_DIR is not None:
        _TMP_DIR.cleanup()
        _TMP_DIR = None
        _SRC_PATH = None


def s24(x: int) -> int:
    x &= 0xFFFFFF
    return x - (1 << 24) if x & 0x800000 else x


def s32(x: int) -> int:
    x &= M32
    return x - (1 << 32) if x & 0x80000000 else x


def ref(op: str, x: int, y: int, z: int | None, clamp: bool) -> int:
    signed = op.endswith("_i24")
    a = s24(x) if signed else (x & 0xFFFFFF)
    b = s24(y) if signed else (y & 0xFFFFFF)
    val = a * b
    if z is not None:
        val += s32(z) if signed else (z & M32)
    if clamp:
        lo, hi = (-(1 << 31), (1 << 31) - 1) if signed else (0, M32)
        val = min(max(val, lo), hi)
    return val & M32


def translate_and_run(body_lines: list[str]) -> tuple[int, str]:
    """Translate a synthetic PS returning v7 as two 16-bit halves ORed into 0x40000000."""
    if _TMP_DIR is None:
        setUpModule()
    asm_lines = [
        *body_lines,
        "v_and_b32_e32 v0, 0xffff, v7",
        "v_lshrrev_b32_e32 v1, 16, v7",
        "v_or_b32_e32 v0, 0x40000000, v0",
        "v_or_b32_e32 v1, 0x40000000, v1",
        "exp mrt0 v0, v1, v0, v0 done vm",
        "s_endpgm",
    ]
    with open(_SRC_PATH, "w", encoding="utf-8", newline="\n") as f:
        f.write("; test_mul24_clamp\n" + "".join(f"\t{ln}\n" for ln in asm_lines))
    spv_text = rdna2spirv.translate(_SRC_PATH)
    lo, hi = si.run_fragment(spv_text)["%out_mrt0"][:2]
    assert lo >> 16 == 0x4000 and hi >> 16 == 0x4000, (hex(lo), hex(hi))
    return (lo & 0xFFFF) | ((hi & 0xFFFF) << 16), spv_text


class Mul24ClampTest(unittest.TestCase):
    def _check(self, op: str, x: int, y: int, z: int | None, clamp: bool) -> str:
        asm_op = op + "_e64" if op.startswith("v_mul_") and clamp else op
        suffix = " clamp" if clamp else ""
        lines = [
            f"v_mov_b32_e32 v2, {x:#x}",
            f"v_mov_b32_e32 v3, {y:#x}",
        ]
        if z is None:
            lines.append(f"{asm_op} v7, v2, v3{suffix}")
        else:
            lines.append(f"v_mov_b32_e32 v4, {z:#x}")
            lines.append(f"{asm_op} v7, v2, v3, v4{suffix}")
        got, spv = translate_and_run(lines)
        want = ref(op, x, y, z, clamp)
        self.assertEqual(got, want, f"{asm_op}({x:#x}, {y:#x}, {z}){suffix}: got {got:#010x}, want {want:#010x}")
        return spv

    def test_mul_i32_i24_directed(self):
        cases = [
            (0x00800000, 0x00800000),  # (-2^23)*(-2^23) = +2^46 -> saturates to 0x7FFFFFFF
            (0x00800000, 0x007FFFFF),  # (-2^23)*(2^23-1) -> saturates to 0x80000000
            (0x007FFFFF, 0x007FFFFF),  # positive overflow -> 0x7FFFFFFF
            (0xFF800000, 0xAB000100),  # upper 8 bits ignored: (-2^23)*256 = -2^31 (exact INT32_MIN)
            (0x00010000, 0x00008000),  # +2^31 -> saturates to 0x7FFFFFFF
            (0x00010000, 0x00007FFF),  # fits in int32
            (0x00FFFFFF, 0x00000001),  # (-1)*1 = -1
            (0x00000000, 0x00800000),  # 0
        ]
        for x, y in cases:
            for clamp in (False, True):
                with self.subTest(x=hex(x), y=hex(y), clamp=clamp):
                    self._check("v_mul_i32_i24", x, y, None, clamp)

    def test_mul_u32_u24_directed(self):
        cases = [
            (0x00800000, 0x00800000),  # 2^46 -> saturates to 0xFFFFFFFF
            (0x00FFFFFF, 0x00FFFFFF),  # max u24 * max u24 -> 0xFFFFFFFF
            (0xFF010000, 0xAA00FFFF),  # upper 8 bits ignored: 0xFFFF0000 (fits)
            (0x00010000, 0x00010000),  # 2^32 -> saturates to 0xFFFFFFFF
            (0x00000000, 0x00FFFFFF),  # 0
        ]
        for x, y in cases:
            for clamp in (False, True):
                with self.subTest(x=hex(x), y=hex(y), clamp=clamp):
                    self._check("v_mul_u32_u24", x, y, None, clamp)

    def test_mad_i32_i24_directed_and_cancellation(self):
        cases = [
            (0x00800000, 0x00800000, 0xFFFFFFFF),  # +2^46 - 1 -> 0x7FFFFFFF
            (0x00800000, 0x007FFFFF, 0x7FFFFFFF),  # -(2^46 - 2^23) + (2^31-1) -> 0x80000000
            (0x00010000, 0x00008000, 0xFFFFFFFF),  # +2^31 + (-1) = +2^31-1 = 0x7FFFFFFF (fits!)
            (0x00010000, 0x00008000, 0xFFFFFFFE),  # +2^31 + (-2) = +2^31-2 = 0x7FFFFFFE (fits!)
            (0x00010000, 0x00008000, 0x80000000),  # +2^31 + (-2^31) = 0 (fits!)
            (0x00800000, 0x00000101, 0x00800000),  # (-2^23)*257 + 2^23 = -2^31 = 0x80000000 (fits!)
            (0x00800000, 0x00000101, 0x00800001),  # (-2^23)*257 + (2^23+1) = -2^31+1 = 0x80000001 (fits!)
            (0x00800000, 0x00000100, 0xFFFFFFFF),  # -2^31 + (-1) -> saturates to 0x80000000
            (0x00000000, 0x00000000, 0x7FFFFFFF),  # 0 + INT32_MAX -> INT32_MAX
        ]
        for x, y, z in cases:
            for clamp in (False, True):
                with self.subTest(x=hex(x), y=hex(y), z=hex(z), clamp=clamp):
                    self._check("v_mad_i32_i24", x, y, z, clamp)

    def test_mad_u32_u24_directed(self):
        cases = [
            (0x00800000, 0x00000002, 0xFFFFFFFF),  # 2^24 + 0xFFFFFFFF -> 0xFFFFFFFF
            (0x00010000, 0x0000FFFF, 0x0000FFFF),  # 0xFFFF0000 + 0xFFFF = 0xFFFFFFFF (exact boundary)
            (0x00010000, 0x0000FFFF, 0x00010000),  # 0xFFFF0000 + 0x10000 = 2^32 -> 0xFFFFFFFF
            (0x00800000, 0x00800000, 0x00000000),  # 2^46 + 0 -> 0xFFFFFFFF
            (0xFF000003, 0xAA000004, 0x00000005),  # upper 8 bits ignored: 3*4+5 = 17
        ]
        for x, y, z in cases:
            for clamp in (False, True):
                with self.subTest(x=hex(x), y=hex(y), z=hex(z), clamp=clamp):
                    self._check("v_mad_u32_u24", x, y, z, clamp)

    def test_random_matrix(self):
        rng = random.Random(20261124)
        for op in ("v_mul_i32_i24", "v_mul_u32_u24", "v_mad_i32_i24", "v_mad_u32_u24"):
            for _ in range(12):
                x = rng.randrange(1 << 32)
                y = rng.randrange(1 << 32)
                z = rng.randrange(1 << 32) if op.startswith("v_mad_") else None
                for clamp in (False, True):
                    with self.subTest(op=op, x=hex(x), y=hex(y), z=hex(z) if z is not None else None, clamp=clamp):
                        self._check(op, x, y, z, clamp)

    def test_spirv_val_if_available(self):
        spirv_as = shutil.which("spirv-as")
        spirv_val = shutil.which("spirv-val")
        if not spirv_as or not spirv_val:
            self.skipTest("spirv-as / spirv-val not on PATH")
        for op, z in (("v_mul_i32_i24", None), ("v_mul_u32_u24", None),
                      ("v_mad_i32_i24", 0xFFFFFFFE), ("v_mad_u32_u24", 0xFFFFFFFF)):
            spv_text = self._check(op, 0x00800000, 0x007FFFFF, z, True)
            with tempfile.TemporaryDirectory() as td:
                spvasm = os.path.join(td, f"{op}.spvasm")
                spv = os.path.join(td, f"{op}.spv")
                with open(spvasm, "w", encoding="utf-8", newline="\n") as f:
                    f.write(spv_text)
                subprocess.run([spirv_as, "--target-env", "vulkan1.2", spvasm, "-o", spv], check=True)
                subprocess.run([spirv_val, "--target-env", "vulkan1.2", spv], check=True)


if __name__ == "__main__":
    unittest.main()
