#!/usr/bin/env python3
"""Read-only census of named shader records in the observed RPF9 pack.

Writes a manifest and a few sample containers to the requested output folder.
This is a validated record scan, not an implementation of the encrypted TOC.
"""
import argparse
from collections import Counter
import hashlib
import json
import mmap
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tests'))
from xsx_shader_pack import inflate_record, inspect_shader, records


def export(directory, record, blob, inspection, payloads):
    directory.mkdir(parents=True, exist_ok=True)
    (directory / 'shader.dxbc').write_bytes(blob)
    (directory / 'manifest.json').write_text(json.dumps({'record': record, **inspection}, indent=2) + '\n')
    for i, payload in enumerate(payloads):
        (directory / f'variant-{i}.fssm').write_bytes(payload['fssm'])
        (directory / f'variant-{i}.code.bin').write_bytes(payload['code'])


def census(pack, output):
    before = pack.stat()
    rows, counts, stages, formats, failures = [], Counter(), Counter(), Counter(), Counter()
    first, smallest_cs = {}, None
    with pack.open('rb') as handle, mmap.mmap(handle.fileno(), 0, access=mmap.ACCESS_READ) as data:
        pack_hash = hashlib.sha256(data).hexdigest()
        for record in records(data):
            counts['record_candidates'] += 1
            row = {'offset': hex(record['offset']), 'name': record['name'],
                   'compressed_size': record['compressed_size'], 'expanded_size': record['expanded_size']}
            try:
                blob, trailer = inflate_record(data, record)
                row['trailer_hex_opaque'] = trailer.hex()
                inspection, payloads = inspect_shader(blob)
            except ValueError as error:
                row['error'] = str(error)
                failures[str(error).split(':', 1)[0]] += 1
            else:
                counts['validated_native_containers'] += 1
                counts['native_variants'] += len(payloads)
                stage = inspection['stage']
                stages[stage] += 1
                formats[inspection['format']] += 1
                row.update(stage=stage, shader_model=inspection['shader_model'], shader_hash=inspection['hash'],
                           code_sizes=[v['code_size'] for v in inspection['native_variants']],
                           hardware_stages=[[s['hardware_stage_raw'] for s in v['hardware_stages']]
                                            for v in inspection['native_variants']])
                sample = record, blob, inspection, payloads
                if stage not in first:
                    first[stage] = sample
                if stage == 'CS':
                    score = (sum(row['code_sizes']), len(blob), record['offset'])
                    if smallest_cs is None or score < smallest_cs[0]:
                        smallest_cs = score, sample
            rows.append(row)
        after = pack.stat()
        if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
            raise ValueError('pack changed while being inspected; retry after download finishes')
    if not counts['validated_native_containers']:
        raise ValueError('no validated native shader containers found')
    output.mkdir(parents=True, exist_ok=True)
    for stage, sample in first.items():
        export(output / f'first-{stage}', *sample)
    if smallest_cs:
        export(output / 'smallest-CS', *smallest_cs[1])
    result = {'source': str(pack), 'size': before.st_size, 'sha256': pack_hash,
              'counts': dict(counts), 'stages': dict(stages), 'formats': dict(formats),
              'errors': dict(failures), 'entries': rows,
              'scope': 'Named-record scan with strict DEFLATE/DXBC/XDAT/XBCH/FSSM/hash validation; not a full RPF TOC parser.'}
    (output / 'manifest.json').write_text(json.dumps(result, indent=2) + '\n')
    return result


def extract_record(pack, offset, output):
    before = pack.stat()
    with pack.open('rb') as handle, mmap.mmap(handle.fileno(), 0, access=mmap.ACCESS_READ) as data:
        selected = None
        for record in records(data):
            if record['offset'] == offset:
                selected = record
                break
            if record['offset'] > offset:
                break
        if selected is None:
            raise ValueError(f'no validated named-record boundary at {offset:#x}')
        blob, trailer = inflate_record(data, selected)
        inspection, payloads = inspect_shader(blob)
        selected = dict(selected, trailer_hex_opaque=trailer.hex(),
                        source_pack_sha256=hashlib.sha256(data).hexdigest())
        after = pack.stat()
        if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
            raise ValueError('pack changed during extraction')
    export(output, selected, blob, inspection, payloads)
    return {'record': selected, 'stage': inspection['stage'], 'hash': inspection['hash']}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('pack', type=Path)
    parser.add_argument('--out-dir', required=True, type=Path)
    parser.add_argument('--record-offset', type=lambda s: int(s, 0), help='extract one record at the exact validated boundary')
    args = parser.parse_args()
    try:
        if args.record_offset is not None:
            print(json.dumps(extract_record(args.pack, args.record_offset, args.out_dir), indent=2))
            return 0
        result = census(args.pack, args.out_dir)
        print(json.dumps({k: v for k, v in result.items() if k != 'entries'}, indent=2))
        return 0 if not result['errors'] else 1
    except (OSError, ValueError) as error:
        parser.exit(1, f'{error}\n')


if __name__ == '__main__':
    raise SystemExit(main())
