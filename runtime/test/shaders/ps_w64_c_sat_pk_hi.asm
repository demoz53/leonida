; SHADER-SATPK-HALVES tier C: v_sat_pk_u8_i16 keeps the high half of its VGPR, and here the high half is the raw lane
; number 0..63 (v_mbcnt_lo/hi of -1), so the export depends on how the wave is cut and tools/wave64_halves.py must call
; the shader tier C (it read the old destination of the write only after v_sat_pk left _FULL32_V16_PREFIXES). x, y,
; SRC0 and s as in ps_w64_a_sat_pk_hi (lo = 8x - 128, hi = 300 - 8y, s = sat8(hi) << 8 | sat8(lo)).
;   r0 = lane id << 16 | s   (v_sat_pk_u8_i16_e32 into lane id << 16 | 0xa5a5)
;   r1 = r0 >> 16            (the kept high half read back: the lane id)
;   r2 = r0 & 0xffff = s     (the same on any partition)
;   r3 = 0
; Run: vk_harness vs_ids.spv ps_w64_c_sat_pk_hi.spv out.raw with VKH_SUBGROUP_SIZE=64 and 32: r2 and r3 equal, r0 and
; r1 differ (on 32-lane subgroups the lane id is 0..31 twice).
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
v_mbcnt_lo_u32_b32 v10, -1, 0
v_mbcnt_hi_u32_b32 v10, -1, v10
v_lshl_or_b32 v6, v10, 16, 0xa5a5
v_sat_pk_u8_i16_e32 v6, v4
v_lshrrev_b32_e32 v7, 16, v6
v_and_b32_e32 v8, 0xffff, v6
v_mov_b32_e32 v9, 0
exp mrt0 v6, v7, v8, v9 done vm
s_endpgm
