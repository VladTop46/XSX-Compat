#!/usr/bin/env python3 -I
"""MSVC RTTI: имя класса по RVA vtable (читает [vtable-8] -> CompleteObjectLocator -> TypeDescriptor). Статически. Использование: rtti.py <exe> <vtable_rva_hex> ..."""
import mmap, struct, sys
f = open(sys.argv[1], 'rb'); m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
pe = struct.unpack_from('<I', m, 0x3c)[0]; n = struct.unpack_from('<H', m, pe + 6)[0]; osz = struct.unpack_from('<H', m, pe + 20)[0]
base = struct.unpack_from('<Q', m, pe + 24 + 24)[0]; so = pe + 24 + osz; secs = [struct.unpack_from('<8sIIII', m, so + 40 * i) for i in range(n)]
def r2o(r):
    for nm, vs, va, rs, rp in secs:
        if va <= r < va + rs: return rp + r - va
for a in sys.argv[2:]:
    v = int(a, 16); o = r2o(v - 8)
    if o is None: print(a, 'не в файле (bss?)'); continue
    col = struct.unpack_from('<Q', m, o)[0] - base
    co = r2o(col)
    if co is None: print(a, 'COL вне образа'); continue
    sig, off, cd, tdrva, chdrva, selfrva = struct.unpack_from('<IIIiII', m, co)
    to = r2o(tdrva); nm = bytes(m[to + 16:to + 16 + 120]).split(b'\0')[0].decode(errors='replace') if to else '?'
    print(f'{a}: {nm}')
