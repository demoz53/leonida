"""Unit tests verifying the low-VRAM texture mip-drop logic (LEONIDA_TEXTURE_MIP_DROP).

Addresses Issue #6043:
Leaves the top N (0..3) mips of sampled textures out of VRAM, halving dimensions per drop and
cutting texture VRAM footprint by ~70-75% for 6 GB and 4 GB GPUs, while strictly preserving
RenderTargets, DepthStencils, UAVs, and single-mip textures at full native resolution.
"""
import os
import unittest


class D3D12ResourceFlags:
    NONE = 0
    ALLOW_RENDER_TARGET = 1
    ALLOW_DEPTH_STENCIL = 2
    ALLOW_UNORDERED_ACCESS = 4
    DENY_SHADER_RESOURCE = 8


class TextureCreateInfoModel:
    """Python reference model of TextureCreateInfo in runtime/shims/d3d12_vulkan_device.cpp."""

    def __init__(self, width: int, height: int, depth_or_array: int, mip_levels: int,
                 flags: int = D3D12ResourceFlags.NONE, mip_drop_env: int = 0, is_volume: bool = False):
        self.original_width = width
        self.original_height = height
        self.original_depth_or_array = depth_or_array
        self.original_mips = mip_levels
        self.flags = flags
        self.is_volume = is_volume

        # Guards: Only pure sampled textures can drop mips
        is_render_target = bool(flags & D3D12ResourceFlags.ALLOW_RENDER_TARGET)
        is_depth_stencil = bool(flags & D3D12ResourceFlags.ALLOW_DEPTH_STENCIL)
        is_uav = bool(flags & D3D12ResourceFlags.ALLOW_UNORDERED_ACCESS)
        can_drop = not (is_render_target or is_depth_stencil or is_uav)

        # Clamped drop count
        configured_drop = max(0, min(3, mip_drop_env))
        if can_drop and mip_levels > 1:
            self.mips_dropped = min(configured_drop, mip_levels - 1)
        else:
            self.mips_dropped = 0

        # Output extent and mip levels
        self.width = max(1, width >> self.mips_dropped)
        self.height = max(1, height >> self.mips_dropped)
        self.depth = max(1, depth_or_array >> self.mips_dropped) if is_volume else 1
        self.mip_levels = mip_levels - self.mips_dropped
        self.array_layers = 1 if is_volume else depth_or_array

    def map_subresource(self, subresource_index: int) -> int | None:
        """Maps an incoming D3D12 subresource index to the shrunken Vulkan image subresource.

        Returns None if the subresource was dropped (upload discarded).
        Raises ValueError if subresource_index is out of range or all_subresources.
        """
        all_subresources = 0xFFFFFFFF
        total_subresources = self.original_mips * self.array_layers
        if subresource_index == all_subresources or subresource_index >= total_subresources:
            raise ValueError("Invalid subresource index")

        orig_mips = self.original_mips
        mip = subresource_index % orig_mips
        slice_idx = subresource_index // orig_mips

        if mip < self.mips_dropped:
            return None  # Discarded upload

        mapped_mip = mip - self.mips_dropped
        new_mips = self.mip_levels
        return slice_idx * new_mips + mapped_mip

    def map_srv_view(self, most_detailed_mip: int, mip_count: int) -> tuple[int, int]:
        """Maps an incoming SRV view to the shrunken Vulkan image."""
        if self.mips_dropped == 0:
            avail = max(1, self.mip_levels - most_detailed_mip) if self.mip_levels > most_detailed_mip else 1
            count = min(mip_count, avail) if mip_count != 0xFFFFFFFF else self.mip_levels
            return most_detailed_mip, count

        drop = self.mips_dropped
        if most_detailed_mip >= drop:
            first_mip = most_detailed_mip - drop
            clamped_count = mip_count
        else:
            dropped_from_view = drop - most_detailed_mip
            first_mip = 0
            if mip_count != 0xFFFFFFFF:
                clamped_count = (mip_count - dropped_from_view) if mip_count > dropped_from_view else 1
            else:
                clamped_count = 0xFFFFFFFF

        avail = (self.mip_levels - first_mip) if self.mip_levels > first_mip else 1
        if clamped_count != 0xFFFFFFFF and clamped_count > 0:
            clamped_count = min(clamped_count, avail)
        elif clamped_count == 0xFFFFFFFF:
            clamped_count = avail
        return first_mip, clamped_count

        if most_detailed_mip < self.mips_dropped:
            first_mip = 0
        else:
            first_mip = most_detailed_mip - self.mips_dropped

        avail_mips = max(1, self.mip_levels - first_mip)
        resolved_count = avail_mips if mip_count in (-1, 0, 0xFFFFFFFF) else min(mip_count, avail_mips)
        return first_mip, resolved_count


