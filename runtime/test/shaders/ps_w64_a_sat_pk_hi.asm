; SHADER-SATPK-HALVES tier A (docs/design.md, "v_sat_pk_u8_i16"): v_sat_pk_u8_i16 writes the low half of its VGPR and
; keeps the high half, in each encoding that keeps it (e32, e64, DPP, SDWA dst_sel:WORD_0 with UNUSED_PRESERVE). Every
; word is a function of the pixel, x = floor(u * 64), y = floor(v * 64) on the 64x64 target of vs_ids, so the shader
; gives the same exports when its 64 lanes are cut into two 32-lane subgroups (tools/wave64_halves.py: tier A).
; SRC0 = hi << 16 | lo with lo = 8x - 128 (-128..376 across x) and hi = 300 - 8y (300..-204 across y), 16 bits each:
; both signed halves saturate below 0 and above 255 across the target. s = sat8(hi) << 8 | sat8(lo), where sat8 clamps
; a signed 16-bit value to 0..255 (the ISA pseudocode). p = 64 * y + x. Each destination is seeded with its own high
; half (and a low half the write must replace):
;   r0 = v_sat_pk_u8_i16_e32 into p << 16 | 0xa5a5:                    p << 16 | s
;   r1 = v_sat_pk_u8_i16_e64 into (0xff00 - p) << 16 | 0x1234:         (0xff00 - p) << 16 | s
;   r2 = v_sat_pk_u8_i16_dpp (quad_perm identity, all rows and banks)
;        into (p ^ 0x5555) << 16 | 0x0f0f:                              (p ^ 0x5555) << 16 | s
;   r3 = v_sat_pk_u8_i16_sdwa dst_sel:WORD_0 dst_unused:UNUSED_PRESERVE
;        into x << 24 | y << 16 | 0xffff:                               x << 24 | y << 16 | s
; The high halves are what the translator's write_lo16 keeps; expected on any Vulkan GPU, since the GPU runs the
; translated SPIR-V (this checks the translator, not the RDNA2 instruction itself).
; Run: vk_harness vs_ids.spv ps_w64_a_sat_pk_hi.spv out.raw, with VKH_SUBGROUP_SIZE=64 and 32 where both can be
; required: the two images equal each other and the words above.
; mattr: +wavefrontsize64,-wavefrontsize32
v_interp_p1_f32_e32 v2, v0, attr0.x
v_interp_p2_f32_e32 v2, v1, attr0.x
v_interp_p1_f32_e32 v3, v0, attr0.y
v_interp_p2_f32_e32 v3, v1, attr0.y
v_mul_f32_e32 v2, 0x42800000, v2
v_mul_f32_e32 v3, 0x42800000, v3
v_cvt_u32_f32_e32 v2, v2
v_cvt_u32_f32_e32 v3, v3
s_mov_b64 s[10:11], exec
v_lshlrev_b32_e32 v4, 3, v2
v_subrev_nc_u32_e32 v4, 0x80, v4
v_and_b32_e32 v4, 0xffff, v4
v_lshlrev_b32_e32 v5, 3, v3
v_sub_nc_u32_e32 v5, 0x12c, v5
v_lshl_or_b32 v4, v5, 16, v4
v_lshl_add_u32 v10, v3, 6, v2
v_lshl_or_b32 v6, v10, 16, 0xa5a5
v_sub_nc_u32_e32 v7, 0xff00, v10
v_lshl_or_b32 v7, v7, 16, 0x1234
v_xor_b32_e32 v8, 0x5555, v10
v_lshl_or_b32 v8, v8, 16, 0xf0f
v_lshlrev_b32_e32 v9, 8, v2
v_or_b32_e32 v9, v9, v3
v_lshl_or_b32 v9, v9, 16, 0xffff
v_sat_pk_u8_i16_e32 v6, v4
v_sat_pk_u8_i16_e64 v7, v4
v_sat_pk_u8_i16_dpp v8, v4 quad_perm:[0,1,2,3] row_mask:0xf bank_mask:0xf bound_ctrl:1
v_sat_pk_u8_i16_sdwa v9, v4 dst_sel:WORD_0 dst_unused:UNUSED_PRESERVE src0_sel:DWORD
exp mrt0 v6, v7, v8, v9 done vm
s_endpgm
