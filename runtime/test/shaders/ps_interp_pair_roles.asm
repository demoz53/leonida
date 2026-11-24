; Pair weights are the values the named registers hold (SHADER-659). Drawn with the test triangle
; (vs_tri / vs_ids: (-1,-1), (3,-1), (-1,3) with attr0 = (0,0), (2,0), (0,2) at the vertices), whose
; barycentrics over the 64-by-64 frame are i = (x + 0.5) / 128, j = (y + 0.5) / 128: attr0.x interpolates
; to 2*i = (x + 0.5) / 64 and attr0.y to 2*j = (y + 0.5) / 64. Each pair here reads one of the two
; components, so its value is 2 * (the weight its p1 names) on attr0.x and 2 * (the weight its p2 names)
; on attr0.y. Expected mrt0 = (2j, 2i, 2i, 2j) = ((y + 0.5) / 64, (x + 0.5) / 64, (x + 0.5) / 64,
; (y + 0.5) / 64):
;   v2: p1(v1)/p2(v0) on attr0.x  - the pair named the other way round
;   v3: p1(v0)/p2(v1) on attr0.x  - the standard pair, the control
;   v4: p1(v0)/p2(v0) on attr0.y  - both ops read I
;   v5: p1(v1)/p2(v1) on attr0.y  - both ops read J
; A model that used the hardware weights whatever register the op named would give (2i, 2i, 2j, 2j).
v_interp_p1_f32_e32 v2, v1, attr0.x
v_interp_p2_f32_e32 v2, v0, attr0.x
v_interp_p1_f32_e32 v3, v0, attr0.x
v_interp_p2_f32_e32 v3, v1, attr0.x
v_interp_p1_f32_e32 v4, v0, attr0.y
v_interp_p2_f32_e32 v4, v0, attr0.y
v_interp_p1_f32_e32 v5, v1, attr0.y
v_interp_p2_f32_e32 v5, v1, attr0.y
exp mrt0 v2, v3, v4, v5 done vm
s_endpgm
