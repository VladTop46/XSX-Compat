"""Read the observed Xbox FXCP shader metadata and standard RTS0 root signature.

This is a bounded, read-only frontend for this DLL's 0x403 metadata dialect.
Token meanings are inferred by cross-checking its bindings against RTS0; they
are not an Xbox API declaration. Unknown layouts fail rather than being guessed.
"""
import struct


def words(data, offset, count):
    if offset < 0 or offset % 4 or count < 0 or offset + count * 4 > len(data):
        raise ValueError(f'word range outside blob: offset={offset:#x}, count={count}')
    return struct.unpack_from('<' + 'I' * count, data, offset)


def root_signature(dxbc):
    if len(dxbc) < 32 or dxbc[:4] != b'DXBC':
        raise ValueError('not a DXBC root signature')
    size, count = words(dxbc, 24, 2)
    if size != len(dxbc):
        raise ValueError('DXBC size mismatch')
    offsets = words(dxbc, 32, count)
    chunks, spans = [], []
    for offset in offsets:
        if offset < 32 + count * 4:
            raise ValueError('DXBC chunk overlaps header')
        tag, length = words(dxbc, offset, 2)
        end = offset + 8 + length
        if end > len(dxbc) or any(offset < b and a < end for a, b in spans):
            raise ValueError('DXBC chunk out of range or overlapping')
        spans.append((offset, end))
        if tag == int.from_bytes(b'RTS0', 'little'):
            chunks.append(dxbc[offset + 8:end])
    if len(chunks) != 1:
        raise ValueError('expected one RTS0 chunk')
    data = chunks[0]
    version, nparams, params, nsamplers, samplers, flags = words(data, 0, 6)
    if version not in (1, 2):
        raise ValueError(f'unsupported root signature version {version}')
    words(data, params, nparams * 3)
    words(data, samplers, nsamplers * 13)
    types = {0: 'table', 1: 'constants', 2: 'CBV', 3: 'SRV', 4: 'UAV'}
    range_types = {0: 'SRV', 1: 'UAV', 2: 'CBV', 3: 'sampler'}
    parameters = []
    for i in range(nparams):
        kind, visibility, offset = words(data, params + i * 12, 3)
        if kind not in types:
            raise ValueError(f'unknown root parameter type {kind}')
        parameter = {'index': i, 'type': types[kind], 'visibility': visibility}
        if kind == 0:
            nranges, ranges = words(data, offset, 2)
            stride = 6 if version == 2 else 5
            words(data, ranges, nranges * stride)
            parameter['ranges'] = []
            next_offset = 0
            for j in range(nranges):
                fields = words(data, ranges + j * stride * 4, stride)
                typ, num, reg, space = fields[:4]
                if typ not in range_types or not num:
                    raise ValueError('invalid descriptor range type/count')
                range_flags = fields[4] if version == 2 else 0
                table_offset = fields[-1]
                if table_offset == 0xffffffff:
                    if next_offset is None:
                        raise ValueError('APPEND after unbounded range')
                    table_offset = next_offset
                next_offset = table_offset + num if num != 0xffffffff else None
                parameter['ranges'].append({'type': range_types[typ], 'count': num,
                    'register': reg, 'space': space, 'flags': range_flags,
                    'table_offset': table_offset})
        elif kind == 1:
            reg, space, num = words(data, offset, 3)
            parameter.update(register=reg, space=space, count=num)
        else:
            fields = words(data, offset, 3 if version == 2 else 2)
            parameter.update(register=fields[0], space=fields[1],
                             flags=fields[2] if version == 2 else 0)
        parameters.append(parameter)
    static = []
    for i in range(nsamplers):
        fields = words(data, samplers + i * 52, 13)
        static.append({'index': i, 'filter': fields[0], 'address_u': fields[1],
                       'address_v': fields[2], 'address_w': fields[3],
                       'register': fields[10], 'space': fields[11],
                       'visibility': fields[12], 'raw_words': list(fields)})
    return {'version': version, 'flags': flags, 'parameters': parameters,
            'static_samplers': static}


def nop_hash(code, *, third=True):
    """Two VOP3 v_nop-with-literal encodings carry 128 bits; a third is fixed.

    Reconstruct the fields, including the byte split across SRC0 and SRC1.
    The opcode is corroborated by assembling v_nop_e64 with LLVM for gfx1030.
    These NOPs remain in the original program; no executable bytes are patched.
    """
    w = words(code, 0, 9 if third else 6)
    result = []
    for index in (0, 3):
        first, second, literal = w[index:index + 3]
        if first & 0xffff0000 != 0xd5800000 or (
                second & ~(0xff | (0xff << 9))) != 0x03fe0100:
            raise ValueError('unrecognized hash NOP encoding')
        result.extend([(first & 0xffff) | ((second & 0xff) << 16) |
                       (((second >> 9) & 0xff) << 24), literal])
    if third and w[6:] != (0xd5800000, 0x03fe0100, 0xac0):
        raise ValueError('unrecognized third prefix NOP')
    return {'hash_words': result, 'hash_hex': struct.pack('<4I', *result).hex(),
            'size': 36 if third else 24, 'third_nop_literal': '0xac0' if third else None}


