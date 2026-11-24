#!/usr/bin/env python3
"""Locate and extract the Xbox FXCP native PSO selected by a logged root call.

Section labels follow the checked Xbox +0x5717..0x5765 descriptor builder.
No DLL code is loaded or modified; decoding ISA is a separate experiment.
"""
import argparse
import hashlib
import json
import re
import runpy
import struct
import uuid
from pathlib import Path

PE = runpy.run_path(str(Path(__file__).with_name('inspect-ffx-abi.py')))['PE']
SECTION_NAMES = ('root-signature.dxbc', 'packet.bin', 'packet-meta.bin',
                 'instructions.bin', 'shader-meta.bin')


def native_call(pe):
    # This is a check of this DLL's descriptor builder, not an Xbox header.
    # The source instruction bytes are copied into the XMem buffer before call.
    pe.expect(0x5700, '48 8b 55 88 4c 8b c7 48 8b ce e8 59 f9 03 00 '
              '48 8b 45 b0 4c 8d 4b 10 48 8b 4d b8 '
              '4c 8d 05 d6 1d 06 00 48 89 45 f0 48 8d 55 f0 '
              '48 8b 44 24 68 48 89 45 f8 8b 44 24 70 48 89 45 00 '
              '48 8b 44 24 78 48 89 45 08 8b 45 80 48 89 45 10 '
              '48 8b 45 98 48 89 45 28 8b 45 a0 48 89 45 30 '
              '48 89 75 18 48 89 7d 20 48 8b 01 ff 90 18 02 00 00')
    iid = str(uuid.UUID(bytes_le=pe.read(pe.rip_target(0x571b), 16)))
    if iid != '765a30f3-f624-4c6f-a828-ace948622445':
        raise ValueError(f'native call requests unexpected IID {iid}')
    return {'call_rva': '0x5765', 'vtable_offset': '0x218',
            'requested_iid': iid, 'requested_interface': 'ID3D12PipelineState',
            'descriptor_bytes_written': 72,
            'descriptor_offsets': {'root_signature': 0, 'packet': 8, 'packet_size': 16,
                                   'packet_meta': 24, 'packet_meta_size': 32,
                                   'instructions': 40, 'instructions_size': 48,
                                   'shader_meta': 56, 'shader_meta_size': 64},
            'method_identification': 'Inferred native compute PSO deserialization; '
                                     'exact Xbox declaration is unavailable.'}


def parse_fxcp(blob):
    if len(blob) < 56:
        raise ValueError('truncated FXCP header')
    header = struct.unpack_from('<14I', blob)
    if header[:3] != (int.from_bytes(b'FXCP', 'little'), 1, 2):
        raise ValueError('expected Xbox FXCP v1, backend 2')
    if header[3] != len(blob):
        raise ValueError('FXCP size does not match container')
    sections = []
    ranges = []
    for index, name in enumerate(SECTION_NAMES):
        offset, size = header[4 + index * 2:6 + index * 2]
        if not size:
            if offset or index in (0, 1, 3):
                raise ValueError(f'{name}: missing required section or nonzero empty offset')
        else:
            if offset < 56 or offset % 16 or offset + size > len(blob):
                raise ValueError(f'{name}: invalid range or alignment')
            if any(offset < end and start < offset + size for start, end in ranges):
                raise ValueError(f'{name}: overlapping section')
            ranges.append((offset, offset + size))
        data = blob[offset:offset + size]
        sections.append({'name': name, 'offset': offset, 'size': size,
                         'sha256': hashlib.sha256(data).hexdigest(),
                         'prefix': data[:32].hex(' '),
                         'DXBC_markers': data.count(b'DXBC'),
                         'DXIL_markers': data.count(b'DXIL'),
                         'LLVM_bitcode_markers': data.count(b'BC\xc0\xde')})
    return sections


