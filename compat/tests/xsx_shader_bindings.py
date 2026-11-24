"""Conservative scalar-register provenance for the decoded compute shader.

This links SMEM descriptor/CBV reads to MIMG operations under the inferred
metadata SGPR input map. It does not evaluate shader arithmetic. A control-flow
join loses provenance when values differ; no guessed register value is used.
"""
from collections import Counter, deque
import re


def operands(asm):
    text = asm.split(None, 1)[1] if ' ' in asm or '\t' in asm else ''
    result, start, depth = [], 0, 0
    for i, char in enumerate(text):
        if char == '[':
            depth += 1
        elif char == ']':
            depth -= 1
        elif char == ',' and not depth:
            result.append(text[start:i].strip())
            start = i + 1
    if text:
        result.append(text[start:].strip())
    return result


def registers(operand):
    match = re.fullmatch(r's(\d+)|s\[(\d+):(\d+)\]', operand)
    if not match:
        return []
    lo = int(match[1] or match[2])
    hi = lo if match[1] else int(match[3])
    if not 0 <= lo <= hi < 128:
        raise ValueError('SGPR index outside supported analysis state')
    return list(range(lo, hi + 1))


def immediate(operand):
    token = operand.split()[0]
    if token == 'null':
        return 0
    try:
        return int(token, 0)
    except ValueError:
        return None


def read(state, operand, width):
    regs = registers(operand)
    if regs:
        return tuple(state[r] for r in regs) if len(regs) == width else (None,) * width
    value = immediate(operand)
    if value is None:
        return (None,) * width
    return tuple((value >> (i * 32)) & 0xffffffff for i in range(width))


def descriptor_source(state, operand, bindings):
    regs = registers(operand)
    if not regs:
        return None
    first = state[regs[0]]
    if not isinstance(first, tuple) or first[0] != 'resource' or first[2] != 0:
        return None
    if any(state[r] != ('resource', first[1], i) for i, r in enumerate(regs)):
        return None
    return bindings[first[1]]


def cbv_source(state, operand, bindings):
    regs = registers(operand)
    if len(regs) != 4:
        return None
    lo, hi = state[regs[0]], state[regs[1]]
    if not isinstance(lo, tuple) or lo[0] != 'root_address' or lo[2] != 0:
        return None
    if hi != ('root_address', lo[1], 1):
        return None
    matches = [b for b in bindings if b['type'] == 'CBV' and
               b['source'].get('root_parameter') == lo[1]]
    return matches[0] if len(matches) == 1 else None


def table_load(state, args, bindings):
    address = read(state, args[1], 2)
    offset = immediate(args[2])
    if not isinstance(address[0], tuple) or address[0][0] != 'table_base' or (
            address[1] != 0 or offset is None or offset % 32):
        return None
    root, slot = address[0][1], offset // 32
    matches = [(i, b) for i, b in enumerate(bindings) if
               b['source'].get('kind') == 'descriptor_table' and
               b['source']['root_parameter'] == root and b['source']['slot'] == slot]
    return matches[0] if len(matches) == 1 else None


def transfer(state, ins, bindings):
    args, mnemonic = operands(ins['asm']), ins['mnemonic']
    if not args or mnemonic.startswith(('s_cmp', 's_cbranch', 's_wait', 's_store')):
        return state
    dest = registers(args[0])
    if not dest:
        return state
    value = (None,) * len(dest)
    if mnemonic in ('s_mov_b32', 's_mov_b64'):
        value = read(state, args[1], len(dest))
    elif mnemonic == 's_movk_i32' and len(dest) == 1:
        val = immediate(args[1])
        if val is not None:
            val &= 0xffff
            value = (((val if val < 0x8000 else val - 0x10000) & 0xffffffff),)
    elif mnemonic == 's_bfm_b32' and len(dest) == 1:
        width, shift = read(state, args[1], 1)[0], read(state, args[2], 1)[0]
        if isinstance(width, int) and isinstance(shift, int):
            value = ((((1 << (width & 31)) - 1) << (shift & 31)) & 0xffffffff,)
    elif mnemonic == 's_cselect_b64':
        left, right = read(state, args[1], len(dest)), read(state, args[2], len(dest))
        value = tuple(a if a == b else None for a, b in zip(left, right))
    elif mnemonic == 's_load_dwordx8':
        source = table_load(state, args, bindings)
        if source:
            index, binding = source
            value = tuple(('resource', index, i) if i < binding['location_dwords'] else None
                          for i in range(len(dest)))
    result = list(state)
    for reg, val in zip(dest, value):
        result[reg] = val
    return tuple(result)


