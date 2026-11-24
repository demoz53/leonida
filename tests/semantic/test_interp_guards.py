"""SHADER-177: v_interp reads the barycentric pair through its VGPR source (I = v0, J = v1), and
v_interp_mov_f32 names the vertex with p0 (p10/p20 would be the other two vertices). The single-lane
model returns the attribute's own value, which is only that when the operand really holds a coordinate of
the pair, so a source with no provable value is refused instead of silently translating wrong
(docs/design.md, PS system inputs):

* v_interp_p1/p2 (f32 and f16 forms) interpolate with the value their VGPR source holds: p1 weights P10 and
  p2 weights P20, and a source qualifies when it provably holds a coordinate of the pair — the hardware
  register v0 (I) or v1 (J), or a qualifying copy rooted at one. A pair that names them in the other order,
  p1(v1)/p2(v0), or with both ops naming the same one, is modelled with the values those registers hold
  (SHADER-659: measured with runtime/test/shaders/ps_interp_pair_roles.asm on a gfx1036 iGPU and an RTX 4090,
  docs/design.md "v_interp operand guards"). A source whose value is nothing provable — v4, v9, an unwritten
  register — is still Unsupported naming the register and the role (the corpus has none: 117,001 v0 and 116,997
  v1 over 26,503 shaders, and no read names the other role);
* an opposite-role read with no partner op to take the other axis from is Unsupported: the model needs both
  weights of the pair;
* v_interp_mov_f32 with p10/p20 is modelled as P1 - P0 / P2 - P0 (test_interp_mov_delta.py; all 6,926 in the
  corpus are p0); any other select is Unsupported, naming it;
* a plain v_mov_b32 copy of the op's coordinate (rooted at v0 for p1, at v1 for p2) is still accepted,
  including a chain of copies: its single write in the whole shader is the mov, and its source v0/v1
  is itself unwritten up to the copy (a copy of an already-overwritten v0/v1 carries a computed value —
  the zero-before-copy measurement renders (0,0) on hardware — while zeroing after the copy is fine,
  the copy saved the value). One corpus shader reads its J through a copy of v1 and must keep
  translating (bit-identical .spv); a copy in the role its coordinate fits keeps the centre-interpolated
  read and that bit-identical .spv; a copy of the *other* coordinate is weighted with that value;
* a register written twice (the mov, then anything else) no longer qualifies — and since the
  computed-I/J model landed, a written-again register, a copy of an already-overwritten v0/v1, or a
  modified (dpp) mov is no longer refused but modelled: the operand is a value the program wrote,
  and the model weights the interpolation with the value loaded from %vgpr (Vonsek's zero-before-copy
  row renders P0, which is exactly those loads' result). A copy in either role is
  modelled with the barycentric its root holds, like the pairs above;
* the standard forms (v0/v1 pairs, p0 mov) translate and compute exactly as before.

On main (before the guards) the refused cases translated silently and passed spirv-val: the operand
was ignored outright, so a second-pair I/J produced the very module of the standard pair.
"""
import struct
import unittest

import support
import rdna2spirv

ATTR0 = (0.5, 0.25, 0.0, 1.0)
FLAT_ATTR1_X = 0x101          # attr1.x read through v_interp_mov only: the provoking vertex's word
BARY = (0.25, 0.25, 0.5)       # (k, i, j): the weights the interpreter seeds v0/v1 from (.y/.z)
# Three DISTINCT vertices of attr0 (raw words), so a test's weights are visible in the result.
VERTS0 = ((0.0, 0.0, 1.0, 0.0), (2.0, 0.0, 3.0, 0.0), (0.0, 2.0, 5.0, 0.0))


def f32_bits(x):
    return struct.unpack("<I", struct.pack("<f", x))[0]


WORDS0 = [tuple(f32_bits(x) for x in v) for v in VERTS0]


