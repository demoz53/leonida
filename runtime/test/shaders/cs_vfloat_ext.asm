; GPU test for four float VALU rows that docs/design.md lists as not GPU-verified (their translations were
; CPU-checked and, except v_mac_legacy_f32, the instructions measured on hardware, but the translated SPIR-V never
; ran on a GPU): v_cmp_class_f16 / v_cmp_class_f64, v_fmamk_f16 / v_fmaak_f16, v_cvt_pknorm_i16_f16 /
; v_cvt_pknorm_u16_f16, and v_mac_legacy_f32.
;
; Group 32x1x1, only lane 0 runs, so a compare's lane-0 bit is the whole answer. 16-bit VOP2 destinations are preset
; to 0xA5A5A5A5 to show the kept high half. The words follow the design.md rows:
;
;   out[0]  v_cmp_class_f16, one bit per case (class mask bits: 0 sNaN, 1 qNaN, 2 -inf, 4 -denormal, 9 +inf):
;           bit0 0x7C01 (sNaN) in {sNaN} = 1, bit1 0x8001 (-denormal) in {-denormal} = 1,
;           bit2 0x3C00 (+normal) in {+inf} = 0, bit3 0xFC00 (-inf) in {-inf} = 1                    -> 0x0000000B
;   out[1]  v_cmp_class_f64, the same way (5 -0, 7 +denormal): bit0 -0.0 in {-0} = 1,
;           bit1 a qNaN (hi 0x7FF80000) in {sNaN} = 0, bit2 the smallest +denormal in {+denormal} = 1,
;           bit3 +inf in {+inf} = 1                                                                   -> 0x0000000D
;   out[2]  v_fmamk_f16: 1.5 * K(2.0) + 0.5 = 3.5 (0x4300), high half kept                         -> 0xA5A54300
;   out[3]  v_fmaak_f16: (1 + 2^-10) * 3.0 + K(0) = 3.0029296875, one rounding to the even 0x4202   -> 0xA5A54202
;   out[4]  v_cvt_pknorm_i16_f16: low 0.5 * 32767 = 16383.5 rounds to even 16384 (0x4000),
;           high -inf gives -32767 (0x8001)                                                         -> 0x80014000
;   out[5]  v_cvt_pknorm_u16_f16: low NaN gives 0, high 2.0 clamps to 1.0, 65535                    -> 0xFFFF0000
;   out[6]  v_mac_legacy_f32: D = legacy(0 * +inf) + 2.0, and legacy 0 * anything is +0            -> 0x40000000
;   out[7]  v_mac_legacy_f32: D = 1.5 * 2.0 + 0.25 = 3.25                                           -> 0x40500000
;
; Run: vk_compute_test cs_vfloat_ext.spv 1 dump
;   -> out[0..7] = 0x0000000B 0x0000000D 0xA5A54300 0xA5A54202
;                  0x80014000 0xFFFF0000 0x40000000 0x40500000
; group: 32,1,1
; mattr: +wavefrontsize32,-wavefrontsize64
v_cmpx_eq_u32_e32 0, v0

; --- v_cmp_class_f16: four cases, one bit each ---
v_mov_b32_e32 v20, 0
v_mov_b32_e32 v1, 0x7C01
v_mov_b32_e32 v2, 0x1
v_cmp_class_f16_e64 s10, v1, v2
v_cndmask_b32_e64 v3, 0, 1, s10
v_or_b32_e32 v20, v20, v3
v_mov_b32_e32 v1, 0x8001
v_mov_b32_e32 v2, 0x10
v_cmp_class_f16_e64 s10, v1, v2
v_cndmask_b32_e64 v3, 0, 2, s10
v_or_b32_e32 v20, v20, v3
v_mov_b32_e32 v1, 0x3C00
v_mov_b32_e32 v2, 0x200
v_cmp_class_f16_e64 s10, v1, v2
v_cndmask_b32_e64 v3, 0, 4, s10
v_or_b32_e32 v20, v20, v3
v_mov_b32_e32 v1, 0xFC00
v_mov_b32_e32 v2, 0x4
v_cmp_class_f16_e64 s10, v1, v2
v_cndmask_b32_e64 v3, 0, 8, s10
v_or_b32_e32 v20, v20, v3
v_mov_b32_e32 v21, 0
buffer_store_dword v20, v21, s[0:3], 0 offen

