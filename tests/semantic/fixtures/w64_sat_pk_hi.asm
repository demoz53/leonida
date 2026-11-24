; vs_w64_sat_pk_hi: hand-written wave64 vertex shader (llvm-mc -triple=amdgcn -mcpu=gfx1030 -show-encoding).
; v_sat_pk_u8_i16 writes the low half of v6, whose high half holds the lane id (0..63 in the wave), and the high half
; is read back afterwards (v7) and exported. SRC0's two signed halves come from the vertex index k (the global lane,
; the same however the wave is cut): lo = 8k - 128 (-128..376), hi = 300 - 8k (300..-204), so both halves saturate
; below 0 and above 255 across the lanes.
; param0 of lane k: v6 = lane_id << 16 | sat8(hi) << 8 | sat8(lo), v7 = lane_id, v1 = SRC0, 0. With all 64 lanes in one
; wave lane_id = k; cut into two waves of 32 it is k % 32, so the export differs for k >= 32 and the program is tier C
; (tools/wave64_halves.py must keep v6's old value as a source of the write).
	s_mov_b64 s[10:11], exec                ; encoding: [0x7e,0x04,0x8a,0xbe]
	v_mbcnt_lo_u32_b32 v10, -1, 0           ; encoding: [0x0a,0x00,0x65,0xd7,0xc1,0x00,0x01,0x02]
	v_mbcnt_hi_u32_b32 v10, -1, v10         ; encoding: [0x0a,0x00,0x66,0xd7,0xc1,0x14,0x02,0x02]
	v_lshlrev_b32_e32 v2, 3, v0             ; encoding: [0x83,0x00,0x04,0x34]
	v_subrev_nc_u32_e32 v3, 0x80, v2        ; encoding: [0xff,0x04,0x06,0x4e,0x80,0x00,0x00,0x00]
	v_sub_nc_u32_e32 v4, 0x12c, v2          ; encoding: [0xff,0x04,0x08,0x4c,0x2c,0x01,0x00,0x00]
	v_and_b32_e32 v3, 0xffff, v3            ; encoding: [0xff,0x06,0x06,0x36,0xff,0xff,0x00,0x00]
	v_lshl_or_b32 v1, v4, 16, v3            ; encoding: [0x01,0x00,0x6f,0xd7,0x04,0x21,0x0d,0x04]
	v_lshl_or_b32 v6, v10, 16, 0xa5a5       ; encoding: [0x06,0x00,0x6f,0xd7,0x0a,0x21,0xfd,0x03,0xa5,0xa5,0x00,0x00]
	v_sat_pk_u8_i16_e32 v6, v1              ; encoding: [0x01,0xc5,0x0c,0x7e]
	v_lshrrev_b32_e32 v7, 16, v6            ; encoding: [0x90,0x0c,0x0e,0x2c]
	v_mov_b32_e32 v5, 0                     ; encoding: [0x80,0x02,0x0a,0x7e]
	exp pos0, v5, v5, v5, v5 done           ; encoding: [0xcf,0x08,0x00,0xf8,0x05,0x05,0x05,0x05]
	exp param0, v6, v7, v1, v5              ; encoding: [0x0f,0x02,0x00,0xf8,0x06,0x07,0x01,0x05]
	s_endpgm                                ; encoding: [0x00,0x00,0x81,0xbf]
