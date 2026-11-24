#!/usr/bin/env python3 -I
"""Статический разбор DXBC+ACCC (ACO-cooked native Xbox compute shader) из amd_fidelityfx_upscaler_xbox.dll.
Не загружает DLL. Использование: inspect-accc.py <dll> <out_dir> [--mcpu gfx1030]
Формат ACCC (по наблюдению 29 контейнеров, НЕ документирован):
  u32 magic 'FSPX', ver(=1), hdr2(=4), payload_size, 0, tbl_end(0x50), rec_size(0x1c), 0
  @0x20: 16-байтные записи (type, offset, size, 0) до tbl_end; type 6 = инструкции, 7 = метаданные, 1 = 0x14 байт неизвестного назначения
  @tbl_end: блок rec_size байт (вероятно регистры PGM_RSRC/launch state — не расшифровано)."""
import re, struct, sys, json, os, subprocess, hashlib
dll, out = sys.argv[1], sys.argv[2]
mcpu = sys.argv[sys.argv.index('--mcpu')+1] if '--mcpu' in sys.argv else 'gfx1030'
d = open(dll, 'rb').read(); os.makedirs(out, exist_ok=True); items = []
for mm in re.finditer(rb'DXBC', d):
    o = mm.start()
    if o + 32 > len(d): continue
    total, cnt = struct.unpack_from('<II', d, o + 24)
    if not (32 <= total <= 16 << 20 and 1 <= cnt <= 32 and o + total <= len(d)): continue
    chunks = {}
    for i in range(cnt):
        off = struct.unpack_from('<I', d, o + 32 + 4 * i)[0]
        tag = d[o+off:o+off+4].decode('latin1'); sz = struct.unpack_from('<I', d, o+off+4)[0]
        chunks[tag] = (o+off+8, sz)
    if 'ACCC' not in chunks: continue
    a, asz = chunks['ACCC']; b = d[a:a+asz]
    if b[:4] != b'FSPX': continue
    _, ver, h2, payload, _, tend, rec, _ = struct.unpack_from('<8I', b, 0)
    ents = [struct.unpack_from('<4I', b, 0x20 + 16*i) for i in range((tend - 0x20)//16)]
    sec = {t: b[off:off+sz] for t, off, sz, _ in ents}
    idx = len(items); name = f'{idx:02d}_{o:#x}'
    code = sec.get(6, b''); meta = sec.get(7, b'')
    open(f'{out}/{name}.code.bin', 'wb').write(code); open(f'{out}/{name}.meta.bin', 'wb').write(meta)
    hexs = ' '.join(f'0x{x:02x}' for x in code)
    r = subprocess.run(['llvm-mc', '--disassemble', f'-mcpu={mcpu}', '-triple=amdgcn-amd-amdhsa'], input=hexs.encode(), capture_output=True)
    bad = r.stderr.count(b'invalid instruction')
    items.append(dict(name=name, dxbc_off=hex(o), dxbc_size=total, rts0=chunks['RTS0'][1], psv0=chunks['PSV0'][1],
        accc=asz, hdr2=h2, payload=payload, entries=[(t, hex(off), hex(sz)) for t, off, sz, _ in ents],
        code=len(code), meta=len(meta), code_sha=hashlib.sha256(code).hexdigest()[:16],
        decode_invalid=bad, decode_lines=r.stdout.count(b'\n'), regs_block=b[tend:tend+rec].hex()))
json.dump(items, open(f'{out}/manifest.json', 'w'), indent=1)
print(len(items), 'ACCC shaders; code bytes', sum(i['code'] for i in items), 'meta', sum(i['meta'] for i in items))
print(f'mcpu={mcpu}: containers with 0 invalid decode:', sum(1 for i in items if i['decode_invalid'] == 0), 'of', len(items))
