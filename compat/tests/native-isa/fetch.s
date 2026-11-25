// Vertex fetch subroutine (Xbox "wrapped" VS calls it via s_swappc_b64 s[4:5]).
// Must preserve s8 (resource table used by the VS body after return) and s[4:5].
// s[0:1] table (40-bit VA, like the PSO-linked variant), s2 base vertex, v0 vertex id.
// Body expects POSITION v[4:6], COLOR v[8:11], TEXCOORD v[12:13]; layout stride 24.
  s_mov_b64 s[12:13], s[0:1]
  s_bfe_u32 s13, s13, 0x80000
  s_load_dwordx4 s[12:15], s[12:13], 0x0
  v_add_nc_u32 v0, s2, v0
  s_waitcnt lgkmcnt(0)
  buffer_load_dwordx3 v[4:6], v0, s[12:15], 0 idxen
  tbuffer_load_format_xyzw v[8:11], v0, s[12:15], 0 format:[BUF_FMT_8_8_8_8_UNORM] idxen offset:12
  buffer_load_dwordx2 v[12:13], v0, s[12:15], 0 idxen offset:16
  s_setpc_b64 s[4:5]
