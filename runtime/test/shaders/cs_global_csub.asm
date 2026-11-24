; mcpu: gfx1030
; mattr: +wavefrontsize32,-wavefrontsize64
; global_atomic_csub and buffer_atomic_csub (GFX10.3): tmp = M; M = (DATA > tmp) ? 0 : tmp - DATA, unsigned, a
; subtract clamped at 0 that never wraps; with glc the old M comes back (docs/design.md, measured on gfx1036 by #3193).
; gfx1013 has neither instruction, so this file is assembled for gfx1030 (the "; mcpu:" line above). llvm-mc takes
; global_atomic_csub with a destination only with glc; without glc it has none.
; Run with 40 groups (vk_compute_test cs_global_csub.spv 40 dump): the heap is 40 * 256 = 0x2800 bytes, global
; addresses are heap offsets (s[20:21] = P = 0x2000, v30 = 0), as in cs_global_mem_ext. Wave32, 64 threads a group.
; 1. Group 0 thread 0 seeds 12 dwords through the V# in s[0:3]; then every thread meets at s_barrier.
;      P+0x00 5   P+0x04 9   P+0x08 100   P+0x0c 0x80000000   P+0x10 0xffffffff   P+0x14 7   P+0x18 0x10
;      P+0x1c 2   P+0x20 20  P+0x24 0     P+0x28 0            P+0x2c 1000
; 2. Contention, the 32 lanes of group 0's first wave at once:
;      global_atomic_csub glc of 1 at P+0x20 (20): the old values are 20, 19, ..., 1 and then 0 twelve times; M ends
;      at 0. Each lane adds its old value to P+0x24 (sum 210) and adds 1 to P+0x28 when it got 0 (12).
;      global_atomic_csub without glc of lane + 1 at P+0x2c (1000): 1 + ... + 32 = 528 never reaches 0, M = 472.
;    A subtract that wraps would leave 0xfffffff4 at P+0x20; one that is not atomic would lose updates.
;    Then every thread meets at s_barrier again.
; 3. Group 0 thread 0, one case each (old value with glc, then M after):
;      A global glc 7 at P+0x00 (5): old 5, M 0 (DATA > M)    B global glc 9 at P+0x04 (9): old 9, M 0 (DATA == M)
;      C global glc 1 at P+0x08 (100): old 100, M 99          D global glc 0xffffffff at P+0x0c: old 0x80000000, M 0
;        (a subtract without the clamp leaves 0x80000000 - 0xffffffff = 0x80000001)
;      E global, no glc, 1 at P+0x10 (0xffffffff): M 0xfffffffe (a signed compare reads 1 > -1 and leaves 0)
;      F global glc 0 at P+0x14 (7): old 7, M 7               G buffer glc 3 at 0x2018 (0x10): old 0x10, M 0xd
;      H buffer, no glc, 5 at 0x201c (2): M 0, and its data register keeps 5
; vk_compute_test dumps out[0..7], so the small values are packed a byte or a half each (a wrong value spills into
; its neighbours, which still shows):
;   out[0] = A old | B old << 8 | F old << 16 | G old << 24              = 0x10070905
;   out[1] = A M | B M << 8 | F M << 16 | G M << 24                      = 0x0d070000
;   out[2] = C old | C M << 16                                           = 0x00630064
;   out[3] = D old                                                       = 0x80000000
;   out[4] = D M                                                         = 0x00000000
;   out[5] = E M                                                         = 0xfffffffe
;   out[6] = H M | H data register << 8 | lanes that got 0 << 16 | P+0x20 after << 24   = 0x000c0500
;   out[7] = sum of the old values | P+0x2c after << 16                  = 0x01d800d2
s_mov_b32 s40, exec_lo
v_or_b32_e32 v1, s4, v0
v_cmpx_eq_u32_e32 0, v1
v_mov_b32_e32 v2, 0x2000
v_mov_b32_e32 v1, 5
buffer_store_dword v1, v2, s[0:3], 0 offen offset:0
v_mov_b32_e32 v1, 9
buffer_store_dword v1, v2, s[0:3], 0 offen offset:4
v_mov_b32_e32 v1, 0x64
buffer_store_dword v1, v2, s[0:3], 0 offen offset:8
v_bfrev_b32_e32 v1, 1
buffer_store_dword v1, v2, s[0:3], 0 offen offset:12
v_mov_b32_e32 v1, -1
buffer_store_dword v1, v2, s[0:3], 0 offen offset:16
v_mov_b32_e32 v1, 7
buffer_store_dword v1, v2, s[0:3], 0 offen offset:20
v_mov_b32_e32 v1, 0x10
buffer_store_dword v1, v2, s[0:3], 0 offen offset:24
v_mov_b32_e32 v1, 2
buffer_store_dword v1, v2, s[0:3], 0 offen offset:28
v_mov_b32_e32 v1, 20
buffer_store_dword v1, v2, s[0:3], 0 offen offset:32
v_mov_b32_e32 v1, 0
buffer_store_dword v1, v2, s[0:3], 0 offen offset:36
buffer_store_dword v1, v2, s[0:3], 0 offen offset:40
v_mov_b32_e32 v1, 0x3e8
buffer_store_dword v1, v2, s[0:3], 0 offen offset:44
s_waitcnt vmcnt(0)
s_mov_b32 exec_lo, s40
s_barrier
s_mov_b32 s20, 0x2000
s_mov_b32 s21, 0
v_lshrrev_b32_e32 v1, 5, v0
v_or_b32_e32 v1, s4, v1
v_cmpx_eq_u32_e32 0, v1
v_mov_b32_e32 v30, 0
v_mov_b32_e32 v3, 1
global_atomic_csub v10, v30, v3, s[20:21] offset:32 glc
s_waitcnt vmcnt(0)
global_atomic_add v30, v10, s[20:21] offset:36
v_cmp_eq_u32_e32 vcc_lo, 0, v10
v_cndmask_b32_e64 v11, 0, 1, vcc_lo
global_atomic_add v30, v11, s[20:21] offset:40
v_add_nc_u32_e32 v12, 1, v0
global_atomic_csub v30, v12, s[20:21] offset:44
s_waitcnt vmcnt(0)
s_mov_b32 exec_lo, s40
s_barrier
v_or_b32_e32 v1, s4, v0
v_cmpx_eq_u32_e32 0, v1
v_mov_b32_e32 v30, 0
v_mov_b32_e32 v3, 7
global_atomic_csub v20, v30, v3, s[20:21] glc
v_mov_b32_e32 v3, 9
global_atomic_csub v21, v30, v3, s[20:21] offset:4 glc
v_mov_b32_e32 v3, 1
global_atomic_csub v22, v30, v3, s[20:21] offset:8 glc
v_mov_b32_e32 v3, -1
global_atomic_csub v23, v30, v3, s[20:21] offset:12 glc
v_mov_b32_e32 v3, 1
global_atomic_csub v30, v3, s[20:21] offset:16
v_mov_b32_e32 v3, 0
global_atomic_csub v24, v30, v3, s[20:21] offset:20 glc
v_mov_b32_e32 v25, 3
v_mov_b32_e32 v2, 0x2018
buffer_atomic_csub v25, v2, s[0:3], 0 offen glc
v_mov_b32_e32 v26, 5
v_mov_b32_e32 v2, 0x201c
buffer_atomic_csub v26, v2, s[0:3], 0 offen
s_waitcnt vmcnt(0)
global_load_dword v40, v30, s[20:21]
global_load_dword v41, v30, s[20:21] offset:4
global_load_dword v42, v30, s[20:21] offset:8
global_load_dword v43, v30, s[20:21] offset:12
global_load_dword v44, v30, s[20:21] offset:16
global_load_dword v45, v30, s[20:21] offset:20
global_load_dword v46, v30, s[20:21] offset:24
global_load_dword v47, v30, s[20:21] offset:28
global_load_dword v48, v30, s[20:21] offset:32
global_load_dword v49, v30, s[20:21] offset:36
global_load_dword v50, v30, s[20:21] offset:40
global_load_dword v51, v30, s[20:21] offset:44
s_waitcnt vmcnt(0)
v_lshl_or_b32 v60, v21, 8, v20
v_lshl_or_b32 v60, v24, 16, v60
v_lshl_or_b32 v60, v25, 24, v60
v_lshl_or_b32 v61, v41, 8, v40
v_lshl_or_b32 v61, v45, 16, v61
v_lshl_or_b32 v61, v46, 24, v61
v_lshl_or_b32 v62, v42, 16, v22
v_lshl_or_b32 v66, v26, 8, v47
v_lshl_or_b32 v66, v50, 16, v66
v_lshl_or_b32 v66, v48, 24, v66
v_lshl_or_b32 v67, v51, 16, v49
v_mov_b32_e32 v2, 0
buffer_store_dword v60, v2, s[0:3], 0 offen offset:0
buffer_store_dword v61, v2, s[0:3], 0 offen offset:4
buffer_store_dword v62, v2, s[0:3], 0 offen offset:8
buffer_store_dword v23, v2, s[0:3], 0 offen offset:12
buffer_store_dword v43, v2, s[0:3], 0 offen offset:16
buffer_store_dword v44, v2, s[0:3], 0 offen offset:20
buffer_store_dword v66, v2, s[0:3], 0 offen offset:24
buffer_store_dword v67, v2, s[0:3], 0 offen offset:28
s_endpgm
