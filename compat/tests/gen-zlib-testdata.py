#!/usr/bin/env python3 -I
"""Генерирует эталонные zlib-наборы zt_*.z/.raw для xcrdapi-test.exe (положить в sandbox/game). Использование: gen-zlib-testdata.py <dir>"""
import zlib, random, sys
random.seed(7); d = sys.argv[1].rstrip('/') + '/'
cases = {'fixed_small': (b'hello hello hello hello', 1), 'text_dynamic': ((b'The quick brown fox jumps over the lazy dog. ' * 400) + bytes(range(256)) * 20, 6),
         'random_stored': (bytes(random.getrandbits(8) for _ in range(70000)), 0), 'mixed_l9': (b''.join(bytes([i % 251]) * random.randint(1, 40) for i in range(5000)), 9),
         'empty': (b'', 6), 'big_l1': (bytes(range(256)) * 4096, 1)}
for n, (raw, lv) in cases.items(): open(d + f'zt_{n}.raw', 'wb').write(raw); open(d + f'zt_{n}.z', 'wb').write(zlib.compress(raw, lv))
open(d + 'zt_list.txt', 'w').write('\n'.join(cases))
