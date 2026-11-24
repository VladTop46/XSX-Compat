#!/usr/bin/env python3 -I
"""Для RVA (адрес возврата/функции) показывает границы функции и ASCII/UTF-16 строки, на которые она ссылается через lea/mov [rip+d]. Статически.
Использование: func-strings.py <exe> <rva_hex> ..."""
import mmap, struct, sys, re, bisect
f = open(sys.argv[1], 'rb'); m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
pe = struct.unpack_from('<I', m, 0x3c)[0]; n = struct.unpack_from('<H', m, pe + 6)[0]; osz = struct.unpack_from('<H', m, pe + 20)[0]; opt = pe + 24
so = opt + osz; secs = [struct.unpack_from('<8sIIII', m, so + 40 * i) for i in range(n)]
def r2o(r):
    for nm, vs, va, rs, rp in secs:
        if va <= r < va + rs: return rp + r - va
er, es = struct.unpack_from('<II', m, opt + 112 + 24); eo = r2o(er)
ents = [struct.unpack_from('<III', m, eo + 12 * i) for i in range(es // 12)]; begins = [e[0] for e in ents]
def cstr(rva):
    o = r2o(rva)
    if o is None: return None
    b = bytes(m[o:o + 120]); a = re.match(rb'[ -~]{5,}', b)
    if a: return a.group(0).decode()
    w = re.match(rb'(?:[ -~]\x00){5,}', b)
    if w: return 'L"' + w.group(0).decode('utf-16le') + '"'
for a in sys.argv[2:]:
    rva = int(a, 16); i = bisect.bisect_right(begins, rva) - 1; b, e, u = ents[i]
    if not (b <= rva < e): print(f'{a}: нет функции'); continue
    code = bytes(m[r2o(b):r2o(b) + (e - b)]); strs = []
    for mm in re.finditer(rb'[\x48\x4c]\x8d[\x05\x0d\x15\x1d\x25\x2d\x35\x3d]', code):
        d = struct.unpack_from('<i', code, mm.start() + 3)[0]; t = b + mm.start() + 7 + d
        s = cstr(t)
        if s and s not in strs: strs.append(s)
    print(f'{a}: функция {b:#x}-{e:#x} размер {e-b:#x}'); [print('     ', s[:110]) for s in strs[:8]]
