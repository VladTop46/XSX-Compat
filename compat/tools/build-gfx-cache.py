#!/usr/bin/env python3
"""Translate the requested game VS/PS programs (by 128-bit hash) into a runtime shader cache.

For every hash: <out>/<hash>.dxil (DXC, SM6.0, unsigned), <hash>.bind (text bindings for d3d12_xs),
<hash>.hlsl (generated source). Programs that the lowering rejects are listed in <out>/failures.txt
with the exact reason; nothing is substituted. Reads the pack read-only.
"""
import argparse
import json
import mmap
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / 'compat/tests'))
from xsx_shader_pack import inflate_record, records
from xsx_gfx_translate import translate_graphics


def bind_text(b):
    lines = [f'stage {b["stage"]}', f'hash {b["hash"]}']
    lines += [f'buffer {x["slot_byte_offset"]} {x["dwords"]} {x["register"]} {x["space"]}' for x in b['buffers']]
    lines += [f'texture {x["slot_byte_offset"]} {x["dwords"]} {x["register"]} {x["space"]}' for x in b['textures']]
    lines += ['sampler ' + ' '.join(str(w) for w in x['words']) + f' {x["register"]} {x["space"]}' for x in b['samplers']]
    c = b['vsharp_cbuffer']
    lines.append(f'vsharp {c["register"]} {c["space"]} {c["count"]}')
    lines += [f'vsin {x["semantic"]} {x["index"]} {x["mask"]} {x["first_vgpr"]}' for x in b['vs_inputs']]
    return '\n'.join(lines) + '\n'


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--pack', type=Path, required=True)
    ap.add_argument('--manifest', type=Path, default=REPO / 'compat/reports/native-shaders-20261124/pack/manifest.json')
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--dxc', type=Path, default=REPO / 'sdk/dxc/bin/dxc')
    ap.add_argument('hashes', nargs='+', help='32-hex program hashes')
    args = ap.parse_args()
    manifest = json.loads(args.manifest.read_text())
    want, offsets = set(args.hashes), {}
    for e in manifest['entries']:
        if e.get('shader_hash') in want:
            offsets[int(e['offset'], 0)] = e['shader_hash']
    missing = want - set(offsets.values())
    args.out.mkdir(parents=True, exist_ok=True)
    fails = [f'{h} not in pack manifest' for h in sorted(missing)]
    done = 0
    with args.pack.open('rb') as fh, mmap.mmap(fh.fileno(), 0, access=mmap.ACCESS_READ) as data:
        for rec in records(data):
            h = offsets.get(rec['offset'])
            if not h:
                continue
            blob, _ = inflate_record(data, rec)
            try:
                r = translate_graphics(blob)
                (args.out / f'{h}.hlsl').write_text(r['hlsl'])
                prof = 'vs_6_0' if r['bindings']['stage'] == 'VS' else 'ps_6_0'
                c = subprocess.run([str(args.dxc), '-T', prof, '-E', 'main', '-Ges', '-Fo', str(args.out / f'{h}.dxil'), str(args.out / f'{h}.hlsl')], capture_output=True, text=True)
                if c.returncode:
                    raise ValueError('dxc: ' + c.stderr.strip().splitlines()[0])
                (args.out / f'{h}.bind').write_text(bind_text(r['bindings']))
                done += 1
            except ValueError as error:
                fails.append(f'{h} {rec["name"]}: {error}')
    (args.out / 'failures.txt').write_text('\n'.join(fails) + ('\n' if fails else ''))
    print(f'translated {done}/{len(want)}; failures: {len(fails)}')
    for f in fails:
        print(' ', f)


if __name__ == '__main__':
    main()