def shader_metadata(data, instruction_size):
    if len(data) < 56 or (len(data) - 24) % 32:
        raise ValueError('metadata has an incomplete header/record array')
    header = words(data, 0, 6)
    count = (len(data) - 24) // 32
    if header[:5] != (0x403, len(data) - 20, instruction_size // 4, 0, 0) or instruction_size % 4:
        raise ValueError('unsupported metadata header or instruction length mismatch')
    if header[5] != (count << 15) | 0x440:
        raise ValueError('record array token/count mismatch')
    types = {0x300: 'SRV', 0x401: 'sampler', 0x102: 'CBV', 0x303: 'UAV'}
    bindings, opaque = [], []
    tags, hash_words = [], None
    for offset in range(24, len(data), 32):
        record = words(data, offset, 8)
        tag = record[0]
        tags.append(tag)
        if tag == 0x43:
            if record[5:] != (0, 0, 0):
                raise ValueError('unknown hash record suffix')
            hash_words = list(record[1:5])
        else:
            if record[6:] != (0xffffffff, 0):
                raise ValueError('unknown binding/opaque record suffix')
            if tag == 0x41:
                if record[1] not in types:
                    raise ValueError(f'unknown resource token {record[1]:#x}')
                bindings.append({'record_offset': offset, 'type': types[record[1]],
                    'token': hex(record[1]), 'descriptor_offset_dwords': record[2],
                    'register': record[3], 'dimension': record[4],
                    'location_index': record[5] >> 16,
                    'location_register': (record[5] >> 8) & 0xff,
                    'location_dwords': record[5] & 0xff, 'raw_words': list(record)})
            elif tag == 0x42:
                opaque.append({'record_offset': offset, 'raw_words': list(record)})
            else:
                raise ValueError(f'unknown metadata record {tag:#x}')
    if tags != sorted(tags) or tags.count(0x43) != 1 or tags[-1] != 0x43:
        raise ValueError('unexpected record ordering/hash count')
    return {'dialect': 'observed 0x403', 'record_count': count, 'bindings': bindings,
            'opaque_0x42': opaque, 'hash_words': hash_words,
            'hash_hex': struct.pack('<4I', *hash_words).hex()}


def match_bindings(root, metadata):
    """Cross-check inferred location indices, registers and offsets against RTS0.

    The 8-DWORD table stride is observed here, not the host descriptor increment.
    Static sampler location_register=0 is not evidence of a live SGPR s0 input.
    """
    matched = []
    for original in metadata['bindings']:
        binding = dict(original)
        typ, index = binding['type'], binding['location_index']
        if typ == 'sampler':
            candidates = root['static_samplers']
            if index >= len(candidates) or candidates[index]['register'] != binding['register']:
                raise ValueError('sampler metadata does not match RTS0')
            if binding['location_register'] or binding['location_dwords'] != 4:
                raise ValueError('unrecognized static sampler location')
            binding['source'] = {'kind': 'static_sampler', 'index': index,
                                 'space': candidates[index]['space']}
        else:
            if index >= len(root['parameters']):
                raise ValueError('metadata root index outside RTS0 parameters')
            parameter = root['parameters'][index]
            if parameter['type'] == 'table':
                candidates = [r for r in parameter['ranges'] if r['type'] == typ and
                              r['register'] <= binding['register'] < r['register'] + r['count']]
                if len(candidates) != 1:
                    raise ValueError('binding does not match a unique root descriptor range')
                r = candidates[0]
                slot = r['table_offset'] + binding['register'] - r['register']
                if binding['descriptor_offset_dwords'] != slot * 8:
                    raise ValueError('metadata table offset differs from 8-DWORD descriptor stride')
                if binding['dimension'] not in (1, 4) or binding['location_dwords'] != (
                        4 if binding['dimension'] == 1 else 8):
                    raise ValueError('unknown buffer/Texture2D descriptor width')
                binding['source'] = {'kind': 'descriptor_table', 'root_parameter': index,
                    'space': r['space'], 'slot': slot,
                    'heap_pointer_sgpr_candidate': binding['location_register'],
                    'pointer_dwords_candidate': 1}
            elif parameter['type'] == typ:
                if parameter['register'] != binding['register'] or binding['descriptor_offset_dwords']:
                    raise ValueError('root descriptor register/offset mismatch')
                if binding['location_dwords'] != 2:
                    raise ValueError('root address width is not two DWORDs')
                binding['source'] = {'kind': 'root_descriptor', 'root_parameter': index,
                    'space': parameter['space'], 'address_sgpr_candidate': binding['location_register'],
                    'address_dwords_candidate': 2}
            else:
                raise ValueError('metadata/root parameter type mismatch')
        matched.append(binding)
    return matched


def inspect_sections(root, code, meta):
    signature = root_signature(root)
    metadata = shader_metadata(meta, len(code))
    prefix = nop_hash(code)
    if prefix['hash_words'] != metadata['hash_words']:
        raise ValueError('NOP prefix hash differs from metadata 0x43 record')
    bindings = match_bindings(signature, metadata)
    return {'root_signature': signature, 'metadata': metadata, 'prefix': prefix,
            'matched_bindings': bindings,
            'limitations': ['0x42 record semantics and packet layout remain opaque.',
                            'SGPR interpretation is inferred; RTS0/offset/hash relationships are checked.',
                            'No pipeline creation, shader execution or translation is implemented.']}
