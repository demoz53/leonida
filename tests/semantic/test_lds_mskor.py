"""Value tests for RDNA2 ds_mskor_b32 / ds_mskor_rtn_b32, run through spirv_mini_interp on the CPU.

RDNA2 ISA, "Data Share Instructions": tmp = M[A]; M[A] = (tmp & ~DATA) | DATA2; the _rtn_ form returns tmp in vdst.
Single-lane cases check values and register aliasing; a two-lane race checks the old values and final memory against
both possible serial orders. Standard library only.
"""
import os
import random
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.dont_write_bytecode = True
sys.path.insert(0, HERE)

from test_lds_atomics_values import FSSM, VSHARP, run_atomic  # noqa: E402  (also puts tools/ on sys.path)
import support  # noqa: E402
import spirv_mini_interp as si  # noqa: E402

M32 = 0xFFFFFFFF
VALUES = [0, M32, 0x12345678, 0xF0F0F0F0, 0x0000FFFF, 0x80000001]


def mskor(m, mask, data):
    return (m & ~mask | data) & M32


def contention_listing(returning=True, offset=0):
    """Two lanes replace each other's bit. An atomic execution ends in 1 or 2, never their union 3."""
    atomic = "ds_mskor_rtn_b32 v4, v1, v2, v3" if returning else "ds_mskor_b32 v1, v2, v3"
    return ["v_mov_b32_e32 v1, 0", "v_mov_b32_e32 v4, 0xdead",
            "s_mov_b32 exec_lo, 1", f"ds_write_b32 v1, v1 offset:{offset}", "s_waitcnt lgkmcnt(0)",
            "s_mov_b32 exec_lo, 3", "s_barrier",
            "v_lshlrev_b32_e32 v2, v0, 1", "v_xor_b32_e32 v3, 3, v2", f"{atomic} offset:{offset}",
            "s_waitcnt lgkmcnt(0)", "s_barrier", f"ds_read_b32 v5, v1 offset:{offset}",
            "s_waitcnt lgkmcnt(0)", "v_lshlrev_b32_e32 v6, 3, v0",
            "buffer_store_dword v4, v6, s[0:3], 0 offen",
            "buffer_store_dword v5, v6, s[0:3], 0 offen offset:4", "s_endpgm"]


def run_wave(body):
    spv = support.translate_lines("cs_mskor", body, fssm=FSSM)
    return si.run_workgroup(spv, 32, 32,
                            inputs_for=lambda i: {"%wg_id": (0, 0, 0), "%local_id": (i, 0, 0), "%sg_lane": i},
                            push_constants=VSHARP + [0] * 28, memory={})


