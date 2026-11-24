; GPU test for four 16-bit VALU rows that docs/design.md lists as not GPU-verified (their translations were
; CPU-checked and the instructions measured on hardware, but the translated SPIR-V never ran on a GPU):
;   v_add_nc_i16 / v_sub_nc_i16 (clamp, op_sel destination half), v_med3_i16 / v_max3_u16 / v_min3_u16,
;   v_min3_f16 / v_max3_f16, and the packed v_pk_add_u16 (clamp) / v_pk_mad_i16.
;
; Group 32x1x1, only lane 0 runs. Each destination is preset to 0xA5A5A5A5 so the half an op does not write shows
; up as 0xA5A5 (design.md "16-bit results": the unwritten half of D is kept). The words follow the design.md rows:
;
;   out[0]  v_add_nc_i16 clamp, 0x7FFF + 1: SClamp to 0x7FFF in the low half                       -> 0xA5A57FFF
;   out[1]  v_sub_nc_i16 clamp op_sel:[0,0,1], 0x8000 - 1: SClamp to 0x8000, written to the high half -> 0x8000A5A5
;   out[2]  v_add_nc_i16 without clamp, 0x7FFF + 1: wraps to 0x8000                                 -> 0xA5A58000
;   out[3]  v_med3_i16 op_sel:[1,0,0,0], S0 high half 0xFFFB (-5), 7, 3: the median, 3              -> 0xA5A50003
;   out[4]  low: v_max3_u16 (0xFFFB, 7, 3) = 0xFFFB; high (op_sel:[0,0,0,1]): v_min3_u16 of the same = 3
;                                                                                                   -> 0x0003FFFB
;   out[5]  low: v_min3_f16 (1.0, qNaN 0x7E00, -2.0) = -2.0 (NMin: a NaN yields the other operand);
;           high (op_sel:[0,0,0,1]): v_max3_f16 of the same = 1.0                                  -> 0x3C00C000
;   out[6]  v_pk_add_u16 clamp, 0xFFF0_0005 + 0x0020_0003: low 8, high 0x10010 saturates to 0xFFFF -> 0xFFFF0008
;   out[7]  v_pk_mad_i16, 0x0002_FFFF * 0x0003_0004 + 0x0001_0010: low -1*4+16 = 12, high 2*3+1 = 7
;                                                                                                   -> 0x0007000C
;
; Run: vk_compute_test cs_valu16_ext.spv 1 dump
;   -> out[0..7] = 0xA5A57FFF 0x8000A5A5 0xA5A58000 0xA5A50003
;                  0x0003FFFB 0x3C00C000 0xFFFF0008 0x0007000C
; group: 32,1,1
; mattr: +wavefrontsize32,-wavefrontsize64
v_cmpx_eq_u32_e32 0, v0

; --- v_add_nc_i16 / v_sub_nc_i16 ---
v_mov_b32_e32 v1, 0x7FFF
v_mov_b32_e32 v2, 1
v_mov_b32_e32 v10, 0xA5A5A5A5
v_add_nc_i16 v10, v1, v2 clamp
v_mov_b32_e32 v21, 0
buffer_store_dword v10, v21, s[0:3], 0 offen
v_mov_b32_e32 v1, 0x8000
v_mov_b32_e32 v10, 0xA5A5A5A5
v_sub_nc_i16 v10, v1, v2 op_sel:[0,0,1] clamp
v_mov_b32_e32 v21, 4
buffer_store_dword v10, v21, s[0:3], 0 offen
v_mov_b32_e32 v1, 0x7FFF
v_mov_b32_e32 v10, 0xA5A5A5A5
v_add_nc_i16 v10, v1, v2
v_mov_b32_e32 v21, 8
buffer_store_dword v10, v21, s[0:3], 0 offen

; --- 16-bit integer min3/max3/med3 ---
v_mov_b32_e32 v1, 0xFFFB1234
v_mov_b32_e32 v2, 7
v_mov_b32_e32 v3, 3
v_mov_b32_e32 v10, 0xA5A5A5A5
v_med3_i16 v10, v1, v2, v3 op_sel:[1,0,0,0]
v_mov_b32_e32 v21, 12
buffer_store_dword v10, v21, s[0:3], 0 offen
v_mov_b32_e32 v1, 0xFFFB
v_mov_b32_e32 v10, 0xA5A5A5A5
v_max3_u16 v10, v1, v2, v3
v_min3_u16 v10, v1, v2, v3 op_sel:[0,0,0,1]
v_mov_b32_e32 v21, 16
buffer_store_dword v10, v21, s[0:3], 0 offen

; --- f16 min3/max3 with a quiet NaN ---
v_mov_b32_e32 v1, 0x3C00
v_mov_b32_e32 v2, 0x7E00
v_mov_b32_e32 v3, 0xC000
v_mov_b32_e32 v10, 0xA5A5A5A5
v_min3_f16 v10, v1, v2, v3
v_max3_f16 v10, v1, v2, v3 op_sel:[0,0,0,1]
v_mov_b32_e32 v21, 20
buffer_store_dword v10, v21, s[0:3], 0 offen

; --- packed 16-bit integer ops ---
v_mov_b32_e32 v1, 0xFFF00005
v_mov_b32_e32 v2, 0x00200003
v_pk_add_u16 v10, v1, v2 clamp
v_mov_b32_e32 v21, 24
buffer_store_dword v10, v21, s[0:3], 0 offen
v_mov_b32_e32 v1, 0x0002FFFF
v_mov_b32_e32 v2, 0x00030004
v_mov_b32_e32 v3, 0x00010010
v_pk_mad_i16 v10, v1, v2, v3
v_mov_b32_e32 v21, 28
buffer_store_dword v10, v21, s[0:3], 0 offen
s_endpgm
