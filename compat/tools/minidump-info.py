#!/usr/bin/env python3 -I
"""Минимальный разбор minidump: исключение (код/адрес/параметры), регистры (CONTEXT x64), модули (база+имя). Использование: minidump-info.py <dmp>"""
import struct, sys
d = open(sys.argv[1], 'rb').read()
sig, ver, n, dirrva = struct.unpack_from('<4sIII', d, 0); assert sig == b'MDMP'
streams = {}
for i in range(n):
    t, sz, rva = struct.unpack_from('<III', d, dirrva + 12 * i); streams[t] = (sz, rva)
print('streams:', sorted(streams))
def ustr(rva): l = struct.unpack_from('<I', d, rva)[0]; return d[rva+4:rva+4+l].decode('utf-16le')
mods = []
if 4 in streams:
    cnt = struct.unpack_from('<I', d, streams[4][1])[0]
    for i in range(cnt):
        base, size, _, _, nrva = struct.unpack_from('<QIIII', d, streams[4][1] + 4 + 108 * i); mods.append((base, size, ustr(nrva)))
if 6 in streams:
    tid, _ = struct.unpack_from('<II', d, streams[6][1])
    code, flags, rec, addr, npar = struct.unpack_from('<IIQQI', d, streams[6][1] + 8)
    info = struct.unpack_from('<15Q', d, streams[6][1] + 8 + 32)
    csz, crva = struct.unpack_from('<II', d, streams[6][1] + 8 + 152)
    print(f'EXCEPTION thread={tid} code={code:#x} flags={flags:#x} address={addr:#x} params={[hex(x) for x in info[:npar]]}')
    for b, s, nm in mods:
        if b <= addr < b + s: print(f'  in module {nm} base={b:#x} RVA={addr-b:#x}')
    ctx = d[crva:crva+csz]
    names = ['Rax','Rcx','Rdx','Rbx','Rsp','Rbp','Rsi','Rdi','R8','R9','R10','R11','R12','R13','R14','R15','Rip']
    vals = struct.unpack_from('<17Q', ctx, 0x78); print('  ' + ' '.join(f'{n}={v:#x}' for n, v in zip(names, vals)))
    rsp = vals[4]
print('modules:', [(hex(b), n.split('\\')[-1]) for b, s, n in mods if 'game_scarlett' in n or 'XCRD' in n or 'xgame' in n])
# --- стек исключившего потока ---
if 3 in streams and 6 in streams:
    cnt = struct.unpack_from('<I', d, streams[3][1])[0]
    for i in range(cnt):
        off = streams[3][1] + 4 + 48 * i
        t, sc, sp, prio, teb, stk_start, stk_size, stk_rva, ctx_sz, ctx_rva = struct.unpack_from('<IIIIQQIIII', d, off)
        if t == tid:
            print(f'stack: {stk_start:#x} size={stk_size:#x}')
            n_q = min(48, stk_size // 8)
            exe = [m for m in mods if 'game_scarlett' in m[2]][0]
            for k in range(n_q):
                addr = rsp + 8 * k
                if not (stk_start <= addr < stk_start + stk_size): break
                v = struct.unpack_from('<Q', d, stk_rva + (addr - stk_start))[0]
                tag = f'  <- exe RVA {v - exe[0]:#x}' if exe[0] <= v < exe[0] + exe[1] else ''
                print(f'  [rsp+{8*k:#04x}] {v:#018x}{tag}')