class LdsMskor(unittest.TestCase):
    def test_returning_form(self):
        for m in VALUES:
            for mask in VALUES[::2]:
                for data in VALUES[1::2]:
                    with self.subTest(m=hex(m), mask=hex(mask), data=hex(data)):
                        got = run_atomic("ds_mskor_rtn_b32 v5, v1, v3, v4", m, data=(mask, data))
                        self.assertEqual(got, (m, mskor(m, mask, data)))

    def test_non_returning_form_leaves_vdst_alone(self):
        for m, mask, data in ((0x12345678, 0x0000FF00, 0x0000AB00), (M32, M32, 0), (0, 0, 0x55)):
            with self.subTest(m=hex(m)):
                self.assertEqual(run_atomic("ds_mskor_b32 v1, v3, v4", m, data=(mask, data)),
                                 (0xDEAD, mskor(m, mask, data)))

    def test_data_outside_the_mask_and_offset(self):
        # DATA2 bits outside DATA are ORed in too: (tmp & ~DATA) | DATA2, not a masked insert.
        self.assertEqual(run_atomic("ds_mskor_rtn_b32 v5, v1, v3, v4", 0x00FF00FF, data=(0x0000FFFF, 0xFFFF0000)),
                         (0x00FF00FF, 0xFFFF0000 | 0x00FF0000))
        self.assertEqual(run_atomic("ds_mskor_rtn_b32 v5, v1, v3, v4", 7, addr=0x20, offset=0x24, data=(3, 8)),
                         (7, 12))

    def test_seeded(self):
        rng = random.Random(0x3502)
        for _ in range(20):
            m, mask, data = rng.getrandbits(32), rng.getrandbits(32), rng.getrandbits(32)
            with self.subTest(m=hex(m), mask=hex(mask), data=hex(data)):
                self.assertEqual(run_atomic("ds_mskor_rtn_b32 v5, v1, v3, v4", m, data=(mask, data)),
                                 (m, mskor(m, mask, data)))

    def test_contending_updates_are_atomic(self):
        # run_workgroup interleaves one SPIR-V instruction per lane: split AND/OR clears both bits before either
        # lane sets one, incorrectly leaving 3. A CAS loser must retry from the winner's value. Accept either order.
        for returning in (False, True):
            for offset in (0, 12):
                with self.subTest(returning=returning, offset=offset):
                    mem = run_wave(contention_listing(returning, offset))
                    got = tuple(mem[i] for i in (0, 4, 8, 12))
                    expected = ((0, 1, 2, 1), (1, 2, 0, 2)) if returning else (
                        (0xDEAD, 1, 0xDEAD, 1), (0xDEAD, 2, 0xDEAD, 2))
                    self.assertIn(got, expected)

    def test_return_can_alias_address_mask_or_data(self):
        init, mask, data = 0x12345678, 0xFF, 0x555
        for reg in (1, 2, 3):
            with self.subTest(destination=reg):
                mem = run_wave(["s_mov_b32 exec_lo, 1", "v_mov_b32_e32 v1, 0x40",
                                f"v_mov_b32_e32 v2, {mask}", f"v_mov_b32_e32 v3, {data}",
                                f"v_mov_b32_e32 v4, {init}", "ds_write_b32 v1, v4", "s_waitcnt lgkmcnt(0)",
                                f"ds_mskor_rtn_b32 v{reg}, v1, v2, v3", "s_waitcnt lgkmcnt(0)",
                                "v_mov_b32_e32 v10, 0x40", "ds_read_b32 v5, v10", "s_waitcnt lgkmcnt(0)",
                                "v_mov_b32_e32 v6, 0", f"buffer_store_dword v{reg}, v6, s[0:3], 0 offen",
                                "buffer_store_dword v5, v6, s[0:3], 0 offen offset:4", "s_endpgm"])
                self.assertEqual((mem[0], mem[4]), (init, mskor(init, mask, data)))

    def test_inactive_lanes_keep_their_destination(self):
        mem = run_wave(["v_mov_b32_e32 v1, 0x40", "v_mov_b32_e32 v2, 7", "v_mov_b32_e32 v3, 3",
                        "v_mov_b32_e32 v4, 8", "v_mov_b32_e32 v5, 0xdead", "s_mov_b32 exec_lo, 1",
                        "ds_write_b32 v1, v2", "s_waitcnt lgkmcnt(0)", "ds_mskor_rtn_b32 v5, v1, v3, v4",
                        "s_waitcnt lgkmcnt(0)", "s_mov_b32 exec_lo, -1", "s_barrier", "ds_read_b32 v6, v1",
                        "s_waitcnt lgkmcnt(0)", "v_lshlrev_b32_e32 v7, 3, v0",
                        "buffer_store_dword v5, v7, s[0:3], 0 offen",
                        "buffer_store_dword v6, v7, s[0:3], 0 offen offset:4", "s_endpgm"])
        self.assertEqual([mem[8 * lane] for lane in range(32)], [7] + [0xDEAD] * 31)
        self.assertEqual([mem[8 * lane + 4] for lane in range(32)], [12] * 32)


if __name__ == "__main__":
    unittest.main()
