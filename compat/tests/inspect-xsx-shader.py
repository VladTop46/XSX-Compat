#!/usr/bin/env python3
"""Decode checked FXCP metadata/bindings and inspect native GPU instructions.

Read-only: no Xbox DLL is loaded, no native resource/vtable or code is changed.
"""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import runpy
import struct

from xsx_shader_metadata import inspect_sections, root_signature, words
from xsx_shader_isa import LLVMDisassembler, inspect_isa, listing
from xsx_shader_bindings import analyze_bindings

fxcp = runpy.run_path(str(Path(__file__).with_name('inspect-xsx-native-pso.py')))


def inspect_container(blob):
    sections = fxcp['parse_fxcp'](blob)
    data = {section['name']: blob[section['offset']:section['offset'] + section['size']]
            for section in sections}
    result = inspect_sections(data['root-signature.dxbc'], data['instructions.bin'],
                              data['shader-meta.bin'])
    result.update({'container_sha256': hashlib.sha256(blob).hexdigest(),
                   'instructions_sha256': hashlib.sha256(data['instructions.bin']).hexdigest(),
                   'instruction_bytes': len(data['instructions.bin']),
                   'packet_hex_opaque': data['packet.bin'].hex(),
                   'packet_metadata_hex_opaque': data['packet-meta.bin'].hex()})
    if len(data['packet.bin']) % 4 == 0:
        result['packet_words_opaque'] = list(struct.unpack(
            '<' + 'I' * (len(data['packet.bin']) // 4), data['packet.bin']))
    return result, data['instructions.bin']


def inspect_accc(blob):
    """Read the observed FSPX v1 dialect inside one complete DXBC container."""
    root_signature(blob)  # Validates all DXBC chunk bounds/overlap as well as RTS0.
    count = words(blob, 28, 1)[0]
    chunks = []
    for offset in words(blob, 32, count):
        if blob[offset:offset + 4] == b'ACCC':
            size = words(blob, offset + 4, 1)[0]
            chunks.append(blob[offset + 8:offset + 8 + size])
    if len(chunks) != 1:
        raise ValueError('expected one ACCC chunk')
    data = chunks[0]
    header = words(data, 0, 8)
    if header != (int.from_bytes(b'FSPX', 'little'), 1, 4, len(data), 0, 0x50, 0x1c, 0):
        raise ValueError('unsupported FSPX header')
    sections, spans = {}, []
    words(data, 0x50, 7)  # Packet/launch block, semantics still inferred.
    for index in range(3):
        typ, offset, size, reserved = words(data, 0x20 + index * 16, 4)
        if typ not in (1, 6, 7) or typ in sections or reserved:
            raise ValueError('unknown/duplicate FSPX section')
        if not size or offset < 0x6c or offset % 4 or offset + size > len(data):
            raise ValueError('invalid FSPX section range')
        if any(offset < end and start < offset + size for start, end in spans):
            raise ValueError('overlapping FSPX sections')
        spans.append((offset, offset + size))
        sections[typ] = data[offset:offset + size]
    if len(sections[1]) != 20:
        raise ValueError('unrecognized FSPX type-1 size')
    code = sections[6]
    result = inspect_sections(blob, code, sections[7])
    packet = data[0x50:0x6c]
    result.update({'format': 'DXBC/ACCC/FSPX v1 (observed dialect)',
                   'container_sha256': hashlib.sha256(blob).hexdigest(),
                   'instructions_sha256': hashlib.sha256(code).hexdigest(),
                   'instruction_bytes': len(code), 'packet_hex_opaque': packet.hex(),
                   'packet_words_opaque': list(words(packet, 0, 7)),
                   'packet_metadata_hex_opaque': sections[1].hex()})
    return result, code


def containers(pe):
    for _, va, raw_size, raw in pe.sections:
        data = pe.data[raw:raw + raw_size]
        position = 0
        while (position := data.find(b'FXCP', position)) >= 0:
            offset = position
            position += 4
            if offset + 56 > len(data):
                continue
            header = struct.unpack_from('<14I', data, offset)
            if header[1:3] == (1, 2):
                yield va + offset, data[offset:offset + header[3]]


def survey(dll):
    pe = fxcp['PE'](dll)
    fxcp['native_call'](pe)
    entries, errors = [], []
    types, dimensions, roots, tokens = Counter(), Counter(), Counter(), Counter()
    for rva, blob in containers(pe):
        try:
            result, _ = inspect_container(blob)
            types.update(b['type'] for b in result['matched_bindings'])
            dimensions.update(f'{b["type"]}:{b["dimension"]}' for b in result['matched_bindings'])
            roots[str(result['root_signature']['version'])] += 1
            tokens.update(hex(r['raw_words'][2]) for r in result['metadata']['opaque_0x42'])
            entries.append({'container_rva': hex(rva), 'size': len(blob),
                            'instructions_size': result['instruction_bytes'],
                            'binding_count': len(result['matched_bindings']),
                            'shader_hash': result['prefix']['hash_hex']})
        except (ValueError, struct.error) as error:
            errors.append({'container_rva': hex(rva), 'error': str(error)})
    if not entries and not errors:
        raise ValueError('no Xbox FXCP containers found')
    return {'dll': str(dll), 'dll_sha256': hashlib.sha256(pe.data).hexdigest(),
            'containers_checked': len(entries), 'errors': errors,
            'matched_bindings': dict(types), 'binding_dimensions': dict(dimensions),
            'root_versions': dict(roots), 'opaque_0x42_tokens': dict(tokens), 'entries': entries,
            'scope': '0x403 structure/hash/root-binding consistency; no GPU execution or translation'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    selector = parser.add_mutually_exclusive_group(required=True)
    selector.add_argument('--log', type=Path)
    selector.add_argument('--root-rva', type=lambda value: int(value, 0))
    selector.add_argument('--container', type=Path)
    selector.add_argument('--dxbc', type=Path, help='complete DXBC with native ACCC/FSPX, such as game FSR4')
    selector.add_argument('--all', action='store_true')
    parser.add_argument('--dll', type=Path, default=Path(__file__).resolve().parents[2] /
                        'tests/03-fidelityfx/amd_fidelityfx_upscaler_xbox.dll')
    parser.add_argument('--isa', action='store_true')
    parser.add_argument('--cpu', choices=('gfx1010', 'gfx1013', 'gfx1030'), default='gfx1030')
    parser.add_argument('--wave', type=int, choices=(32, 64), default=32)
    parser.add_argument('--out', type=Path)
    parser.add_argument('--listing', type=Path)
    args = parser.parse_args()
    if args.all and (args.isa or args.listing):
        parser.error('--all surveys metadata; ISA inspection requires a selected container')
    if args.listing and not args.isa:
        parser.error('--listing requires --isa')
    try:
        if args.all:
            result = survey(args.dll)
            status = int(bool(result['errors']))
        else:
            provenance = {}
            if args.container or args.dxbc:
                path = args.container or args.dxbc
                blob = path.read_bytes()
                provenance['container_file'] = str(path)
            else:
                rva, size = fxcp['select_from_log'](args.log) if args.log else (args.root_rva, None)
                selected = fxcp['inspect'](args.dll, rva, size)
                pe = fxcp['PE'](args.dll)
                blob = pe.read(int(selected['container_rva'], 0), selected['container_size'])
                provenance = {k: selected[k] for k in ('dll', 'dll_sha256', 'container_rva', 'root_rva')}
            result, code = inspect_accc(blob) if args.dxbc else inspect_container(blob)
            result.update(provenance)
            if args.isa:
                with LLVMDisassembler(args.cpu, args.wave) as decoder:
                    result['isa'] = inspect_isa(code, decoder)
                result['binding_provenance'] = analyze_bindings(result)
                if args.listing:
                    args.listing.write_text(listing(result['isa']))
            status = 0
        text = json.dumps(result, indent=2) + '\n'
        if args.out:
            args.out.write_text(text)
        else:
            print(text, end='')
        return status
    except (OSError, ValueError, struct.error, AttributeError) as error:
        parser.exit(1, f'{error}\n')


if __name__ == '__main__':
    raise SystemExit(main())
