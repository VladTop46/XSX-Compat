#!/usr/bin/env python3 -I
"""Генерирует логирующие заглушки отсутствующих системных Xbox DLL по таблице импортов exe (статически, без запуска).
Каждая функция принимает до 6 аргументов, печатает [STUB dll!name] args и возвращает значение по политике DLL.
Использование: gen-stub-dlls.py <exe> <out_dir>"""
import subprocess, sys, os, re
exe, out = sys.argv[1], sys.argv[2]
# политика: возвращаемое значение. E_NOTIMPL для HRESULT-API, 0 для остальных (семантика НЕ известна).
POLICY = {'xg_xs.dll': '0x80004001', 'gameinput.dll': '0x80004001'}
ORDINAL_NAMES = {('gameinput.dll', 1): 'GameInputCreate'}   # публичный API GameInput.h, экспорт по ординалу 1
TARGETS = {'chad.dll', 'xapu.dll'}  # AcpHal: compat/runtime/audio; xg_xs/GameInput также имеют рукописные runtime.
txt = subprocess.run(['llvm-readobj', '--coff-imports', exe], capture_output=True, text=True).stdout
mods = {}; cur = None
for line in txt.splitlines():
    m = re.match(r'\s+Name: (\S+)', line)
    if m: cur = m.group(1); continue
    m = re.match(r'\s+Symbol: (.*)', line)
    if m and cur and cur.lower() in TARGETS:
        s = m.group(1).strip(); mods.setdefault(cur, []).append(s)
os.makedirs(out, exist_ok=True)
for dll, syms in mods.items():
    base = dll[:-4]; names = []
    for s in syms:
        mo = re.match(r'^\s*\((\d+)\)$', s) or re.match(r'^$', s)
        if s.strip().startswith('(') or not s.strip() or re.match(r'^\(\d+\)$', s.strip()):
            o = int(re.search(r'\d+', s).group()); names.append((ORDINAL_NAMES.get((dll.lower(), o), f'Ordinal{o}'), o))
        else:
            n = re.sub(r'\s*\(\d+\)$', '', s).strip(); names.append((n, None))
    ret = POLICY.get(dll.lower(), '0')
    cpp = ['#include <windows.h>', '#include <cstdio>', '#include <cstdint>',
           f'#define SLOG(n,a,b,c,d,e,f) do {{ fprintf(stderr, "[STUB {dll}!%s] args=(%p,%p,%p,%p,%p,%p) -> {ret}\\n", n, (void*)a,(void*)b,(void*)c,(void*)d,(void*)e,(void*)f); fflush(stderr); }} while (0)', 'extern "C" {']
    d = [f'LIBRARY {dll}', 'EXPORTS']
    for n, o in names:
        cpp.append(f'__declspec(dllexport) uintptr_t {n}(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e, uintptr_t f) {{ SLOG("{n}", a, b, c, d, e, f); return (uintptr_t){ret}; }}')
        d.append(f'    {n}' + (f' @{o}' if o else ''))
    cpp.append('}')
    open(f'{out}/{base}.cpp', 'w').write('\n'.join(cpp) + '\n'); open(f'{out}/{base}.def', 'w').write('\n'.join(d) + '\n')
    print(dll, len(names), 'exports', [n for n, _ in names][:12])