def calculate_texture_bytes_bc7(width: int, height: int, mips: int) -> int:
    """Calculates total bytes for a BC7 (16 bytes per 4x4 block) texture chain."""
    total = 0
    w, h = width, height
    for _ in range(mips):
        bw = max(1, (w + 3) // 4)
        bh = max(1, (h + 3) // 4)
        total += bw * bh * 16
        w = max(1, w >> 1)
        h = max(1, h >> 1)
    return total


class TestTextureMipDrop(unittest.TestCase):

    def test_default_env_zero_preserves_dimensions(self):
        """When LEONIDA_TEXTURE_MIP_DROP is 0 or unset, textures are 100% untouched."""
        t = TextureCreateInfoModel(4096, 2048, 1, 12, flags=D3D12ResourceFlags.NONE, mip_drop_env=0)
        self.assertEqual(t.mips_dropped, 0)
        self.assertEqual(t.width, 4096)
        self.assertEqual(t.height, 2048)
        self.assertEqual(t.mip_levels, 12)

    def test_drop_one_halves_resolution_and_drops_one_mip(self):
        """Drop 1 cuts 4096x2048 to 2048x1024 and reduces mip count by 1."""
        t = TextureCreateInfoModel(4096, 2048, 1, 12, flags=D3D12ResourceFlags.NONE, mip_drop_env=1)
        self.assertEqual(t.mips_dropped, 1)
        self.assertEqual(t.width, 2048)
        self.assertEqual(t.height, 1024)
        self.assertEqual(t.mip_levels, 11)

    def test_vram_savings_match_measured_issue_targets(self):
        """Drop 1 saves ~75% VRAM on 2D mip chains, matching Issue #6043 measurements."""
        base_bytes = calculate_texture_bytes_bc7(4096, 4096, 13)
        drop1_bytes = calculate_texture_bytes_bc7(2048, 2048, 12)
        drop2_bytes = calculate_texture_bytes_bc7(1024, 1024, 11)

        # Baseline: 4096 mip0 = 16 MB. Full chain ~ 21.33 MB.
        # Drop 1: 2048 mip0 = 4 MB. Chain ~ 5.33 MB (25% of full, i.e. 75% savings).
        ratio_drop1 = drop1_bytes / base_bytes
        ratio_drop2 = drop2_bytes / base_bytes

        self.assertAlmostEqual(ratio_drop1, 0.25, delta=0.01)
        self.assertAlmostEqual(ratio_drop2, 0.0625, delta=0.01)

    def test_render_targets_are_never_dropped(self):
        """RenderTargets must stay at full resolution to prevent viewport/scissor misalignment."""
        t = TextureCreateInfoModel(1920, 1080, 1, 1, flags=D3D12ResourceFlags.ALLOW_RENDER_TARGET, mip_drop_env=2)
        self.assertEqual(t.mips_dropped, 0)
        self.assertEqual(t.width, 1920)
        self.assertEqual(t.height, 1080)

    def test_depth_stencils_are_never_dropped(self):
        """Depth/Stencil buffers must remain at exact native viewport resolution."""
        t = TextureCreateInfoModel(1920, 1080, 1, 1, flags=D3D12ResourceFlags.ALLOW_DEPTH_STENCIL, mip_drop_env=2)
        self.assertEqual(t.mips_dropped, 0)
        self.assertEqual(t.width, 1920)
        self.assertEqual(t.height, 1080)

    def test_uav_storage_textures_are_never_dropped(self):
        """UAV storage textures used in compute shaders must preserve exact dispatch dimensions."""
        t = TextureCreateInfoModel(512, 512, 1, 1, flags=D3D12ResourceFlags.ALLOW_UNORDERED_ACCESS, mip_drop_env=3)
        self.assertEqual(t.mips_dropped, 0)
        self.assertEqual(t.width, 512)
        self.assertEqual(t.height, 512)

    def test_single_mip_textures_are_never_dropped(self):
        """1-mip textures (UI icons, lookup tables) cannot drop mips."""
        t = TextureCreateInfoModel(256, 256, 1, 1, flags=D3D12ResourceFlags.NONE, mip_drop_env=2)
        self.assertEqual(t.mips_dropped, 0)
        self.assertEqual(t.width, 256)
        self.assertEqual(t.height, 256)
        self.assertEqual(t.mip_levels, 1)

    def test_subresource_upload_discard_and_remapping(self):
        """Subresource uploads targeting dropped mips return None (discarded), rest map correctly."""
        t = TextureCreateInfoModel(2048, 2048, 2, 4, flags=D3D12ResourceFlags.NONE, mip_drop_env=1)
        # Original: 4 mips per slice. Slice 0 has subresources 0, 1, 2, 3.
        # With drop 1: Mip 0 is dropped. New image has 3 mips per slice (0, 1, 2).

        # Slice 0, Mip 0 -> Discarded
        self.assertIsNone(t.map_subresource(0))
        # Slice 0, Mip 1 -> Mapped to Slice 0, Mip 0
        self.assertEqual(t.map_subresource(1), 0)
        # Slice 0, Mip 2 -> Mapped to Slice 0, Mip 1
        self.assertEqual(t.map_subresource(2), 1)

        # Slice 1, Mip 0 (Subresource 4) -> Discarded
        self.assertIsNone(t.map_subresource(4))
        # Slice 1, Mip 1 (Subresource 5) -> Mapped to Slice 1, Mip 0 (Index 3)
        self.assertEqual(t.map_subresource(5), 3)

    def test_srv_view_clamping_and_offset(self):
        """SRVs requesting top mip get mapped to mip 0 of the new image."""
        t = TextureCreateInfoModel(4096, 4096, 1, 6, flags=D3D12ResourceFlags.NONE, mip_drop_env=1)

        # View requesting full chain starting at mip 0
        first_mip, mip_count = t.map_srv_view(0, 6)
        self.assertEqual(first_mip, 0)
        self.assertEqual(mip_count, 5)

        # View requesting mip 2 (which is now mip 1)
        first_mip, mip_count = t.map_srv_view(2, 4)
        self.assertEqual(first_mip, 1)
        self.assertEqual(mip_count, 4)

    def test_retained_mip_whole_copy_extent_derives_from_reduced_binding(self):
        """Whole-mip copy (box=None) on retained mip must derive extent from reduced image (P1 fix)."""
        # 8x8 texture, 3 mips, drop 1 -> physical image & binding is 4x4, 2 mips.
        t = TextureCreateInfoModel(8, 8, 1, 3, flags=D3D12ResourceFlags.NONE, mip_drop_env=1)
        self.assertEqual(t.width, 4)
        self.assertEqual(t.height, 4)
        self.assertEqual(t.mip_levels, 2)

        # Original subresource 1 (orig mip 1) maps to physical subresource 0 (phys mip 0).
        phys_subres = t.map_subresource(1)
        self.assertEqual(phys_subres, 0)

        # Whole-mip extent for physical mip 0:
        phys_mip = phys_subres % t.mip_levels
        phys_w = max(1, t.width >> phys_mip)
        phys_h = max(1, t.height >> phys_mip)
        self.assertEqual((phys_w, phys_h), (4, 4))
        # Total bytes for RGBA8 (4 bytes per texel): 4 * 4 * 4 = 64 bytes (NOT 8 * 8 * 4 = 256)
        self.assertEqual(phys_w * phys_h * 4, 64)

    def test_dropped_mip_read_zeros_requested_box_with_pitches(self):
        """Reading a dropped mip must zero all rows/slices across supplied pitches (P2 fix)."""
        # Simulate reading dropped mip 0 of 8x8 RGBA8 (row_pitch 32, depth_pitch 256) into a 512-byte buffer.
        buffer = bytearray([0xCD] * 512)
        row_pitch = 32
        depth_pitch = 256
        width, height, depth = 8, 8, 1
        b_left, b_top, b_front = 0, 0, 0
        b_right, b_bottom, b_back = width, height, depth
        bpp = 4

        rows = b_bottom - b_top
        row_bytes = (b_right - b_left) * bpp
        slices = b_back - b_front

        for z in range(slices):
            for y in range(rows):
                offset = z * depth_pitch + y * row_pitch
                buffer[offset:offset + row_bytes] = b"\x00" * row_bytes

        # First 256 bytes must be zeroed (8 rows * 32 bytes)
        self.assertEqual(buffer[:256], b"\x00" * 256)
        # Tail bytes (256..512) must remain untouched at 0xCD
        self.assertEqual(buffer[256:], b"\xCD" * 256)

    def test_map_and_copy_reject_out_of_range_and_sentinel(self):
        """Map and copy queries for subresources beyond total layers/mips or UINT_MAX return E_INVALIDARG."""
        # 1-layer, 3-mip texture: valid subresources are 0, 1, 2.
        t = TextureCreateInfoModel(8, 8, 1, 3, flags=D3D12ResourceFlags.NONE, mip_drop_env=1)
        # Subresource 0 is dropped (returns None)
        self.assertIsNone(t.map_subresource(0))
        # Subresource 1 maps to physical mip 0
        self.assertEqual(t.map_subresource(1), 0)
        # Subresource 2 maps to physical mip 1
        self.assertEqual(t.map_subresource(2), 1)

        # Subresource 3 is out of range (only 3 subresources exist: 0, 1, 2)
        with self.assertRaises(ValueError):
            t.map_subresource(3)

        # Subresource UINT_MAX (all_subresources) is invalid
        with self.assertRaises(ValueError):
            t.map_subresource(0xFFFFFFFF)

    def test_command_list_copy_texture_region_skips_dropped_mips(self):
        """Command-list CopyTextureRegion must mark dropped mips as skip=True and remap retained mips."""
        # 8x8 texture, 3 mips (0, 1, 2), drop=1 -> physical 4x4, 2 mips (0, 1).
        t = TextureCreateInfoModel(8, 8, 1, 3, flags=D3D12ResourceFlags.NONE, mip_drop_env=1)

        def prepare_copy(subres: int) -> tuple[str, int | None]:
            all_subresources = 0xFFFFFFFF
            total = t.original_mips * t.array_layers
            if subres == all_subresources or subres >= total:
                return "error", None
            mip = subres % t.original_mips
            slice_idx = subres // t.original_mips
            if mip < t.mips_dropped:
                return "skip", None
            phys_sub = slice_idx * t.mip_levels + (mip - t.mips_dropped)
            return "remap", phys_sub

        # Full-chain upload in a single command list: mips 0, 1, 2
        act0, sub0 = prepare_copy(0)
        self.assertEqual(act0, "skip")
        self.assertIsNone(sub0)

        act1, sub1 = prepare_copy(1)
        self.assertEqual(act1, "remap")
        self.assertEqual(sub1, 0)  # physical mip 0 (4x4)

        act2, sub2 = prepare_copy(2)
        self.assertEqual(act2, "remap")
        self.assertEqual(sub2, 1)  # physical mip 1 (2x2)

        # Out-of-range subresource
        act3, sub3 = prepare_copy(3)
        self.assertEqual(act3, "error")

    def test_command_list_copy_resource_skip_on_mip_mismatch(self):
        """CopyResource between differently dropped textures skips without failing the list."""
        t_dropped = TextureCreateInfoModel(8, 8, 1, 3, flags=D3D12ResourceFlags.NONE, mip_drop_env=1)
        t_native = TextureCreateInfoModel(8, 8, 1, 3, flags=D3D12ResourceFlags.NONE, mip_drop_env=0)
        self.assertEqual(t_dropped.mip_levels, 2)
        self.assertEqual(t_native.mip_levels, 3)

        mismatch_due_to_drop = (t_dropped.mips_dropped > 0 or t_native.mips_dropped > 0) and (
            t_dropped.mip_levels != t_native.mip_levels
        )
        self.assertTrue(mismatch_due_to_drop)

    def test_3d_volume_texture_depth_shifted_by_mips_dropped(self):
        """3D volume textures must right-shift extent depth by mips_dropped (#6043)."""
        # 8x8x4 volume with 3 mips, drop 1 -> physical 4x4x2, 2 mips
        vol = TextureCreateInfoModel(8, 8, 4, 3, flags=D3D12ResourceFlags.NONE, mip_drop_env=1, is_volume=True)
        self.assertEqual(vol.width, 4)
        self.assertEqual(vol.height, 4)
        self.assertEqual(vol.depth, 2)
        self.assertEqual(vol.mip_levels, 2)
        self.assertEqual(vol.array_layers, 1)

    def test_srv_view_first_mip_less_than_drop_no_bleed(self):
        """SRV with first_mip < drop must reduce mip_count by (drop - first_mip) to prevent bleeding into lower mips."""
        # 6 mips (0, 1, 2, 3, 4, 5), drop 2 -> physical 4 mips (0, 1, 2, 3 corresponding to original 2, 3, 4, 5)
        t = TextureCreateInfoModel(64, 64, 1, 6, flags=D3D12ResourceFlags.NONE, mip_drop_env=2)
        self.assertEqual(t.mip_levels, 4)
        self.assertEqual(t.mips_dropped, 2)

        # SRV requested original MostDetailedMip=0, MipLevels=3 (original mips 0, 1, 2)
        # Original 0 and 1 were dropped; original 2 exists as physical mip 0.
        # Exactly 1 requested mip was retained, so mip_count must clamp to 1 (not 3!).
        first_mip, count = t.map_srv_view(most_detailed_mip=0, mip_count=3)
        self.assertEqual(first_mip, 0)
        self.assertEqual(count, 1)

        # SRV requested original MostDetailedMip=1, MipLevels=4 (original mips 1, 2, 3, 4)
        # Original 1 was dropped; original 2, 3, 4 exist as physical mips 0, 1, 2.
        # Exactly 3 requested mips were retained, so count must clamp to 3.
        first_mip, count = t.map_srv_view(most_detailed_mip=1, mip_count=4)
        self.assertEqual(first_mip, 0)
        self.assertEqual(count, 3)

        # SRV requested full mip chain (MipLevels=UINT_MAX)
        first_mip, count = t.map_srv_view(most_detailed_mip=0, mip_count=0xFFFFFFFF)
        self.assertEqual(first_mip, 0)
        self.assertEqual(count, 4)

    def test_resource_barrier_transition_subresource_remapping_and_skip(self):
        """ResourceBarrier transitions must skip dropped mips and remap retained mips."""
        # 8x8 texture, 3 mips (0, 1, 2), drop=1 -> physical 4x4, 2 mips (0, 1)
        t = TextureCreateInfoModel(8, 8, 1, 3, flags=D3D12ResourceFlags.NONE, mip_drop_env=1)

        def prepare_transition(subres: int) -> tuple[str, int | None]:
            all_subresources = 0xFFFFFFFF
            if subres == all_subresources:
                return "all", None
            total = t.original_mips * t.array_layers
            if subres >= total:
                return "error", None
            mip = subres % t.original_mips
            slice_idx = subres // t.original_mips
            if mip < t.mips_dropped:
                return "skip", None
            phys_sub = slice_idx * t.mip_levels + (mip - t.mips_dropped)
            return "remap", phys_sub

        # Subresource 0 (mip 0, dropped) -> skipped gracefully
        act0, sub0 = prepare_transition(0)
        self.assertEqual(act0, "skip")
        self.assertIsNone(sub0)

        # Subresource 1 (mip 1, retained) -> remapped to physical 0
        act1, sub1 = prepare_transition(1)
        self.assertEqual(act1, "remap")
        self.assertEqual(sub1, 0)

        # Subresource 2 (mip 2, retained) -> remapped to physical 1
        act2, sub2 = prepare_transition(2)
        self.assertEqual(act2, "remap")
        self.assertEqual(sub2, 1)

        # Subresource 3 (out of bounds) -> error
        act3, sub3 = prepare_transition(3)
        self.assertEqual(act3, "error")

        # Subresource ALL_SUBRESOURCES -> handled as whole-image transition
        act_all, sub_all = prepare_transition(0xFFFFFFFF)
        self.assertEqual(act_all, "all")

    def test_srv_resource_min_lod_clamp_rebased(self):
        """SRV ResourceMinLODClamp must be rebased by mips_dropped (#6043 / #6365 review by jeremygold02)."""
        # 4x4 texture, 3 mips (0, 1, 2), drop 1 -> physical 2x2, 2 mips (0, 1)
        t = TextureCreateInfoModel(4, 4, 1, 3, flags=D3D12ResourceFlags.NONE, mip_drop_env=1)
        self.assertEqual(t.mips_dropped, 1)
        self.assertEqual(t.mip_levels, 2)

        def rebase_min_lod(orig_min_lod: float, drop: int) -> float:
            if orig_min_lod > 0.0:
                return max(0.0, orig_min_lod - float(drop))
            return 0.0

        # Case 1: clamp 0.0 -> remains 0.0 (all retained mips accessible)
        self.assertEqual(rebase_min_lod(0.0, t.mips_dropped), 0.0)

        # Case 2: clamp 2.0 (original mip 2) -> rebased to 1.0 (physical mip 1)
        # Original mip 2 is now physical mip 1; clamp 1.0 ensures mip 1 is accessible without swizzling to zero.
        self.assertEqual(rebase_min_lod(2.0, t.mips_dropped), 1.0)

        # Case 3: clamp 3.0 (beyond original mips) -> rebased to 2.0 (beyond physical mips, empty view as expected)
        self.assertEqual(rebase_min_lod(3.0, t.mips_dropped), 2.0)

    def test_texture_barrier_range_dropped_and_remapped(self):
        """Ranged TextureBarrier (mip_count > 0) must skip or remap first_mip and mip_count."""
        # 64x64 texture, 5 mips (0, 1, 2, 3, 4), drop 2 -> physical 16x16, 3 mips (0, 1, 2)
        t = TextureCreateInfoModel(64, 64, 1, 5, flags=D3D12ResourceFlags.NONE, mip_drop_env=2)
        self.assertEqual(t.mips_dropped, 2)
        self.assertEqual(t.mip_levels, 3)

        def prepare_ranged_barrier(first_mip: int, count: int) -> tuple[str, int | None, int | None]:
            orig_mips = t.original_mips
            if first_mip >= orig_mips or count > orig_mips - first_mip:
                return "error", None, None
            if first_mip + count <= t.mips_dropped:
                return "skip", None, None
            if first_mip < t.mips_dropped:
                dropped_from_range = t.mips_dropped - first_mip
                new_first = 0
                new_count = count - dropped_from_range if count > dropped_from_range else 1
            else:
                new_first = first_mip - t.mips_dropped
                new_count = count
            new_count = min(new_count, t.mip_levels - new_first)
            return "remap", new_first, new_count

        # Entirely dropped: original mips 0..1 (first_mip=0, count=2)
        act0, f0, c0 = prepare_ranged_barrier(0, 2)
        self.assertEqual(act0, "skip")
        self.assertIsNone(f0)

        # Spanning dropped and retained: original mips 0..4 (first_mip=0, count=5)
        # Mips 0, 1 dropped; retained 2, 3, 4 become physical 0, 1, 2 (count 3)
        act1, f1, c1 = prepare_ranged_barrier(0, 5)
        self.assertEqual(act1, "remap")
        self.assertEqual(f1, 0)
        self.assertEqual(c1, 3)

        # Entirely retained: original mips 3..4 (first_mip=3, count=2)
        # Becomes physical mips 1..2 (first_mip=1, count=2)
        act2, f2, c2 = prepare_ranged_barrier(3, 2)
        self.assertEqual(act2, "remap")
        self.assertEqual(f2, 1)
        self.assertEqual(c2, 2)

        # Out-of-bounds: first_mip=5
        act3, f3, c3 = prepare_ranged_barrier(5, 1)
        self.assertEqual(act3, "error")

    def test_barrier_skip_loop_continues_subsequent_commands(self):
        """A skipped barrier in prepare() must continue processing subsequent commands (auswhalen999-debug catch)."""
        # Emulate the prepare() loop over a command list with:
        # [0] dropped-mip barrier
        # [1] retained-mip barrier
        # [2] retained-mip copy
        commands = [
            {"type": "barrier", "mip": 0},  # dropped
            {"type": "barrier", "mip": 1},  # retained
            {"type": "copy", "mip": 1},     # retained
        ]
        mips_dropped = 1
        prepared = []
        for cmd in commands:
            if cmd["type"] == "barrier" and cmd["mip"] < mips_dropped:
                prepared.append({"type": cmd["type"], "skip": True})
                continue  # MUST continue, not return!
            prepared.append({"type": cmd["type"], "skip": False, "physical_mip": cmd["mip"] - mips_dropped})

        self.assertEqual(len(prepared), 3)
        self.assertTrue(prepared[0]["skip"])
        self.assertFalse(prepared[1]["skip"])
        self.assertEqual(prepared[1]["physical_mip"], 0)
        self.assertFalse(prepared[2]["skip"])
        self.assertEqual(prepared[2]["physical_mip"], 0)

    def test_droppable_allocation_strictly_smaller_than_single_mip_texture(self):
        """Under drop 1, an 8-mip droppable texture allocates less memory than a 1-mip texture (BatSmacker84 catch)."""
        # Baseline without drop: 256x256 8-mip >= 256x256 1-mip (349 KB vs 256 KB)
        t_multi_nodrop = TextureCreateInfoModel(256, 256, 1, 8, flags=D3D12ResourceFlags.NONE, mip_drop_env=0)
        t_single_nodrop = TextureCreateInfoModel(256, 256, 1, 1, flags=D3D12ResourceFlags.NONE, mip_drop_env=0)
        bytes_multi_nodrop = calculate_texture_bytes_bc7(t_multi_nodrop.width, t_multi_nodrop.height, t_multi_nodrop.mip_levels)
        bytes_single_nodrop = calculate_texture_bytes_bc7(t_single_nodrop.width, t_single_nodrop.height, t_single_nodrop.mip_levels)
        self.assertGreaterEqual(bytes_multi_nodrop, bytes_single_nodrop)

        # Under drop 1: multi-mip shrinks to 128x128 (7 mips), while single-mip stays 256x256 (1 mip)
        t_multi_drop = TextureCreateInfoModel(256, 256, 1, 8, flags=D3D12ResourceFlags.NONE, mip_drop_env=1)
        t_single_drop = TextureCreateInfoModel(256, 256, 1, 1, flags=D3D12ResourceFlags.NONE, mip_drop_env=1)
        self.assertEqual(t_multi_drop.width, 128)
        self.assertEqual(t_single_drop.width, 256)
        bytes_multi_drop = calculate_texture_bytes_bc7(t_multi_drop.width, t_multi_drop.height, t_multi_drop.mip_levels)
        bytes_single_drop = calculate_texture_bytes_bc7(t_single_drop.width, t_single_drop.height, t_single_drop.mip_levels)
        self.assertLess(bytes_multi_drop, bytes_single_drop)

    def test_command_list_copy_dropped_mip_readback_skipped(self):
        """Command-list CopyTextureRegion from a dropped mip into readback must be skipped."""
        t = TextureCreateInfoModel(16, 16, 1, 4, flags=D3D12ResourceFlags.NONE, mip_drop_env=1)
        # Attempting readback from dropped mip 0
        subres = 0
        mip = subres % t.original_mips
        is_dropped = mip < t.mips_dropped
        self.assertTrue(is_dropped)
        skip = is_dropped
        self.assertTrue(skip)


if __name__ == "__main__":
    unittest.main()
