; mcpu: gfx1030
; mattr: +wavefrontsize32,-wavefrontsize64
; group: 32,1,1
; global_atomic_csub and buffer_atomic_csub (GFX10.3), the cases cs_global_csub.asm leaves out: inactive lanes aimed
; at a trap word, and buffer_atomic_csub on an element past the V# range. The operation is
; tmp = M; M = (DATA > tmp) ? 0 : tmp - DATA, unsigned, with glc returning tmp (docs/design.md, global_atomic_csub
; row). A dropped atomic leaves memory alone; an active glc lane then gets 0 and an inactive lane keeps vdst/vdata
; (docs/design.md, "Atomic natural alignment" row, measured on gfx1036). An element is in range when
; O + 4 <= num_records under OOB_SELECT 3 (docs/design.md, "Buffer bounds checking" row).
; gfx1013 has neither instruction, so this file is assembled for gfx1030. One wave32 group (32 threads); the heap is
; the 256-byte output buffer and global addresses are heap offsets (s[20:21] = 0x40).
; 1. Lane 0 seeds through the V# in s[0:3]:
;      0x40 A_g 100   0x44 T_g 0x7777   0x48 S_g 0   0x4c S_b 0   0x50 A_b 50   0x54 T_b 0x5555
;      0x58 K_g 0   0x5c K_b 0   0x80 0   0x84 30   0x88 0x99   0x8c 0xab
; 2. All 32 lanes set up their operands; even lanes aim at A_g/A_b and odd lanes at the trap words T_g/T_b
;    (address + 4 * (lane & 1)). Then EXEC = 0x55555555 (the 16 even lanes):
;      global_atomic_csub glc, DATA 1, vdst preset to 0xc5b00000 | lane: A_g 100 -> 84, old values 100..85 in some
;        order, sum 1480 = 0x5c8, added to S_g by the same lanes.
;      buffer_atomic_csub glc, DATA 2 (vdata = 2): A_b 50 -> 18, old values 50, 48, ..., 20, sum 560 = 0x230, added
;        to S_b.
;    The odd lanes do nothing: T_g and T_b keep their seeds, and their vdst / vdata keep 0xc5b00000 | lane / 2.
;    With EXEC back to all 32 lanes, each lane whose register still holds its preset value ORs 1 << lane into K_g
;    (global vdst) or K_b (buffer vdata): only the odd lanes, 0xaaaaaaaa, for both forms. A lane whose old value were
;    not returned would set its even bit too (0xffffffff); an inactive lane given 0 would clear its odd bit. (An
;    atomic OR, not VCC copied into a VGPR: the translator keeps a lane mask as one bit per lane, so v_mov_b32 of it
;    reads this lane's bit; docs/design.md, "Whole-wave masks" row.)
; 3. Lane 0, through a V# in s[8:11] of base 0x80, stride 0, num_records 8, word3 0x31016fac (OOB_SELECT 3, as
;    cs_bounds.asm), so offsets 0 and 4 are in range and 8 and 12 are not:
;      B1 offset 4, glc, DATA 5: in range, M at 0x84 30 -> 25, vdata 30
;      B2 offset 8, glc, DATA 1: out of range, 0x88 keeps 0x99, vdata 0
;      B3 offset 12, no glc, DATA 3: out of range, 0x8c keeps 0xab, vdata keeps 3
; out[0..7] (two values a word where they fit, so a wrong one still shows):
;   out[0] = K_g, the lanes whose vdst still holds its preset          = 0xaaaaaaaa
;   out[1] = A_g after | T_g after << 16                               = 0x77770054
;   out[2] = S_g | S_b << 16                                           = 0x023005c8
;   out[3] = K_b, the lanes whose vdata still holds its preset         = 0xaaaaaaaa
;   out[4] = A_b after | T_b after << 16                               = 0x55550012
;   out[5] = B1 vdata | 0x84 after << 16                               = 0x0019001e
;   out[6] = B2 vdata | 0x88 after << 16                               = 0x00990000
;   out[7] = B3 vdata | 0x8c after << 16                               = 0x00ab0003
; Run: vk_compute_test cs_w32_csub_ext.spv 1 dump -> out[0..7] = 2863311530 2004287572 36701640 2863311530
; 1431633938 1638430 10027008 11206659
s_mov_b32 s40, exec_lo
v_cmpx_eq_u32_e32 0, v0
v_mov_b32_e32 v2, 0
v_mov_b32_e32 v1, 0x64
buffer_store_dword v1, v2, s[0:3], 0 offen offset:64
v_mov_b32_e32 v1, 0x7777
buffer_store_dword v1, v2, s[0:3], 0 offen offset:68
v_mov_b32_e32 v1, 0
buffer_store_dword v1, v2, s[0:3], 0 offen offset:72
buffer_store_dword v1, v2, s[0:3], 0 offen offset:76
buffer_store_dword v1, v2, s[0:3], 0 offen offset:128
v_mov_b32_e32 v1, 0x32
buffer_store_dword v1, v2, s[0:3], 0 offen offset:80
v_mov_b32_e32 v1, 0x5555
buffer_store_dword v1, v2, s[0:3], 0 offen offset:84
v_mov_b32_e32 v1, 0
buffer_store_dword v1, v2, s[0:3], 0 offen offset:88
buffer_store_dword v1, v2, s[0:3], 0 offen offset:92
v_mov_b32_e32 v1, 30
buffer_store_dword v1, v2, s[0:3], 0 offen offset:132
v_mov_b32_e32 v1, 0x99
buffer_store_dword v1, v2, s[0:3], 0 offen offset:136
v_mov_b32_e32 v1, 0xab
buffer_store_dword v1, v2, s[0:3], 0 offen offset:140
s_waitcnt vmcnt(0)
s_mov_b32 exec_lo, s40
s_barrier
s_mov_b32 s20, 0x40
s_mov_b32 s21, 0
v_and_b32_e32 v1, 1, v0
v_lshlrev_b32_e32 v2, 2, v1
v_add_nc_u32_e32 v4, 0x50, v2
v_mov_b32_e32 v3, 1
v_or_b32_e32 v10, 0xc5b00000, v0
v_mov_b32_e32 v11, 2
s_mov_b32 exec_lo, 0x55555555
global_atomic_csub v10, v2, v3, s[20:21] glc
buffer_atomic_csub v11, v4, s[0:3], 0 offen glc
s_waitcnt vmcnt(0)
global_atomic_add v2, v10, s[20:21] offset:8
global_atomic_add v2, v11, s[20:21] offset:12
s_waitcnt vmcnt(0)
s_mov_b32 exec_lo, s40
v_mov_b32_e32 v30, 0
v_or_b32_e32 v12, 0xc5b00000, v0
v_cmp_eq_u32_e32 vcc_lo, v12, v10
v_cndmask_b32_e64 v13, 0, 1, vcc_lo
v_lshlrev_b32_e32 v13, v0, v13
global_atomic_or v30, v13, s[20:21] offset:24
v_cmp_eq_u32_e32 vcc_lo, 2, v11
v_cndmask_b32_e64 v14, 0, 1, vcc_lo
v_lshlrev_b32_e32 v14, v0, v14
global_atomic_or v30, v14, s[20:21] offset:28
s_waitcnt vmcnt(0)
s_barrier
v_cmpx_eq_u32_e32 0, v0
s_mov_b32 s8, 0x80
s_mov_b32 s9, 0
s_mov_b32 s10, 8
s_mov_b32 s11, 0x31016fac
v_mov_b32_e32 v5, 4
v_mov_b32_e32 v20, 5
buffer_atomic_csub v20, v5, s[8:11], 0 offen glc
v_mov_b32_e32 v5, 8
v_mov_b32_e32 v21, 1
buffer_atomic_csub v21, v5, s[8:11], 0 offen glc
v_mov_b32_e32 v5, 12
v_mov_b32_e32 v22, 3
buffer_atomic_csub v22, v5, s[8:11], 0 offen
s_waitcnt vmcnt(0)
global_load_dword v40, v30, s[20:21]
global_load_dword v41, v30, s[20:21] offset:4
global_load_dword v42, v30, s[20:21] offset:8
global_load_dword v43, v30, s[20:21] offset:12
global_load_dword v44, v30, s[20:21] offset:16
global_load_dword v45, v30, s[20:21] offset:20
global_load_dword v46, v30, s[20:21] offset:68
global_load_dword v47, v30, s[20:21] offset:72
global_load_dword v48, v30, s[20:21] offset:76
global_load_dword v60, v30, s[20:21] offset:24
global_load_dword v63, v30, s[20:21] offset:28
s_waitcnt vmcnt(0)
v_lshl_or_b32 v61, v41, 16, v40
v_lshl_or_b32 v62, v43, 16, v42
v_lshl_or_b32 v64, v45, 16, v44
v_lshl_or_b32 v65, v46, 16, v20
v_lshl_or_b32 v66, v47, 16, v21
v_lshl_or_b32 v67, v48, 16, v22
v_mov_b32_e32 v2, 0
buffer_store_dword v60, v2, s[0:3], 0 offen offset:0
buffer_store_dword v61, v2, s[0:3], 0 offen offset:4
buffer_store_dword v62, v2, s[0:3], 0 offen offset:8
buffer_store_dword v63, v2, s[0:3], 0 offen offset:12
buffer_store_dword v64, v2, s[0:3], 0 offen offset:16
buffer_store_dword v65, v2, s[0:3], 0 offen offset:20
buffer_store_dword v66, v2, s[0:3], 0 offen offset:24
buffer_store_dword v67, v2, s[0:3], 0 offen offset:28
s_endpgm
