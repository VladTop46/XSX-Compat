#!/usr/bin/env python3
"""Match bounded runtime PSO snapshots to independently validated pack hashes.

Reads files only. A zero captured buffer does not establish its input/output
role. The unknown Xbox description is never passed to a PC graphics API.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct


def inspect(directory, manifest):
    by_hash = {}
    for record in manifest['entries']:
        if 'shader_hash' in record:
            by_hash.setdefault(record['shader_hash'], []).append(record)
    result = []
    for path in sorted(directory.glob('slot57-??-desc.bin')):
        desc = path.read_bytes()
        if len(desc) != 256:
            raise ValueError(f'{path.name}: expected 256-byte diagnostic window')
        q = struct.unpack('<32Q', desc)
        stem = path.name.removesuffix('-desc.bin')
        pairs, matches = [], []
        for index in range(15):
            pointer, size = q[1 + index * 2:3 + index * 2]
            blob_path = directory / f'{stem}-pair{index:02}.bin'
            row = {'index': index, 'desc_offset': hex(8 + index * 16),
                   'pointer': hex(pointer), 'size': size, 'saved': blob_path.exists()}
            if blob_path.exists():
                if blob_path.stat().st_size != size or size > 128 << 10:
                    raise ValueError(f'{blob_path.name}: capture size differs from description or exceeds limit')
                blob = blob_path.read_bytes()
                row.update(sha256=hashlib.sha256(blob).hexdigest(), all_zero=not any(blob))
                # Observed metadata v2: length field excludes 12-byte header;
                # a 128-bit hash precedes the final 12 zero bytes. Only match
                # independently indexed hashes, without inventing tag meanings.
                if (len(blob) >= 28 and struct.unpack_from('<2I', blob) == (2, len(blob) - 12)
                        and blob[-12:] == bytes(12)):
                    shader_hash = blob[-28:-12].hex()
                    references = by_hash.get(shader_hash, [])
                    if references:
                        stages = sorted({r['stage'] for r in references})
                        match = {'metadata_pair': index, 'program_hash': shader_hash,
                                 'stages_from_pack': stages,
                                 'records': [{key: r[key] for key in ('name', 'offset', 'stage', 'code_sizes')}
                                             for r in references]}
                        matches.append(match)
                        row['matched_hash'] = shader_hash
            pairs.append(row)
        result.append({'capture': stem, 'desc_sha256': hashlib.sha256(desc).hexdigest(),
                       'root_pointer_opaque': hex(q[0]), 'pairs': pairs, 'shader_matches': matches})
    if not result:
        raise ValueError('no slot57 description snapshots found')
    matches = [m for row in result for m in row['shader_matches']]
    return {'captures': result,
            'summary': {'descriptions': len(result), 'metadata_hash_matches': len(matches),
                        'unique_program_hashes': len({m['program_hash'] for m in matches})},
            'limitations': [
                'A match identifies a shader program; it does not decode the complete PSO or translate its ISA.',
                'Stage names come from the validated pack, not from assumed PC description field offsets.',
                'Pointer/size fields and the input/output role of zero buffers remain partly unconfirmed.',
                'Only the first 256 description bytes and explicitly saved blocks were inspected.']}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture_dir', type=Path)
    parser.add_argument('--pack-manifest', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    try:
        report = inspect(args.capture_dir, json.loads(args.pack_manifest.read_text()))
    except (OSError, ValueError, KeyError, struct.error) as error:
        parser.exit(1, f'[FAIL] {error}\n')
    report['source_pack_sha256'] = json.loads(args.pack_manifest.read_text())['sha256']
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    print('[PASS] runtime PSO inspection ' + json.dumps(report['summary']))


if __name__ == '__main__':
    main()
