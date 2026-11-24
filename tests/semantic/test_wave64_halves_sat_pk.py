"""v_sat_pk_u8_i16 on the wave64 halves path keeps its destination's high half (tools/wave64_halves.py).

The op saturates the two signed i16 halves of SRC0 to bytes and writes them to the low 16 bits of VDST. The
translator writes that result with write_lo16, which keeps the high half in the _e32, _e64 and DPP forms; under SDWA
dst_sel/dst_unused decide (a sub-dword dst_sel with UNUSED_PRESERVE keeps the rest, DWORD or UNUSED_PAD do not). The
halves analysis has to read the old destination wherever the translator keeps it: when it took the op for a full
32-bit write (it was in _FULL32_V16_PREFIXES) a lane-dependent high half lost its taint and the program passed as tier
A, though its export changes when the wave is cut in two.

The program is the hand-written fixtures/w64_sat_pk_hi.asm, one line of it swapped for each encoding. It runs on the
strict wave interpreter as one wave of 64 lanes and as two waves of 32 (test_wave64_halves.run_cut); the expected
words come from the plain-Python sat8 reference below. No llvm-mc, GPU or game data.

    python -m unittest discover -s tests/semantic -p test_wave64_halves_sat_pk.py
"""
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import support  # noqa: E402,F401  (sets the tools path)
from support import translate_listing  # noqa: E402
import rdna2spirv  # noqa: E402
import wave64_halves  # noqa: E402
from test_wave64_halves import meta_of, out_param, run_cut, vs  # noqa: E402

FIXTURE = os.path.join(HERE, "fixtures", "w64_sat_pk_hi.asm")
E32_LINE = "\tv_sat_pk_u8_i16_e32 v6, v1              ; encoding: [0x01,0xc5,0x0c,0x7e]\n"

# The fixture's v_sat_pk_u8_i16 line in each encoding (text and bytes from llvm-mc -mcpu=gfx1030), and whether it keeps
# v6's high half.
FORMS = {
    "e32": ("v_sat_pk_u8_i16_e32 v6, v1", "0x01,0xc5,0x0c,0x7e", True),
    "e64": ("v_sat_pk_u8_i16_e64 v6, v1", "0x06,0x00,0xe2,0xd5,0x01,0x01,0x01,0x02", True),
    "dpp": ("v_sat_pk_u8_i16_dpp v6, v1 quad_perm:[0,1,2,3] row_mask:0xf bank_mask:0xf bound_ctrl:1",
            "0xfa,0xc4,0x0c,0x7e,0x01,0xe4,0x08,0xff", True),
    "sdwa_word0_preserve": ("v_sat_pk_u8_i16_sdwa v6, v1 dst_sel:WORD_0 dst_unused:UNUSED_PRESERVE src0_sel:DWORD",
                            "0xf9,0xc4,0x0c,0x7e,0x01,0x14,0x06,0x00", True),
    "sdwa_word0_pad": ("v_sat_pk_u8_i16_sdwa v6, v1 dst_sel:WORD_0 dst_unused:UNUSED_PAD src0_sel:DWORD",
                       "0xf9,0xc4,0x0c,0x7e,0x01,0x04,0x06,0x00", False),
    "sdwa_dword_preserve": ("v_sat_pk_u8_i16_sdwa v6, v1 dst_sel:DWORD dst_unused:UNUSED_PRESERVE src0_sel:DWORD",
                            "0xf9,0xc4,0x0c,0x7e,0x01,0x16,0x06,0x00", False),
}


def sat8(h):
    """A signed 16-bit value saturated to an unsigned byte."""
    h = h - 0x10000 if h & 0x8000 else h
    return 0 if h <= 0 else min(h, 255)


def src0(k):
    """SRC0 of vertex k in the fixture: hi = 300 - 8k, lo = 8k - 128, each as 16 bits."""
    return ((300 - 8 * k) & 0xFFFF) << 16 | ((8 * k - 128) & 0xFFFF)


def sat_pk(s):
    return sat8(s >> 16) << 8 | sat8(s & 0xFFFF)


def program(form):
    with open(FIXTURE, "rb") as f:
        text = f.read().decode("utf-8")
    assert text.count(E32_LINE) == 1, "fixture lost its v_sat_pk_u8_i16_e32 line"
    line, enc, _ = FORMS[form]
    return text.replace(E32_LINE, f"\t{line} ; encoding: [{enc}]\n").encode("utf-8")


def tier_and_runs(name, data):
    asm = translate_listing(name, data)
    w = rdna2spirv.translate.last_gen.w64g
    tier, reasons = (w["tier"], w["reasons"]) if w else (None, [])
    return tier, reasons, out_param(run_cut(asm, 64)), out_param(run_cut(asm, 32))