; --- v_cmp_class_f64: four cases, one bit each ---
v_mov_b32_e32 v20, 0
v_mov_b32_e32 v4, 0
v_mov_b32_e32 v5, 0x80000000
v_mov_b32_e32 v2, 0x20
v_cmp_class_f64_e64 s10, v[4:5], v2
v_cndmask_b32_e64 v3, 0, 1, s10
v_or_b32_e32 v20, v20, v3
v_mov_b32_e32 v5, 0x7FF80000
v_mov_b32_e32 v2, 0x1
v_cmp_class_f64_e64 s10, v[4:5], v2
v_cndmask_b32_e64 v3, 0, 2, s10
v_or_b32_e32 v20, v20, v3
v_mov_b32_e32 v4, 1
v_mov_b32_e32 v5, 0
v_mov_b32_e32 v2, 0x80
v_cmp_class_f64_e64 s10, v[4:5], v2
v_cndmask_b32_e64 v3, 0, 4, s10
v_or_b32_e32 v20, v20, v3
v_mov_b32_e32 v4, 0
v_mov_b32_e32 v5, 0x7FF00000
v_mov_b32_e32 v2, 0x200
v_cmp_class_f64_e64 s10, v[4:5], v2
v_cndmask_b32_e64 v3, 0, 8, s10
v_or_b32_e32 v20, v20, v3
v_mov_b32_e32 v21, 4
buffer_store_dword v20, v21, s[0:3], 0 offen

; --- v_fmamk_f16 / v_fmaak_f16 ---
v_mov_b32_e32 v1, 0x3E00
v_mov_b32_e32 v2, 0x3800
v_mov_b32_e32 v10, 0xA5A5A5A5
v_fmamk_f16 v10, v1, 0x4000, v2
v_mov_b32_e32 v21, 8
buffer_store_dword v10, v21, s[0:3], 0 offen
v_mov_b32_e32 v1, 0x3C01
v_mov_b32_e32 v2, 0x4200
v_mov_b32_e32 v10, 0xA5A5A5A5
v_fmaak_f16 v10, v1, v2, 0x0
v_mov_b32_e32 v21, 12
buffer_store_dword v10, v21, s[0:3], 0 offen

; --- v_cvt_pknorm_i16_f16 / v_cvt_pknorm_u16_f16 ---
v_mov_b32_e32 v1, 0x3800
v_mov_b32_e32 v2, 0xFC00
v_cvt_pknorm_i16_f16 v10, v1, v2
v_mov_b32_e32 v21, 16
buffer_store_dword v10, v21, s[0:3], 0 offen
v_mov_b32_e32 v1, 0x7E00
v_mov_b32_e32 v2, 0x4000
v_cvt_pknorm_u16_f16 v10, v1, v2
v_mov_b32_e32 v21, 20
buffer_store_dword v10, v21, s[0:3], 0 offen

; --- v_mac_legacy_f32: D is the addend ---
v_mov_b32_e32 v1, 0
v_mov_b32_e32 v2, 0x7F800000
v_mov_b32_e32 v10, 0x40000000
v_mac_legacy_f32_e32 v10, v1, v2
v_mov_b32_e32 v21, 24
buffer_store_dword v10, v21, s[0:3], 0 offen
v_mov_b32_e32 v1, 0x3FC00000
v_mov_b32_e32 v2, 0x40000000
v_mov_b32_e32 v10, 0x3E800000
v_mac_legacy_f32_e32 v10, v1, v2
v_mov_b32_e32 v21, 28
buffer_store_dword v10, v21, s[0:3], 0 offen
s_endpgm