class InterpPairGuards(unittest.TestCase):
    def test_second_pair_refused(self):
        with self.assertRaises(rdna2spirv.Unsupported) as cm:
            support.translate_lines("ps_i", ["v_interp_p1_f32_e32 v2, v4, attr0.x",
                                             "v_interp_p2_f32_e32 v2, v5, attr0.x",
                                             "exp mrt0 v2, v2, v2, v2 done vm",
                                             "s_endpgm"])
        self.assertIn("v_interp_p1_f32 I source v4", str(cm.exception))

    def test_second_pair_f16_refused(self):
        with self.assertRaises(rdna2spirv.Unsupported) as cm:
            support.translate_lines("ps_i16", ["v_interp_p1ll_f16 v4, v0, attr1.x",
                                               "v_interp_p2_f16 v4, v9, attr1.x, v4",
                                               "exp mrt0 v4, v4, v4, v4 done vm",
                                               "s_endpgm"])
        self.assertIn("v_interp_p2_f16 J source v9", str(cm.exception))

    def test_mov_vertex_select_translates(self):
        # p10/p20 are modelled as P1 - P0 / P2 - P0 (test_interp_mov_delta.py); only an unknown select is refused.
        for sel in ("p10", "p20"):
            with self.subTest(sel=sel):
                spv = support.translate_lines("ps_m", [f"v_interp_mov_f32_e32 v2, {sel}, attr1.x",
                                                       "exp mrt0 v2, v2, v2, v2 done vm",
                                                       "s_endpgm"])
                self.assertIn("OpDecorate %in_attr1 PerVertexKHR", spv)

    def test_copy_of_pair_translates_and_computes(self):
        for chain in (["v_mov_b32_e32 v6, v1"],                    # the corpus form: J read through a copy
                      ["v_mov_b32_e32 v6, v1", "v_mov_b32_e32 v7, v6"]):   # a chain of plain copies
            with self.subTest(chain=len(chain)):
                src = chain[-1].split()[1].rstrip(",")
                lines = chain + ["v_interp_p1_f32_e32 v2, v0, attr0.x",
                                 f"v_interp_p2_f32_e32 v2, {src}, attr0.x",
                                 "exp mrt0 v2, v2, v2, v2 done vm",
                                 "s_endpgm"]
                spv = support.translate_lines("ps_copy", lines)
                words = support.mrt0_words(spv, inputs={"%in_attr0": ATTR0})
                self.assertEqual(words, [f32_bits(ATTR0[0])] * 4)

    def test_copy_written_again_is_computed(self):
        # Two writes (the copy, then an add) mean the register no longer provably holds the pair, so
        # the guard would refuse it -- but the computed-I/J model represents it: the operand load
        # reads the value the add wrote. J' = I + I doubles the second weight.
        spv = support.translate_lines("ps_copy2", ["v_mov_b32_e32 v6, v1",
                                                   "v_add_f32_e32 v6, v0, v0",
                                                   "v_interp_p1_f32_e32 v2, v0, attr0.x",
                                                   "v_interp_p2_f32_e32 v2, v6, attr0.x",
                                                   "exp mrt0 v2, v2, v2, v2 done vm",
                                                   "s_endpgm"])
        words = support.mrt0_words(spv, inputs=support.varyings(spv, {0: WORDS0}, BARY))
        i, j = BARY[1], 2 * BARY[1]
        exp = VERTS0[0][0] + i * (VERTS0[1][0] - VERTS0[0][0]) + j * (VERTS0[2][0] - VERTS0[0][0])
        self.assertEqual(words, [f32_bits(exp)] * 4)
        self.assertIn("PerVertexKHR", spv)

    def test_copy_with_modifiers_is_computed(self):
        # A dpp mov is not a plain copy, so the single write does not establish the barycentric --
        # but it is still a write of the operand register, and the computed-I/J model loads whatever
        # value the program put there. The pair translates; the weights are the wave-wide dpp
        # results, so this asserts translation (and the per-vertex read), not single-lane words.
        spv = support.translate_lines("ps_copy3", ["v_mov_b32_dpp v6, v1 row_shl:1 row_mask:0xf bank_mask:0xf",
                                                   "v_interp_p1_f32_e32 v2, v0, attr0.x",
                                                   "v_interp_p2_f32_e32 v2, v6, attr0.x",
                                                   "exp mrt0 v2, v2, v2, v2 done vm",
                                                   "s_endpgm"])
        self.assertIn("OpEntryPoint Fragment", spv)
        self.assertIn("PerVertexKHR", spv)

    def test_swapped_and_same_register_pairs_take_the_named_weights(self):
        # SHADER-659: p1 weights P10 and p2 weights P20 with the value their source holds, so naming the
        # pair the other way round (or naming one coordinate twice) is the interpolation with those
        # weights, not a refusal (measured on gfx1036 with runtime/test/shaders/ps_interp_swapped_pair.asm).
        # attr0.z is (1, 3, 5) at the three vertices: P10 = 2, P20 = 4, and with BARY = (.25, .25, .5)
        # the four role pairs give four different numbers.
        bary = {"i": BARY[1], "j": BARY[2]}
        for role in ((0, 1), (1, 0), (1, 1), (0, 0)):
            with self.subTest(p1=role[0], p2=role[1]):
                w1, w2 = (bary["i"] if role[0] == 0 else bary["j"],
                          bary["i"] if role[1] == 0 else bary["j"])
                want = VERTS0[0][2] + w1 * (VERTS0[1][2] - VERTS0[0][2]) + w2 * (VERTS0[2][2] - VERTS0[0][2])
                spv = support.translate_lines("ps_role", [f"v_interp_p1_f32_e32 v2, v{role[0]}, attr0.z",
                                                          f"v_interp_p2_f32_e32 v2, v{role[1]}, attr0.z",
                                                          "exp mrt0 v2, v2, v2, v2 done vm",
                                                          "s_endpgm"])
                words = support.mrt0_words(spv, inputs=support.varyings(spv, {0: WORDS0}, BARY))
                self.assertEqual(words, [f32_bits(want)] * 4)
                # Only the pair that reads the coordinates through the registers the hardware seeded needs
                # the per-vertex read; the standard pair keeps the interpolated input as it always did.
                self.assertEqual("PerVertexKHR" in spv, role != (0, 1))

    def test_opposite_role_read_without_its_partner_refused(self):
        # One op of the pair alone has no weight for the other axis, so it stays a refusal (SHADER-659
        # models pairs; unpaired reads are a separate item).
        for op, other, msg in (("v_interp_p1_f32_e32", "p2", "v_interp_p1_f32 without its p2 partner"),
                               ("v_interp_p2_f32_e32", "p1", "v_interp_p2_f32 without its p1 partner")):
            src = 1 if op.startswith("v_interp_p1") else 0
            with self.subTest(op=op):
                with self.assertRaises(rdna2spirv.Unsupported) as cm:
                    support.translate_lines("ps_lonely", [f"{op} v2, v{src}, attr0.z",
                                                         "exp mrt0 v2, v2, v2, v2 done vm",
                                                         "s_endpgm"])
                self.assertIn(msg, str(cm.exception))

    def test_copy_carries_the_coordinate_its_root_holds(self):
        # A copy of the pair stands for the coordinate its root holds, in either role: v6 = v1 gives the
        # p1 the J weight, v6 = v0 gives the p2 the I weight (the other op of the pair still reads v1/v0).
        for chain, p1, p2, w1, w2 in ((["v_mov_b32_e32 v6, v1"], "v6", "v1", BARY[2], BARY[2]),
                                      (["v_mov_b32_e32 v6, v0"], "v0", "v6", BARY[1], BARY[1])):
            with self.subTest(root=chain[0]):
                want = VERTS0[0][2] + w1 * (VERTS0[1][2] - VERTS0[0][2]) + w2 * (VERTS0[2][2] - VERTS0[0][2])
                spv = support.translate_lines("ps_rolc", chain + [f"v_interp_p1_f32_e32 v2, {p1}, attr0.z",
                                                                  f"v_interp_p2_f32_e32 v2, {p2}, attr0.z",
                                                                  "exp mrt0 v2, v2, v2, v2 done vm",
                                                                  "s_endpgm"])
                words = support.mrt0_words(spv, inputs=support.varyings(spv, {0: WORDS0}, BARY))
                self.assertEqual(words, [f32_bits(want)] * 4)

    def test_copy_in_matching_roles_translates_and_computes(self):
        # Vonsek measurement row 1: copies of untouched v0/v1, one as a chain, feed p1's I and p2's
        # J -> (u, v).
        lines = ["v_mov_b32_e32 v6, v0", "v_mov_b32_e32 v7, v6", "v_mov_b32_e32 v8, v1",
                 "v_interp_p1_f32_e32 v2, v7, attr0.x",
                 "v_interp_p2_f32_e32 v2, v8, attr0.x",
                 "exp mrt0 v2, v2, v2, v2 done vm",
                 "s_endpgm"]
        spv = support.translate_lines("ps_rolk", lines)
        words = support.mrt0_words(spv, inputs={"%in_attr0": ATTR0})
        self.assertEqual(words, [f32_bits(ATTR0[0])] * 4)

    def test_copy_saves_pair_from_later_zero(self):
        # Vonsek measurement row 2: copy v0/v1 out first, then zero them, read through the copies
        # -> (u, v): the copy saved the value, a write to the source after the copy does not
        # disqualify it.
        lines = ["v_mov_b32_e32 v6, v0", "v_mov_b32_e32 v7, v1",
                 "v_mov_b32_e32 v0, 0", "v_mov_b32_e32 v1, 0",
                 "v_interp_p1_f32_e32 v2, v6, attr0.x",
                 "v_interp_p2_f32_e32 v2, v7, attr0.x",
                 "exp mrt0 v2, v2, v2, v2 done vm",
                 "s_endpgm"]
        spv = support.translate_lines("ps_rols", lines)
        words = support.mrt0_words(spv, inputs={"%in_attr0": ATTR0})
        self.assertEqual(words, [f32_bits(ATTR0[0])] * 4)

    def test_copy_of_overwritten_source_is_computed(self):
        # Vonsek measurement row 3: zeroing v0/v1 BEFORE the copy means the copy carries the
        # computed (0) value, not the barycentric pair — hardware renders (0,0) = P0 — so the copy
        # no longer qualifies for the guard. The computed-I/J model represents it exactly: the
        # operand loads read the zeros the program wrote, and the weights (0, 0) pick vertex 0.
        # (The corpus's one copy PS copies v1 while it is still unwritten, then overwrites it: the
        # saved-copy case above, which keeps translating untouched by this.)
        spv = support.translate_lines("ps_rolz", ["v_mov_b32_e32 v0, 0", "v_mov_b32_e32 v1, 0",
                                                  "v_mov_b32_e32 v6, v0", "v_mov_b32_e32 v7, v1",
                                                  "v_interp_p1_f32_e32 v2, v6, attr0.x",
                                                  "v_interp_p2_f32_e32 v2, v7, attr0.x",
                                                  "exp mrt0 v2, v2, v2, v2 done vm",
                                                  "s_endpgm"])
        words = support.mrt0_words(spv, inputs=support.varyings(spv, {0: WORDS0}, BARY))
        self.assertEqual(words, [f32_bits(VERTS0[0][0])] * 4)   # weights (0, 0): the provoking vertex
        self.assertIn("PerVertexKHR", spv)

    def test_standard_pairs_and_p0_mov_unchanged(self):
        spv = support.translate_lines("ps_std", ["v_interp_p1_f32_e32 v2, v0, attr0.x",
                                                 "v_interp_p2_f32_e32 v2, v1, attr0.x",
                                                 "v_interp_mov_f32_e32 v3, p0, attr1.x",
                                                 "exp mrt0 v2, v3, v3, v3 done vm",
                                                 "s_endpgm"])
        words = support.mrt0_words(spv, inputs={"%in_attr0": ATTR0, "%in_attr1": (FLAT_ATTR1_X,) * 4})
        self.assertEqual(words, [f32_bits(ATTR0[0]), FLAT_ATTR1_X, FLAT_ATTR1_X, FLAT_ATTR1_X])
        self.assertIn("OpDecorate %in_attr1 Flat", spv)      # mov-only input stays Flat
        self.assertNotIn("OpDecorate %in_attr0 Flat", spv)

    def test_f16_standard_forms_still_translate(self):
        spv = support.translate_lines("ps_f16", ["v_interp_p1ll_f16 v4, v0, attr1.x",
                                                 "v_interp_p2_f16 v4, v1, attr1.x, v4",
                                                 "exp mrt0 v4, v4, v4, v4 done vm",
                                                 "s_endpgm"])
        self.assertIn("OpEntryPoint Fragment", spv)

    def test_guard_uses_the_copy_analysis(self):
        # Mutation check: with interp_copy_sources returning nothing, the copy case no longer takes
        # the guard's centre-interpolated path; it falls through to the computed-I/J model
        # (PerVertexKHR + operand loads), proving the acceptance above really flows through the
        # tracking and not through a vacuous guard. Either path is exact for a real copy.
        lines = ["v_mov_b32_e32 v6, v1",
                 "v_interp_p1_f32_e32 v2, v0, attr0.x",
                 "v_interp_p2_f32_e32 v2, v6, attr0.x",
                 "exp mrt0 v2, v2, v2, v2 done vm",
                 "s_endpgm"]
        self.assertNotIn("PerVertexKHR", support.translate_lines("ps_vc", lines))
        orig = rdna2spirv.interp_copy_sources
        rdna2spirv.interp_copy_sources = lambda insns: {}
        try:
            self.assertIn("PerVertexKHR", support.translate_lines("ps_vc", lines))
        finally:
            rdna2spirv.interp_copy_sources = orig


if __name__ == "__main__":
    unittest.main()
