# Native RDNA2 execution of Xbox game ISA on host GPU (25.11.2026)

Host: Navi21 (0x73bf, gfx1030). Native Linux, libdrm_amdgpu, raw PM4. No Wine, no game process.
All memory = ordinary process memory mapped into the GPU VM at VA == CPU address (userptr BO +
amdgpu_bo_va_op_raw). VM range [0x10000, 0x800000000000).

## Exp 1 — raw-dispatch.c: PASS 1024/1024
Hand-written gfx1030 compute ISA (store.s), COMPUTE ring, SET_SH_REG + DISPATCH_DIRECT.

## Exp 2 — raw-draw.c: PASS 4096/4096, max_error 0 (3 variants)
Game shaders from shaders.rpf (extract-isa.py): VS_Blit_6e5520d1_Wrapped (903b…),
PS_BlitColour_6e5520d1_Wrapped (b8e1…) = exactly the live "draw A" pair. GFX ring, legacy VS
wave64, hardware RECTLIST (0x11, same as the game), 64x64 linear RGBA8.
- RSRC1/RSRC2 VS/PS and CB_COLOR_CONTROL taken verbatim from PSO state block pso2_q1.bin:
  +0x38 RSRC1_VS 082c0343, +0x3c RSRC2_VS 00000012, +0x4c RSRC1_PS 022cc341,
  +0x50 RSRC2_PS 00000012, +0x7c CB_COLOR_CONTROL 00cc0010 (offsets inferred by field fit,
  proven only to the extent this draw works).
- Variants: prefix replaced by s_nop / ORIGINAL 24-byte hash prefix / game's V# word3 0x0002403c
  (no RESOURCE_LEVEL bit) for the constant buffer — all PASS.
- Wrapped VS calls a fetch subroutine via s_swappc_b64 s[4:5]; fetch.s is ours (written after the
  PSO-linked variant q9: s[0:1] 40-bit table, s2 base vertex, V# stride 24).
- ABI observed: VS user SGPRs s[0:1] table(40-bit), s2 base vertex, s[4:5] fetch addr, 9 user SGPRs;
  PS user SGPR s8 = table (32-bit, hi=0), s9 = PRIM_MASK.
NOT proven: other state block fields, textures/T#/samplers, tiled surfaces, DCC, depth, blending,
integration into the Wine process, coexistence with VKD3D presentation.

## Exp 3 — raw-tex.c: PASS 4096/4096, max_error 0; negative control FAILs as required
Game pair from PSO pso1 (live "draw B"): sga_dr_vs_pos_color_tex_Wrapped (04cf…) +
default_ps_tex_Wrapped (b2e8…), original prefixes. VS: world (V# Buf0 @table+0) × viewproj
(V# Buf1 @table+0x40, matrix at +0x160), pixel-space RECTLIST like the game. PS: image_sample
with T# from table+0x20 (8 dwords, built by us: GFX10 8_8_8_8_UNORM, linear SW_MODE 0) and the
shader's own inline S# (0x400,0xc00000,0x18f00000,0). RSRC1/2 from pso1_q1 (VS 24 VGPR, PS 16).
State block +0x58 = SPI_PS_INPUT_ENA, +0x5c = SPI_PS_IN_CONTROL.NUM_INTERP (pso1=2, pso2=1).
VS and PS both take the resource table in s8 (32-bit, hi forced 0) -> tables must be < 4 GiB VA.
Fetch subroutine must preserve s8 (fixed: now uses s[12:15]); exp2 still PASS after the change.
--negative (+8 px world translation): ok=0/4096, untouched=512 (= 8 columns), out(37,21) equals
texel(29,21) -> the oracle detects geometry errors; shader output is real.
NOT proven: Xbox tiled/swizzled texture layouts (pipe/bank config of Series X vs Navi21 may differ
for _X swizzle modes), DCC, mips, real game T# words (in our runtime game tables hold VKD3D/opaque
tags, not hardware T#; native path must write real T# in CreateShaderResourceView).
