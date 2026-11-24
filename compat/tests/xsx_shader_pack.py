"""Bounded inspection of observed RPF shader records and DXIL/XDAT/XBCH/FSSM.

This scans named records, not the encrypted RPF TOC. Formats are independently
checked against local bytes; no third-party implementation is embedded here.
No native DLL or shader is executed. Unknown container dialects fail explicitly.
"""
import hashlib
import re
import struct
import zlib

from xsx_shader_metadata import nop_hash, words

MAX_BLOB = 64 << 20
KINDS = ('PS', 'VS', 'GS', 'HS', 'DS', 'CS', 'Library', 'RayGen', 'Intersection',
         'AnyHit', 'ClosestHit', 'Miss', 'Callable', 'MS', 'AS', 'Node')
NAME_RUN = re.compile(rb'[A-Za-z0-9_:]{4,256}\x00')


def bounded(data, offset, size):
    if offset < 0 or size < 0 or offset + size > len(data):
        raise ValueError(f'blob range outside input: {offset:#x}+{size:#x}')
    return data[offset:offset + size]


def inflate(data, size, *, wbits):
    if not 0 < size <= MAX_BLOB:
        raise ValueError('expanded size outside supported limit')
    decoder = zlib.decompressobj(wbits)
    try:
        result = decoder.decompress(data, size + 1)
    except zlib.error as error:
        raise ValueError(f'invalid compressed stream: {error}') from error
    if len(result) != size or not decoder.eof or decoder.unused_data or decoder.unconsumed_tail:
        raise ValueError('compressed stream length, EOF or trailing-data mismatch')
    return result


def records(data):
    """Yield non-overlapping named-record candidates with bounded payloads.

    A maximal identifier run may include the length byte itself (e.g. ASCII B
    for a 66-byte field). Recover the longest structurally valid name ending at
    the NUL, preserving effect qualifiers and suppressing suffix aliases.
    """
    if data[:4] != b'9FPR':
        raise ValueError('expected observed RPF9 magic 9FPR')
    position = 4
    while match := NAME_RUN.search(data, position):
        terminator = match.end() - 1
        position = match.end()
        if terminator + 18 > len(data):
            continue
        compressed, opaque, duplicate, expanded = struct.unpack_from('<4I', data, terminator + 2)
        payload = terminator + 18
        if compressed != duplicate or not 0 < compressed <= 16 << 20 or not 0 < expanded <= MAX_BLOB:
            continue
        if payload + compressed > len(data):
            continue
        for start in range(max(0, match.start() - 1, terminator - 255), terminator - 3):
            first = data[start + 1]
            if data[start] != terminator - start or not (65 <= first <= 90 or 97 <= first <= 122):
                continue
            name = data[start + 1:terminator].decode('ascii')
            yield {'offset': start, 'name': name, 'flag': data[terminator + 1],
                   'opaque_word': opaque, 'payload_offset': payload,
                   'compressed_size': compressed, 'expanded_size': expanded}
            position = payload + compressed
            break


def inflate_record(data, record):
    payload = bounded(data, record['payload_offset'], record['compressed_size'])
    if len(payload) <= 12:
        raise ValueError('record has no DEFLATE stream before observed 12-byte trailer')
    # The local pack includes a 12-byte opaque trailer in BOTH compressed-size
    # fields. It is not part of DEFLATE. Require EOF before it; do not silently
    # permit arbitrary trailing bytes in the decompressor.
    return inflate(payload[:-12], record['expanded_size'], wbits=-15), payload[-12:]


def dxbc_parts(blob):
    if len(blob) < 32 or blob[:4] != b'DXBC' or words(blob, 20, 1) != (1,):
        raise ValueError('unsupported DXBC header')
    size, count = words(blob, 24, 2)
    if size != len(blob) or not 1 <= count <= 64:
        raise ValueError('DXBC size/count mismatch')
    offsets = words(blob, 32, count)
    parts, spans = [], []
    for offset in offsets:
        if offset < 32 + count * 4:
            raise ValueError('DXBC part overlaps header')
        length = words(blob, offset + 4, 1)[0]
        data = bounded(blob, offset + 8, length)
        end = offset + 8 + length
        if any(offset < b and a < end for a, b in spans):
            raise ValueError('overlapping DXBC parts')
        spans.append((offset, end))
        try:
            tag = blob[offset:offset + 4].decode('ascii')
        except UnicodeDecodeError as error:
            raise ValueError('non-ASCII DXBC part tag') from error
        parts.append({'tag': tag, 'offset': offset + 8, 'size': length, 'data': data})
    return parts


def one(parts, tag):
    matches = [p for p in parts if p['tag'] == tag]
    if len(matches) != 1:
        raise ValueError(f'expected one {tag} part, found {len(matches)}')
    return matches[0]['data']


def signature(data):
    count, offset = words(data, 0, 2)
    if offset != 8 or count > 128:
        raise ValueError('unsupported signature element array')
    words(data, offset, count * 8)
    result = []
    for i in range(count):
        stream, name, index, system, component, register, masks, precision = words(data, offset + i * 32, 8)
        if name < offset + count * 32 or name >= len(data):
            raise ValueError('signature name outside string table')
        end = data.find(b'\x00', name)
        if end < 0:
            raise ValueError('unterminated signature name')
        try:
            semantic = data[name:end].decode('ascii')
        except UnicodeDecodeError as error:
            raise ValueError('non-ASCII signature name') from error
        result.append({'stream': stream, 'semantic': semantic, 'index': index,
                       'system_value': system, 'component_type': component,
                       'register': register, 'mask': masks & 255,
                       'read_write_mask': (masks >> 8) & 255, 'minimum_precision': precision})
    return result