def dxbc_chunks(data):
    if len(data) < 32 or data[:4] != b'DXBC':
        raise ValueError('root signature is not a DXBC container')
    size, count = struct.unpack_from('<2I', data, 24)
    if size != len(data) or 32 + count * 4 > size:
        raise ValueError('invalid DXBC header size')
    chunks = []
    for i in range(count):
        offset = struct.unpack_from('<I', data, 32 + i * 4)[0]
        if offset < 32 + count * 4 or offset + 8 > size:
            raise ValueError('invalid DXBC chunk offset')
        tag, length = struct.unpack_from('<4sI', data, offset)
        if offset + 8 + length > size:
            raise ValueError('truncated DXBC chunk')
        chunks.append({'fourcc': tag.decode('ascii'), 'size': length})
    return chunks


def select_from_log(log):
    text = log.read_text()
    pair = re.search(r'\[ABI ADDRESS\] root signature caller=([0-9a-fA-F]+) '
                     r'amd_fidelityfx_upscaler_xbox\.dll\+0x([0-9a-fA-F]+)\n'
                     r'\[ROOT\] enter node=\d+ data=([0-9a-fA-F]+) size=(\d+)', text)
    if not pair:
        raise ValueError('log has no Xbox root signature caller/data pair')
    base = int(pair[1], 16) - int(pair[2], 16)
    rva = int(pair[3], 16) - base
    if not 0 <= rva < 2**32:
        raise ValueError('root signature is outside Xbox PE image')
    return rva, int(pair[4])


def inspect(dll, root_rva, root_size=None, output=None):
    pe = PE(dll)
    call = native_call(pe)
    matches = []
    for _, va, raw_size, raw in pe.sections:
        data = pe.data[raw:raw + raw_size]
        position = 0
        while (position := data.find(b'FXCP', position)) >= 0:
            offset = position
            position += 4
            if offset + 56 > len(data):
                continue
            header = struct.unpack_from('<14I', data, offset)
            if header[1:3] != (1, 2) or va + offset + header[4] != root_rva:
                continue
            size = header[3]
            if size < 56 or offset + size > len(data):
                raise ValueError('selected FXCP container is truncated')
            blob = data[offset:offset + size]
            sections = parse_fxcp(blob)
            if root_size is not None and sections[0]['size'] != root_size:
                raise ValueError('selected root size differs from log')
            matches.append((va + offset, blob, sections))
    if len(matches) != 1:
        raise ValueError(f'expected one FXCP for root RVA {root_rva:#x}, found {len(matches)}')
    rva, blob, sections = matches[0]
    root = sections[0]
    result = {'dll': str(dll), 'dll_sha256': hashlib.sha256(pe.data).hexdigest(),
              'container_rva': hex(rva), 'root_rva': hex(root_rva),
              'version': 1, 'backend': 2, 'container_size': len(blob),
              'container_sha256': hashlib.sha256(blob).hexdigest(),
              'native_call': call,
              'root_chunks': dxbc_chunks(blob[root['offset']:root['offset'] + root['size']]),
              'sections': sections,
              'limitation': 'Marker scan does not decode ISA or prove absence of compressed IR.'}
    if output:
        output.mkdir(parents=True, exist_ok=True)
        (output / 'container.fxcp').write_bytes(blob)
        for section in sections:
            start, size = section['offset'], section['size']
            (output / section['name']).write_bytes(blob[start:start + size])
        (output / 'manifest.json').write_text(json.dumps(result, indent=2) + '\n')
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('dll', nargs='?', type=Path,
                        default=Path(__file__).resolve().parents[2] /
                        'tests/03-fidelityfx/amd_fidelityfx_upscaler_xbox.dll')
    selector = parser.add_mutually_exclusive_group(required=True)
    selector.add_argument('--log', type=Path)
    selector.add_argument('--root-rva', type=lambda value: int(value, 0))
    parser.add_argument('--out-dir', type=Path)
    args = parser.parse_args()
    try:
        rva, size = select_from_log(args.log) if args.log else (args.root_rva, None)
        print(json.dumps(inspect(args.dll, rva, size, args.out_dir), indent=2))
    except (OSError, ValueError, struct.error) as error:
        parser.exit(1, f'{error}\n')