def analyze_bindings(shader):
    isa, bindings = shader['isa'], shader['matched_bindings']
    if isa['control_flow']['unmodeled_control_flow']:
        raise ValueError('provenance needs modeled control flow')
    instructions = isa['instructions']
    index = {ins['offset']: i for i, ins in enumerate(instructions)}
    branch = {edge['source']: edge for edge in isa['control_flow']['branches']}
    initial = [None] * 128
    for binding in bindings:
        source, first = binding['source'], binding['location_register']
        if source['kind'] == 'static_sampler':
            continue
        if first + binding['location_dwords'] > 128:
            raise ValueError('input SGPR range is out of bounds')
        root = source['root_parameter']
        values = [('table_base', root)] if source['kind'] == 'descriptor_table' else [
            ('root_address', root, i) for i in range(2)]
        for reg, value in enumerate(values, first):
            if initial[reg] is not None and initial[reg] != value:
                raise ValueError('conflicting input SGPR map')
            initial[reg] = value
    states = [None] * len(instructions)
    states[0] = tuple(initial)
    queue = deque([0])
    queued = {0}
    steps = 0
    while queue:
        i = queue.popleft()
        queued.remove(i)
        steps += 1
        ins = instructions[i]
        state = transfer(states[i], ins, bindings)
        targets = []
        edge = branch.get(ins['offset'])
        if edge:
            targets.append(index[edge['target']])
        if ins['mnemonic'] != 's_endpgm' and (not edge or edge['conditional']):
            if i + 1 >= len(instructions):
                raise ValueError('control flow falls off instruction region')
            targets.append(i + 1)
        for target in targets:
            old = states[target]
            new = state if old is None else tuple(a if a == b else None for a, b in zip(old, state))
            if new != old:
                states[target] = new
                if target not in queued:
                    queue.append(target)
                    queued.add(target)
    images, reads, loads, unresolved = [], [], [], []
    samplers = [b for b in bindings if b['type'] == 'sampler']
    for ins, state in zip(instructions, states):
        if state is None:
            continue
        mnemonic, args = ins['mnemonic'], operands(ins['asm'])
        if mnemonic == 's_load_dwordx8':
            source = table_load(state, args, bindings)
            loads.append({'offset': ins['offset'], 'binding_record': source[1]['record_offset']
                          if source else None})
        elif mnemonic.startswith('s_buffer_load_'):
            source = cbv_source(state, args[1], bindings)
            reads.append({'offset': ins['offset'], 'CBV_register': source['register'] if source else None,
                          'byte_offset': immediate(args[2]), 'dwords': len(registers(args[0]))})
        elif mnemonic.startswith('image_'):
            ranges = re.findall(r's\[\d+:\d+\]', ins['asm'])
            source = descriptor_source(state, ranges[0], bindings) if ranges else None
            entry = {'offset': ins['offset'], 'opcode': mnemonic,
                     'binding_record': source['record_offset'] if source else None,
                     'type': source['type'] if source else None,
                     'register': source['register'] if source else None}
            if len(ranges) > 1:
                descriptor = [state[r] for r in registers(ranges[1])]
                entry['sampler_descriptor_words'] = descriptor if all(
                    isinstance(word, int) for word in descriptor) else None
                entry['sampler_register_candidate'] = samplers[0]['register'] if len(samplers) == 1 else None
                entry['sampler_evidence'] = 'sole sampler in metadata; raw descriptor not format-decoded'
            images.append(entry)
            if source is None:
                unresolved.append(ins['offset'])
    return {'fixpoint_steps': steps, 'reachable_instructions': sum(s is not None for s in states),
            'image_operations': images, 'unresolved_image_offsets': unresolved,
            'image_binding_counts': dict(Counter(f'{i["type"]}:{i["register"]}' for i in images)),
            'descriptor_loads': loads, 'cbv_reads': reads,
            'assumptions': ['Metadata location registers are the initial SGPR binding map.',
                            'Table pointers are 32-bit and descriptor slots have 32-byte stride.',
                            'Two root-address words identify CBV buffer reads; format words are not evaluated.'],
            'limitations': ['Arithmetic values, EXEC masks, sampler format and GPU memory behavior are not evaluated.',
                            'This is conservative static provenance, not a shader translator.']}
