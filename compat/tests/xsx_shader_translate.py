"""Strict first native VS lowering: straight-line VertexID arithmetic + exports.

This deliberately rejects unsupported instructions, bindings and control flow.
It is not a general RDNA translator. Inputs come from validated native blobs,
never from shader names or hash-based substitution with reference shaders.
"""
import re
import struct

from xsx_shader_bindings import operands
from xsx_shader_isa import LLVMDisassembler
from xsx_shader_pack import dxbc_parts, inspect_shader, one, psv_resources


def straight_line(code):
    ends = [i for i in range(24, len(code), 4) if code[i:i + 4] == bytes.fromhex('000081bf')]
    if len(ends) != 1:
        raise ValueError('lowering needs one straight-line program ending')
    end = ends[0] + 4
    with LLVMDisassembler('gfx1030', 32) as d32, LLVMDisassembler('gfx1030', 64) as d64:
        instructions = d32.decode(code, 24, end)
        other = d64.decode(code, 24, end)
    if instructions[-1]['mnemonic'] != 's_endpgm':
        raise ValueError('candidate ending is not an instruction s_endpgm')
    if instructions != other:
        raise ValueError('wave-dependent decoding is unsupported by this lowering')
    if any(code[i:i + 4] != bytes.fromhex('00009fbf') for i in range(end, len(code), 4)):
        raise ValueError('unknown non-padding data after vertex program')
    return instructions


def vertex_program(blob):
    inspection, payloads = inspect_shader(blob)
    if inspection['stage'] != 'VS' or len(payloads) != 1:
        raise ValueError('lowering needs a single native VS variant')
    inputs, outputs = inspection['input_signature'], inspection['output_signature']
    if len(inputs) != 1 or (inputs[0]['semantic'].upper(), inputs[0]['system_value'],
            inputs[0]['component_type'], inputs[0]['register'], inputs[0]['mask']) != ('SV_VERTEXID', 6, 1, 0, 1):
        raise ValueError('unsupported vertex input ABI')
    if len(outputs) != 2 or [(s['semantic'].upper(), s['index'], s['system_value'],
            s['component_type'], s['register'], s['mask']) for s in outputs] != [
            ('SV_POSITION', 0, 1, 3, 0, 15), ('TEXCOORD', 0, 0, 3, 1, 3)]:
        raise ValueError('unsupported vertex output ABI')
    return inspection, straight_line(payloads[0]['code'])


def literal(token):
    if re.fullmatch(r'-?\d+\.\d+', token):
        return struct.unpack('<I', struct.pack('<f', float(token)))[0]
    try:
        return int(token, 0) & 0xffffffff
    except ValueError as error:
        raise ValueError(f'unsupported native source operand {token}') from error


def translate_vertex(blob):
    inspection, instructions = vertex_program(blob)
    values = {'v0': 'vertexID'}
    lines, exports, definitions = [], set(), 0

    def source(token):
        if token in values:
            return values[token]
        if re.fullmatch(r'v\d+', token):
            raise ValueError(f'read of uninitialized native VGPR {token}')
        return f'0x{literal(token):08x}u'

    for ins in instructions:
        mnemonic, args = ins['mnemonic'], operands(ins['asm'])
        if mnemonic == 's_inst_prefetch':
            continue  # Cache hint, no register/memory result.
        if mnemonic == 's_waitcnt' and ins['asm'] == 's_waitcnt expcnt(0)':
            continue  # Export ordering; output values were captured at each exp.
        if mnemonic == 's_endpgm':
            continue
        if mnemonic == 'exp':
            target, raw = args[0], args[1:]
            if len(raw) != 4 or target not in ('pos0', 'param0') or target in exports:
                raise ValueError('unsupported/duplicate vertex export')
            tokens = [a.split() for a in raw]
            if any(len(a) != 1 for a in tokens[:3]) or tokens[3][1:] not in ([], ['done']):
                raise ValueError('unsupported export modifiers')
            vals = [source(t[0]) for t in tokens]
            if target == 'pos0':
                lines.append(f'    output.position = asfloat(uint4({", ".join(vals)}));')
            else:
                lines.append(f'    output.uv = asfloat(uint2({", ".join(vals[:2])}));')
            exports.add(target)
            continue
        supported = {'v_lshlrev_b32_e32': 3, 'v_and_b32_e32': 3, 'v_mov_b32_e32': 2,
                     'v_cvt_f32_ubyte0_e32': 2, 'v_fma_f32': 4}
        if mnemonic not in supported or len(args) != supported[mnemonic] or not re.fullmatch(r'v\d+', args[0]):
            raise ValueError(f'unsupported native instruction at {ins["offset"]:#x}: {ins["asm"]}')
        src = [source(t) for t in args[1:]]
        if mnemonic == 'v_lshlrev_b32_e32':
            expr = f'({src[1]} << ({src[0]} & 31u))'
        elif mnemonic == 'v_and_b32_e32':
            expr = f'({src[0]} & {src[1]})'
        elif mnemonic == 'v_mov_b32_e32':
            expr = src[0]
        elif mnemonic == 'v_cvt_f32_ubyte0_e32':
            expr = f'asuint((float)({src[0]} & 255u))'
        else:
            expr = f'asuint(mad(asfloat({src[0]}), asfloat({src[1]}), asfloat({src[2]})))'
        name = f'r{definitions}'
        definitions += 1
        lines.append(f'    uint {name} = {expr}; // native +0x{ins["offset"]:x}')
        values[args[0]] = name
    if exports != {'pos0', 'param0'}:
        raise ValueError('missing vertex exports')
    hlsl = '\n'.join([
        '// Generated from checked native RDNA instructions; no name/hash replacement.',
        f'// Original GPU program hash: {inspection["hash"]}',
        'struct Output { float4 position : SV_Position; float2 uv : TEXCOORD0; };',
        'Output main(uint vertexID : SV_VertexID) {', '    Output output;',
        *lines, '    return output;', '}', ''])
    return {'hlsl': hlsl, 'source_container_sha256': inspection['container_sha256'],
            'source_program_hash': inspection['hash'], 'instructions': instructions,
            'assumptions': ['VS v0 supplies SV_VertexID; checked signature has no other inputs.',
                            'No EXEC/VCC/scalar ALU, resource accesses or branches are supported.'],
            'limitations': ['Only the explicitly accepted instruction/output subset is lowered.',
                            'HLSL mad contraction and original Xbox output are not generally validated.',
                            'The selected fullscreen arithmetic uses exact small integer-valued floats.']}


