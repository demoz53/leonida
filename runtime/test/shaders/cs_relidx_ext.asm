; GPU test for two relative-register rows that docs/design.md lists as not GPU-verified (CPU-checked, and
; v_swaprel_b32 measured on hardware, but the translated SPIR-V never ran on a GPU): v_swaprel_b32 and the relative
; SGPR reads s_movrels_b32 / s_movrels_b64.
;
; Group 32x1x1, only lane 0 runs. The words follow the design.md rows (RDNA2 ISA VOP1 opcode 104 and SOP1):
;
;   out[0]  v_swaprel_b32 v10, v11 with M0 0xFC02FC03: M0[25:16] = 2 and M0[9:0] = 3 (the other bits are ignored),
;           so v[10+2] and v[11+3] swap; v12 was 0x1212, v14 0x1414: v12 after                       -> 0x00001414
;   out[1]  v14 after the same swap                                                                 -> 0x00001212
;   out[2]  v_swaprel_b32 v11, v12 with M0 (1 << 16) | 0: both name v12, which keeps its value 0xABCD -> 0x0000ABCD
;   out[3]  s_movrels_b32 s30, s20 with M0 3: s[20+3] = 103                                          -> 0x00000067
;   out[4]  s_movrels_b64 s[30:31], s[20:21] with M0 2: the pair s22, s23; low = 102                -> 0x00000066
;   out[5]  its high dword, 103                                                                     -> 0x00000067
;   out[6]  s_movrels_b32 s20, s20 with M0 1 (destination overlaps the source): s21 = 101           -> 0x00000065
;   out[7]  SCC is not written: SCC set by s_cmp_eq_u32 0, 0 before an s_movrels_b32, then
;           s_cselect_b32 7, 9                                                                      -> 0x00000007
;
; Run: vk_compute_test cs_relidx_ext.spv 1 dump
;   -> out[0..7] = 0x00001414 0x00001212 0x0000ABCD 0x00000067
;                  0x00000066 0x00000067 0x00000065 0x00000007
; group: 32,1,1
; mattr: +wavefrontsize32,-wavefrontsize64
v_cmpx_eq_u32_e32 0, v0

; --- v_swaprel_b32 through both M0 fields, upper M0 bits set ---
v_mov_b32_e32 v12, 0x1212
v_mov_b32_e32 v14, 0x1414
s_mov_b32 m0, 0xFC02FC03
v_swaprel_b32 v10, v11
v_mov_b32_e32 v21, 0
buffer_store_dword v12, v21, s[0:3], 0 offen
v_mov_b32_e32 v21, 4
buffer_store_dword v14, v21, s[0:3], 0 offen

; --- v_swaprel_b32 with both operands naming v12 ---
v_mov_b32_e32 v12, 0xABCD
s_mov_b32 m0, 0x10000
v_swaprel_b32 v11, v12
v_mov_b32_e32 v21, 8
buffer_store_dword v12, v21, s[0:3], 0 offen

; --- s_movrels_b32 / s_movrels_b64 ---
s_mov_b32 s20, 100
s_mov_b32 s21, 101
s_mov_b32 s22, 102
s_mov_b32 s23, 103
s_mov_b32 s24, 104
s_mov_b32 s25, 105
s_mov_b32 m0, 3
s_movrels_b32 s30, s20
v_mov_b32_e32 v10, s30
v_mov_b32_e32 v21, 12
buffer_store_dword v10, v21, s[0:3], 0 offen
s_mov_b32 m0, 2
s_movrels_b64 s[30:31], s[20:21]
v_mov_b32_e32 v10, s30
v_mov_b32_e32 v21, 16
buffer_store_dword v10, v21, s[0:3], 0 offen
v_mov_b32_e32 v10, s31
v_mov_b32_e32 v21, 20
buffer_store_dword v10, v21, s[0:3], 0 offen
s_mov_b32 m0, 1
s_movrels_b32 s20, s20
v_mov_b32_e32 v10, s20
v_mov_b32_e32 v21, 24
buffer_store_dword v10, v21, s[0:3], 0 offen

; --- SCC survives s_movrels_b32 ---
s_mov_b32 m0, 2
s_cmp_eq_u32 0, 0
s_movrels_b32 s32, s21
s_cselect_b32 s33, 7, 9
v_mov_b32_e32 v10, s33
v_mov_b32_e32 v21, 28
buffer_store_dword v10, v21, s[0:3], 0 offen
s_endpgm
