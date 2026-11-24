"""Address-preserving inspection of FXCP code with the local LLVM C API.

The hash-carrying NOP prefix is recognized separately. Bytes after the last
aligned s_endpgm are preserved as an opaque tail, not decoded as instructions.
This boundary is a candidate until the control-flow checks below succeed; it
does not prove the absence of embedded data or of a second entry point.
"""
from collections import Counter
import ctypes
import ctypes.util
import hashlib
import struct

from xsx_shader_metadata import nop_hash


class LLVMDisassembler:
    def __init__(self, cpu='gfx1030', wave=32, library=None):
        if cpu not in ('gfx1010', 'gfx1013', 'gfx1030') or wave not in (32, 64):
            raise ValueError('unsupported analysis CPU/wave size')
        library = library or ctypes.util.find_library('LLVM')
        if not library:
            raise ValueError('LLVM shared library not found')
        self.lib = ctypes.CDLL(library)
        self.library, self.cpu, self.wave = library, cpu, wave
        for name in ('LLVMInitializeAMDGPUTargetInfo', 'LLVMInitializeAMDGPUTarget',
                     'LLVMInitializeAMDGPUTargetMC', 'LLVMInitializeAMDGPUDisassembler'):
            fn = getattr(self.lib, name)
            fn.argtypes, fn.restype = [], None
            fn()
        create = self.lib.LLVMCreateDisasmCPUFeatures
        create.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p,
                           ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p]
        create.restype = ctypes.c_void_p
        other_wave = 64 if wave == 32 else 32
        features = f'+wavefrontsize{wave},-wavefrontsize{other_wave}'
        self.context = create(b'amdgcn--amdpal', cpu.encode(), features.encode(), None, 0, None, None)
        if not self.context:
            raise ValueError('LLVM could not create AMDGPU disassembler')
        self.lib.LLVMSetDisasmOptions.argtypes = [ctypes.c_void_p, ctypes.c_uint64]
        self.lib.LLVMSetDisasmOptions.restype = ctypes.c_int
        self.lib.LLVMSetDisasmOptions(self.context, 2)  # PrintImmHex.
        decode = self.lib.LLVMDisasmInstruction
        decode.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint8), ctypes.c_uint64,
                           ctypes.c_uint64, ctypes.c_char_p, ctypes.c_size_t]
        decode.restype = ctypes.c_size_t
        self.lib.LLVMDisasmDispose.argtypes = [ctypes.c_void_p]
        self.lib.LLVMDisasmDispose.restype = None

    def close(self):
        if self.context:
            self.lib.LLVMDisasmDispose(self.context)
            self.context = None

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()

    def decode(self, code, start, end):
        if not self.context or not 0 <= start < end <= len(code):
            raise ValueError('invalid decoder context/range')
        buffer = (ctypes.c_uint8 * len(code)).from_buffer_copy(code)
        output = ctypes.create_string_buffer(2048)
        result, offset = [], start
        while offset < end:
            size = self.lib.LLVMDisasmInstruction(self.context,
                ctypes.cast(ctypes.byref(buffer, offset), ctypes.POINTER(ctypes.c_uint8)),
                end - offset, offset, output, len(output))
            if not size or size % 4 or offset + size > end:
                # No resynchronization: unknown instruction lengths would make
                # the following valid-looking instructions unreliable.
                raise ValueError(f'{self.cpu}: undecodable instruction at {offset:#x}: '
                                 f'{code[offset:offset + 12].hex(" ")}')
            asm = output.value.decode('ascii').strip()
            if not asm:
                raise ValueError(f'empty disassembly at {offset:#x}')
            result.append({'offset': offset, 'size': size,
                           'bytes': code[offset:offset + size].hex(), 'asm': asm,
                           'mnemonic': asm.split()[0]})
            offset += size
        return result


