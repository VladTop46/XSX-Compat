#!/usr/bin/env python3 -I
"""Статически: найти функцию по RVA через .pdata (RUNTIME_FUNCTION). Использование: pe-func.py <exe> <rva_hex> [...]"""
import mmap, struct, sys, bisect
f=open(sys.argv[1],'rb'); m=mmap.mmap(f.fileno(),0,access=mmap.ACCESS_READ)
pe=struct.unpack_from('<I',m,0x3c)[0]; nsec=struct.unpack_from('<H',m,pe+6)[0]; osz=struct.unpack_from('<H',m,pe+20)[0]; opt=pe+24
exc_rva,exc_sz=struct.unpack_from('<II',m,opt+112+3*8)
so=opt+osz; secs=[struct.unpack_from('<8sIIII',m,so+40*i) for i in range(nsec)]
def r2o(r):
    for n,vs,va,rs,rp in secs:
        if va<=r<va+max(vs,rs): return rp+r-va
o=r2o(exc_rva); n=exc_sz//12
ents=[struct.unpack_from('<III',m,o+12*i) for i in range(n)]; begins=[e[0] for e in ents]
for a in sys.argv[2:]:
    rva=int(a,16); i=bisect.bisect_right(begins,rva)-1
    b,e,u=ents[i]
    if b<=rva<e: print(f'{a}: function RVA {b:#x}-{e:#x} (VA {0x140000000+b:#x}), size {e-b:#x}')
    else: print(f'{a}: no function')
