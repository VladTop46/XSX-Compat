#!/usr/bin/env python3 -I
"""Показывает «дыры» (нулевые блоки) в недокачанных файлах. Только чтение. Использование: rpf-holes.py <файл> [блок_МиБ=1]
Диапазоны выводятся в МиБ и в виде смещений (для сверки с загрузчиком)."""
import os, sys
p = sys.argv[1]; B = (int(sys.argv[2]) if len(sys.argv) > 2 else 1) << 20
sz = os.path.getsize(p); n = (sz + B - 1) // B; rng = []; cur = None; zero = 0
with open(p, 'rb') as f:
    for i in range(n):
        blk = f.read(B); z = blk.count(0) == len(blk); zero += z
        if z: cur = [i, i] if cur is None else [cur[0], i]
        elif cur: rng.append(tuple(cur)); cur = None
    if cur: rng.append(tuple(cur))
print(f'{os.path.basename(p)}: {sz/2**20:.0f} МиБ, нулевых блоков {zero}/{n} ({100*zero/max(n,1):.1f}%), диапазонов {len(rng)}')
for a, b in rng[:200]: print(f'  блоки {a}..{b}  = {a*B>>20}..{((b+1)*B)>>20} МиБ  (смещение {a*B:#x}..{(b+1)*B:#x})')