def control_flow(instructions, start, end):
    if not instructions:
        raise ValueError('empty instruction region')
    branches = {'s_branch', 's_cbranch_scc0', 's_cbranch_scc1', 's_cbranch_vccz',
                's_cbranch_vccnz', 's_cbranch_execz', 's_cbranch_execnz'}
    boundaries = {instruction['offset'] for instruction in instructions}
    edges, unknown = [], []
    for ins in instructions:
        mnemonic = ins['mnemonic']
        if mnemonic in branches:
            word = int.from_bytes(bytes.fromhex(ins['bytes'])[:4], 'little')
            if ins['size'] != 4 or word & 0xff800000 != 0xbf800000:
                raise ValueError('branch is not an expected SOPP encoding')
            displacement = struct.unpack('<h', struct.pack('<H', word & 0xffff))[0]
            target = ins['offset'] + 4 + displacement * 4
            if target not in boundaries:
                raise ValueError(f'branch {ins["offset"]:#x} targets non-instruction {target:#x}')
            edges.append({'source': ins['offset'], 'target': target,
                          'conditional': mnemonic != 's_branch'})
        elif mnemonic.startswith('s_cbranch') or mnemonic in (
                's_setpc_b64', 's_swappc_b64', 's_call_b64', 's_rfe_b64',
                's_trap', 's_sethalt', 's_sendmsghalt', 's_endpgm_saved',
                's_endpgm_ordered_ps_done'):
            unknown.append({'offset': ins['offset'], 'asm': ins['asm']})
    if instructions[-1]['mnemonic'] != 's_endpgm':
        raise ValueError('candidate region does not end on an instruction s_endpgm')
    return {'start': start, 'end': end, 'branch_targets_checked': len(edges),
            'branches': edges, 'unmodeled_control_flow': unknown,
            'limitations': 'Linear decoding and direct branch boundaries do not prove shader semantics.'}


def inspect_isa(code, decoder):
    if len(code) % 4:
        raise ValueError('instruction buffer is not DWORD aligned')
    prefix = nop_hash(code)
    ends = [i for i in range(prefix['size'], len(code) - 3, 4)
            if code[i:i + 4] == bytes.fromhex('00 00 81 bf')]
    if not ends:
        raise ValueError('no aligned s_endpgm after prefix')
    end = ends[-1] + 4
    instructions = decoder.decode(code, prefix['size'], end)
    flow = control_flow(instructions, prefix['size'], end)
    tail = code[end:]
    nonpadding = [{'offset': i, 'word': hex(struct.unpack_from('<I', code, i)[0])}
                  for i in range(end, len(code), 4)
                  if code[i:i + 4] != bytes.fromhex('00 00 9f bf')]
    return {'decoder': {'library': decoder.library, 'cpu': decoder.cpu,
                       'wave_size_assumed_for_disassembly': decoder.wave},
            'prefix': prefix, 'body_start': prefix['size'], 'body_end_candidate': end,
            'aligned_endpgm_patterns': ends, 'instructions': instructions,
            'opcode_counts': dict(sorted(Counter(i['mnemonic'] for i in instructions).items())),
            'control_flow': flow,
            'opaque_tail': {'offset': end, 'size': len(tail), 'hex': tail.hex(),
                            'sha256': hashlib.sha256(tail).hexdigest(),
                            'non_s_code_end_words': nonpadding},
            'limitations': ['Wave size comes from the analysis option, not decoded pipeline registers.',
                            'Region end is inferred from s_endpgm patterns; tail is preserved.',
                            'No ISA translation or shader execution is performed.']}


def listing(inspection):
    lines = [f'; {inspection["decoder"]}',
             '; Original bytes preserved; recognized prefix is three 12-byte VOP3 NOPs.',
             f'; Embedded hash: {inspection["prefix"]["hash_hex"]}']
    for i in range(3):
        lines.append(f'{i * 12:08x}: <metadata v_nop_e64 + literal>')
    for ins in inspection['instructions']:
        lines.append(f'{ins["offset"]:08x}: {ins["bytes"]:<24} {ins["asm"]}')
    tail = inspection['opaque_tail']
    lines.append(f'; Opaque tail at {tail["offset"]:#x}, {tail["size"]} bytes; see JSON for exact bytes.')
    return '\n'.join(lines) + '\n'
