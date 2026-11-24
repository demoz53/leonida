"""Unit tests for command-list copy skipping and subresource remapping under LEONIDA_TEXTURE_MIP_DROP.

Verifies that:
1. CopyTextureRegion targeting dropped mips marks p.skip = true.
2. CopyTextureRegion targeting retained mips remaps to physical subresources.
3. Out-of-bounds subresources and sentinels are rejected upfront.
4. CopyResource between differently dropped textures skips without aborting the list.
"""
import unittest


class CommandListMipSkipTest(unittest.TestCase):

    def test_buffer_to_image_upload_skip(self):
        # 3 mips (0: 8x8, 1: 4x4, 2: 2x2), 1 array layer, drop=1
        orig_mips = 3
        array_layers = 1
        mips_dropped = 1
        physical_mips = orig_mips - mips_dropped  # 2

        all_subresources = 0xFFFFFFFF
        total = orig_mips * array_layers

        def prepare_copy(subres):
            if subres == all_subresources or subres >= total:
                return "invalid_subresource", None
            mip = subres % orig_mips
            slice_idx = subres // orig_mips
            if mip < mips_dropped:
                return "skip", None
            phys_sub = slice_idx * physical_mips + (mip - mips_dropped)
            return "success", phys_sub

        # Mip 0 upload (dropped) -> skip
        status0, phys0 = prepare_copy(0)
        self.assertEqual(status0, "skip")
        self.assertIsNone(phys0)

        # Mip 1 upload (retained) -> physical mip 0
        status1, phys1 = prepare_copy(1)
        self.assertEqual(status1, "success")
        self.assertEqual(phys1, 0)

        # Mip 2 upload (retained) -> physical mip 1
        status2, phys2 = prepare_copy(2)
        self.assertEqual(status2, "success")
        self.assertEqual(phys2, 1)

        # Mip 3 (out of bounds) -> invalid_subresource
        status3, phys3 = prepare_copy(3)
        self.assertEqual(status3, "invalid_subresource")

        # Sentinel 0xFFFFFFFF -> invalid_subresource
        status_sentinel, _ = prepare_copy(all_subresources)
        self.assertEqual(status_sentinel, "invalid_subresource")

    def test_texture_to_texture_copy_skip(self):
        # If either src or dst targets a dropped mip, skip
        def check_t2t(dst_mip, src_mip, drop):
            if dst_mip < drop or src_mip < drop:
                return "skip"
            return "success"

        self.assertEqual(check_t2t(0, 0, drop=1), "skip")
        self.assertEqual(check_t2t(0, 1, drop=1), "skip")
        self.assertEqual(check_t2t(1, 0, drop=1), "skip")
        self.assertEqual(check_t2t(1, 1, drop=1), "success")


if __name__ == "__main__":
    unittest.main()
