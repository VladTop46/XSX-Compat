#!/usr/bin/env python3
"""Extract full native ISA (+FSSM info) of given shader hashes from shaders.rpf (read-only)."""
import json, mmap, sys
from pathlib import Path
REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'compat/tests'))
from xsx_shader_pack import inflate_record, records, inspect_shader
pack, out, hashes = Path(sys.argv[1]), Path(sys.argv[2]), set(sys.argv[3:])
man = json.loads((REPO / 'compat/reports/native-shaders-20261124/pack/manifest.json').read_text())
offs = {int(e['offset'], 0): e['shader_hash'] for e in man['entries'] if e.get('shader_hash') in hashes}
out.mkdir(parents=True, exist_ok=True)
with pack.open('rb') as fh, mmap.mmap(fh.fileno(), 0, access=mmap.ACCESS_READ) as data:
    for rec in records(data):
        h = offs.get(rec['offset'])
        if not h: continue
        blob, _ = inflate_record(data, rec)
        insp, payloads = inspect_shader(blob)
        p = payloads[0]
        (out / f'{h}.isa').write_bytes(p['code']); (out / f'{h}.fssm').write_bytes(p['fssm'])
        (out / f'{h}.json').write_text(json.dumps(insp, indent=1, default=str))
        print(h, rec['name'], insp['stage'], 'code', len(p['code']), 'fssm', len(p['fssm']))
