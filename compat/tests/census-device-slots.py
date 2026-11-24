#!/usr/bin/env python3 -I
"""ЭВРИСТИЧЕСКАЯ статическая перепись вызовов через vtable device в game_scarlett_master.exe (без запуска).
Берём глобалы, куда игра раскладывает device после D3D12XboxCreateDevice (RVA-диапазон из callsite 0x1424785b7),
находим `mov reg,[rip+glob]` и первый в следующих 48 байтах `call [rax+disp]`. Это не типизированный анализ:
ложные срабатывания возможны (другой объект в rax), счётчики — верхняя оценка."""
import mmap, re, struct, collections, sys
f = open(sys.argv[1], 'rb'); m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
base, tva, trp, tvs = 0x140000000, 0x1000, 0x400, 0x5F3F966
t = m[trp:trp+tvs]
G0, G1 = 0x1475deff8, 0x1475df048   # 12 глобалов-алиасов device (по callsite)
cnt = collections.Counter(); first = {}
for mm in re.finditer(rb'[\x48\x4c]\x8b[\x05\x0d\x15\x1d\x25\x2d\x35\x3d]', t, re.S):
    o = mm.start()
    d = struct.unpack_from('<i', t, o+3)[0]; va = base + tva + o + 7 + d
    if not (G0 <= va <= G1): continue
    w = t[o+7:o+7+48]
    c = re.search(rb'\xff\x50(.)|\xff\x90(....)', w, re.S)
    if not c: continue
    off = c.group(1)[0] if c.group(1) else struct.unpack('<i', c.group(2))[0]
    cnt[off//8] += 1; first.setdefault(off//8, hex(base + tva + o))
print('device-global loads with following vtable call:', sum(cnt.values()))
for s, n in sorted(cnt.items()): print(f'slot {s:3d} (+{s*8:#x})  x{n:3d}  first@{first[s]}')