class SatPkHighHalfOnHalvesPath(unittest.TestCase):
    def test_reference_saturates_both_halves_both_ways(self):
        # the fixture's lanes reach below 0 and above 255 in each half of SRC0, and the in-range values in between
        lo = [(src0(k) & 0xFFFF) for k in range(64)]
        hi = [(src0(k) >> 16) for k in range(64)]
        for half in (lo, hi):
            got = {sat8(h) for h in half}
            self.assertIn(0, got)
            self.assertIn(255, got)
            self.assertTrue(any(0 < sat8(h) < 255 for h in half))
            self.assertTrue(any(h & 0x8000 for h in half))              # negative inputs
            self.assertTrue(any(0xFF < h < 0x8000 for h in half))       # above 255

    def test_kept_high_half_is_tier_c_and_differs_when_cut(self):
        for form, (_, _, keeps) in FORMS.items():
            if not keeps:
                continue
            with self.subTest(form=form):
                tier, reasons, whole, halves = tier_and_runs(f"vs_w64_sat_pk_hi_{form}", program(form))
                self.assertEqual(whole, [((k << 16) | sat_pk(src0(k)), k, src0(k), 0) for k in range(64)])
                self.assertEqual(halves, [((k % 32) << 16 | sat_pk(src0(k)), k % 32, src0(k), 0)
                                          for k in range(64)])
                self.assertEqual(tier, "C", f"{form}: reasons={reasons}")
                self.assertIn("lane-id", reasons, form)

    def test_full_sdwa_write_stays_tier_a(self):
        for form, (_, _, keeps) in FORMS.items():
            if keeps:
                continue
            with self.subTest(form=form):
                tier, reasons, whole, halves = tier_and_runs(f"vs_w64_sat_pk_full_{form}", program(form))
                want = [(sat_pk(src0(k)), 0, src0(k), 0) for k in range(64)]
                self.assertEqual(whole, want)
                self.assertEqual(halves, want)
                self.assertEqual(tier, "A", f"{form}: reasons={reasons}")

    def test_kept_high_half_blocks_uniform_readlane(self):
        # high half of v10 = vertex index, low half from a uniform SGPR: v10 is not uniform, so v_readlane_b32 of lane 0
        # is a cross-lane read, and the second half's lane 0 (global lane 32) gives another word
        for form in ("e32", "e64"):
            with self.subTest(form=form):
                (tier, reasons, idioms), asm = meta_of(f"vs_w64_sat_pk_readlane_{form}", vs([
                    "v_lshlrev_b32_e32 v10, 16, v0",
                    f"v_sat_pk_u8_i16_{form} v10, s4",
                    "v_readlane_b32 s6, v10, 0",
                    "v_mov_b32_e32 v6, s6",
                ]))
                self.assertEqual(tier, "C", f"reasons={reasons}, idioms={idioms}")
                self.assertIn("cross-lane", reasons)
                self.assertNotIn("readlane-uniform", idioms)
                whole = [w[0] for w in out_param(run_cut(asm, 64))]
                halves = [w[0] for w in out_param(run_cut(asm, 32))]
                self.assertEqual(whole, [whole[0]] * 64)
                self.assertEqual(halves, [whole[0]] * 32 + [whole[0] | 32 << 16] * 32)

    def test_operands_and_partial_write_per_encoding(self):
        v6 = {"k": "vgpr", "r": 6, "n": 1}
        v1 = {"k": "vgpr", "r": 1, "n": 1}
        cases = [(line.split()[0], line.split(" ", 3)[3].split() if line.count(" ") > 2 else [], keeps)
                 for line, _, keeps in FORMS.values()]
        cases.append(("v_sat_pk_u8_i16_sdwa", ["dst_sel:WORD_1", "dst_unused:UNUSED_PRESERVE", "src0_sel:DWORD"],
                      True))
        cases.append(("v_sat_pk_u8_i16_dpp", ["quad_perm:[1,0,3,2]", "row_mask:0xf", "bank_mask:0xf"], True))
        for op, mods, keeps in cases:
            with self.subTest(op=op, mods=mods):
                ins = {"op": op, "args": [v6, v1], "mods": mods}
                _, srcs, _, _ = wave64_halves.operands(ins)
                self.assertEqual(any(x is v6 for x in srcs), keeps)
                self.assertEqual(wave64_halves.partial_write(ins, wave64_halves.base_op(op)), keeps)


if __name__ == "__main__":
    unittest.main()
