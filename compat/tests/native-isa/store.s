// s[0:1] = output base (user SGPR 0..1), s2 = workgroup id x, v0 = local id x
// out[gid] = 0xC0DE0000 | gid
  s_lshl_b32 s3, s2, 6            // wg*64
  v_add_nc_u32 v1, s3, v0         // gid
  v_or_b32 v2, 0xC0DE0000, v1
  v_lshlrev_b32 v3, 2, v1         // byte offset
  global_store_dword v3, v2, s[0:1]
  s_waitcnt vmcnt(0)
  s_endpgm