def psv_resources(data):
    """Standard PSV runtime/resources (LLVM DXContainer.h v2/v3 layouts)."""
    runtime_size = words(data, 0, 1)[0]
    if runtime_size not in (48, 52):
        raise ValueError('unsupported PSV runtime size')
    bounded(data, 4, runtime_size)
    count = words(data, 4 + runtime_size, 1)[0]
    if count > 4096:
        raise ValueError('PSV resource count outside supported limit')
    bindings = []
    if count:
        stride = words(data, 8 + runtime_size, 1)[0]
        if stride != 24:
            raise ValueError('unsupported PSV resource binding stride')
        start = 12 + runtime_size
        words(data, start, count * 6)
        for i in range(count):
            typ, space, lower, upper, kind, flags = words(data, start + i * 24, 6)
            if lower > upper:
                raise ValueError('invalid PSV resource register range')
            bindings.append({'type': typ, 'space': space, 'lower': lower, 'upper': upper,
                             'kind': kind, 'flags': flags})
    return {'shader_kind': data[28], 'numthreads': list(words(data, 40, 3)), 'resources': bindings}


def fssm(data, expected_hash):
    if data[:4] != b'FSSM' or words(data, 4, 3) != (0x101, len(data) - 16, 0):
        raise ValueError('unsupported FSSM header')
    bounded(data, 0xc0, 16)
    offset, size = struct.unpack_from('<2Q', data, 0xc0)
    start = offset + 16
    if start < 0x108 or start % 4 or size % 4 or size < 24:
        raise ValueError('unsupported FSSM code range')
    code = bounded(data, start, size)
    prefix = nop_hash(code, third=False)
    if prefix['hash_hex'] != expected_hash:
        raise ValueError('FSSM NOP hash differs from outer HASH')
    stages = []
    for pos in range(0x100, start - 7, 0x88):
        token = struct.unpack_from('<Q', data, pos)[0]
        if token & ~0xffff != 0x000000c000020000:
            break
        stages.append({'offset': pos, 'hardware_stage_raw': token & 255,
                       'api_stage_raw': (token >> 8) & 255,
                       'descriptor_hex_opaque': bounded(data, pos, min(0x88, start - pos)).hex()})
    if not stages:
        raise ValueError('no observed FSSM hardware-stage descriptor')
    return {'size': len(data), 'version': '0x101', 'code_offset': start, 'code_size': size,
            'code_sha256': hashlib.sha256(code).hexdigest(), 'prefix': prefix,
            'hardware_stages': stages}, code


def inspect_shader(blob):
    parts = dxbc_parts(blob)
    dxil = one(parts, 'DXIL')
    version, units, magic, dxil_version, bc_offset, bc_size = words(dxil, 0, 6)
    if units * 4 != len(dxil) or magic != int.from_bytes(b'DXIL', 'little'):
        raise ValueError('invalid DXIL program header')
    bitcode = bounded(dxil, 8 + bc_offset, bc_size)
    if bitcode != b'NOBC':
        raise ValueError('expected native NOBC program, not LLVM bitcode')
    if dxil[24:28] != b'XDAT':
        raise ValueError('expected XDAT immediately after program header')
    count, table = words(dxil, 28, 2)
    if not 1 <= count <= 64 or table < 36:
        raise ValueError('unsupported XDAT table')
    words(dxil, table, count * 3)
    hash_part = one(parts, 'HASH')
    if len(hash_part) != 20 or words(hash_part, 0, 1) != (0,):
        raise ValueError('unsupported outer HASH')
    shader_hash = hash_part[4:].hex()
    spans, entries, payloads, variants = [], [], [], []
    for i in range(count):
        typ, offset, size = words(dxil, table + i * 12, 3)
        if offset < table + count * 12 or not size:
            raise ValueError('XDAT entry overlaps table or is empty')
        data = bounded(dxil, offset, size)
        end = offset + size
        if any(offset < b and a < end for a, b in spans):
            raise ValueError('overlapping XDAT entries')
        spans.append((offset, end))
        entries.append({'type': hex(typ), 'offset': offset, 'size': size,
                        'sha256': hashlib.sha256(data).hexdigest()})
        if typ == 0x0d:
            if data[:4] != b'XBCH' or words(data, 4, 1) != (3,):
                raise ValueError('unsupported XBCH version')
            compressed, zero, expanded, zero2 = words(data, 8, 4)
            if zero or zero2 or compressed + 24 != len(data):
                raise ValueError('XBCH stream size/reserved mismatch')
            native = inflate(data[24:], expanded, wbits=15)
            info, code = fssm(native, shader_hash)
            info.update(xdat_entry=i, xbch_version=3)
            variants.append(info)
            payloads.append({'fssm': native, 'code': code})
    if not variants:
        raise ValueError('no native XBCH variants')
    kind = version >> 16
    if kind >= len(KINDS):
        raise ValueError('unrecognized API shader kind')
    result = {'format': 'DXBC/DXIL(NOBC)/XDAT/XBCH3/FSSM101', 'stage': KINDS[kind],
              'shader_model': f'{(version >> 4) & 15}.{version & 15}',
              'dxil_version': hex(dxil_version), 'hash': shader_hash,
              'container_sha256': hashlib.sha256(blob).hexdigest(),
              'parts': [{k: v for k, v in p.items() if k != 'data'} for p in parts],
              'input_signature': signature(one(parts, 'ISG1')),
              'output_signature': signature(one(parts, 'OSG1')),
              'psv': psv_resources(one(parts, 'PSV0')),
              'xdat_entries': entries, 'native_variants': variants,
              'limitations': ['FSSM metadata/binding ABI and hardware-stage descriptors remain partly opaque.',
                              'No root binding map is inferred from DXBC names alone.',
                              'Container validation does not implement ISA semantics or a native PSO.']}
    return result, payloads