def translate_compute_reset(blob):
    inspection, payloads = inspect_shader(blob)
    if inspection['stage'] != 'CS' or len(payloads) != 1 or inspection['input_signature'] or inspection['output_signature']:
        raise ValueError('lowering needs one compute program without IO signatures')
    psv = psv_resources(one(dxbc_parts(blob), 'PSV0'))
    if psv['shader_kind'] != 5 or psv['numthreads'] != [1, 1, 1] or len(psv['resources']) != 1:
        raise ValueError('compute lowering needs numthreads(1,1,1) and one resource')
    resource = psv['resources'][0]
    if (resource['type'], resource['kind'], resource['flags']) != (7, 11, 0) or resource['lower'] != resource['upper']:
        raise ValueError('compute lowering needs one non-array raw UAV')
    instructions = straight_line(payloads[0]['code'])
    sgpr, vgpr, loads, stores, lines = {'s4': 'table_base'}, {}, 0, 0, []
    for ins in instructions:
        op, args = ins['mnemonic'], operands(ins['asm'])
        if op in ('s_inst_prefetch', 's_endpgm') or (op == 's_waitcnt' and args == ['lgkmcnt(0)']):
            continue
        if op == 's_mov_b32' and len(args) == 2 and args[0] == 's5' and literal(args[1]) == 0:
            sgpr['s5'] = 0
        elif op == 's_load_dwordx4' and args == ['s[0:3]', 's[4:5]', 'null'] and sgpr.get('s5') == 0:
            loads += 1
            for i in range(4): sgpr[f's{i}'] = ('raw_uav', i)
        elif op == 'v_mov_b32_e32' and len(args) == 2 and re.fullmatch(r'v\d+', args[0]):
            vgpr[args[0]] = f'0x{literal(args[1]):08x}u'
        elif op in ('buffer_store_dword', 'buffer_store_dwordx4') and len(args) == 4:
            if args[1:] != ['off', 's[0:3]', '0'] or any(sgpr.get(f's{i}') != ('raw_uav', i) for i in range(4)):
                raise ValueError('unsupported buffer store address/descriptor')
            if op == 'buffer_store_dword':
                regs = [args[0]]
            else:
                match = re.fullmatch(r'v\[(\d+):(\d+)\]', args[0])
                if not match or int(match[2]) - int(match[1]) != 3:
                    raise ValueError('unsupported buffer store VGPR range')
                regs = [f'v{i}' for i in range(int(match[1]), int(match[2]) + 1)]
            if any(reg not in vgpr for reg in regs):
                raise ValueError('read of uninitialized buffer store VGPR')
            values = [vgpr[reg] for reg in regs]
            lines.append(f'    destination.Store{4 if len(regs) == 4 else ""}(0u, ' +
                         (f'uint4({", ".join(values)})' if len(regs) == 4 else values[0]) + ');')
            stores += 1
        else:
            raise ValueError(f'unsupported compute instruction at {ins["offset"]:#x}: {ins["asm"]}')
    if loads != 1 or stores != 1:
        raise ValueError('compute lowering needs exactly one descriptor load and one store')
    hlsl = '\n'.join([
        '// Lowered native buffer store; register/space taken from PSV0.',
        f'RWByteAddressBuffer destination : register(u{resource["lower"]}, space{resource["space"]});',
        '[numthreads(1,1,1)]', 'void main() {', *lines, '}', ''])
    return {'hlsl': hlsl, 'source_container_sha256': inspection['container_sha256'],
            'source_program_hash': inspection['hash'], 'instructions': instructions,
            'resource': resource, 'store_dwords': 4 if 'Store4' in hlsl else 1,
            'assumptions': ['Initial s4 identifies the descriptor table containing the sole PSV raw UAV.',
                            'Only the validated one-thread, one-descriptor, constant-store subset is lowered.'],
            'limitations': ['Original Xbox descriptor contents/output were not captured.',
                            'Dynamic indexing, other bindings, synchronization and arithmetic are rejected.']}
