"""Straight-line RDNA2 (gfx1030) VS/PS -> HLSL lowering for game graphics shaders.

Scope: the small UI/blit programs that the title runs first (no branches, no LDS,
no derivatives). Unsupported instructions/ABI raise ValueError -- nothing is guessed.

PC-side ABI produced here (the runtime in d3d12_xs implements the same contract):
  * descriptors are never loaded from memory by the shader. Every `s_load_*` from the
    user-data table pointer s[8:9] (+ constant offset) is recorded as "table slot at byte
    offset N" and later uses (s_buffer_load / buffer_load / image_sample) decide its kind.
  * buffer slot k  -> ByteAddressBuffer  Buf<k> : register(t<k>, space1)
  * texture  slot k -> Texture2D<float4> Tex<k> : register(t<k>, space1)
  * immediate sampler words (S# built with s_mov) -> SamplerState Smp<j> : register(s<j>, space1)
  * V# stride / num_records (needed for idxen buffer_load): cbuffer VSharp : register(b0, space2) { uint4 vs[K]; }
    vs[k] = (stride, num_records, 0, 0) for buffer slot k, filled per draw by the runtime.
  * VS input element i (ISG1 order) occupies VGPR 4+4*i .. 4+4*i+3 (fetch-shader output);
    VS export paramN <- N-th non-system output element; PS attrN <- N-th non-system input element.
The result carries a `bindings` manifest consumed by the runtime.
"""
import re
import struct

from xsx_shader_bindings import operands
from xsx_shader_isa import LLVMDisassembler
from xsx_shader_pack import inspect_shader

HELPERS = '''
float mul_legacy(float a, float b) { return (a == 0.0 || b == 0.0) ? 0.0 : a * b; }
float med3f(float a, float b, float c) { return max(min(a, b), min(max(a, b), c)); }
float2 unpack_h2(uint p) { return float2(f16tof32(p & 0xffffu), f16tof32(p >> 16)); }
uint pack_rtz(float a, float b) { return f32tof16(a) | (f32tof16(b) << 16); }
'''


def code_instructions(code):
    ends = [i for i in range(24, len(code), 4) if code[i:i + 4] == bytes.fromhex('000081bf')]
    if len(ends) != 1:
        raise ValueError('need exactly one s_endpgm (no multi-exit programs supported)')
    end = ends[0] + 4
    with LLVMDisassembler('gfx1030', 32) as d:
        ins = d.decode(code, 24, end)
    if any(code[i:i + 4] != bytes.fromhex('00009fbf') for i in range(end, len(code), 4)):
        raise ValueError('unknown non-padding data after program')
    return ins


def lit(tok):
    tok = tok.strip().split()[0]
    named = {'null': 0, '0': 0, '1.0': 0x3f800000, '0.5': 0x3f000000, '2.0': 0x40000000, '4.0': 0x40800000,
             '-1.0': 0xbf800000, '-0.5': 0xbf000000, '-2.0': 0xc0000000, '-4.0': 0xc0800000}
    if tok in named:
        return named[tok]
    if re.fullmatch(r'-?\d+\.\d+(e[+-]?\d+)?', tok):
        return struct.unpack('<I', struct.pack('<f', float(tok)))[0]
    return int(tok, 0) & 0xffffffff


def reg_list(tok):
    m = re.fullmatch(r'([sv])(\d+)|([sv])\[(\d+):(\d+)\]', tok.strip())
    if not m:
        return None
    if m[1]:
        return m[1], [int(m[2])]
    return m[3], list(range(int(m[4]), int(m[5]) + 1))


class Translator:
    def __init__(self, inspection, code):
        self.ins, self.stage = inspection, inspection['stage']
        self.instructions = code_instructions(code)
        self.s, self.v = {}, {}
        self.lines, self.n = [], 0
        self.slots = {}        # byte offset -> {'dwords', 'kind'}
        self.samplers = []     # list of 4-word tuples
        self.pending_interp = {}
        self.exports = {}
        self.sgpr_const = {}
        sig_in = [e for e in inspection['input_signature'] if e['system_value'] == 0]
        sig_out = [e for e in inspection['output_signature'] if e['system_value'] == 0]
        self.sig_in, self.sig_out = sig_in, sig_out
        if self.stage == 'VS':
            for i, e in enumerate(sig_in):
                for k in range(4):
                    if e['mask'] >> k & 1:
                        self.v[4 + 4 * i + k] = f'asuint(inp.i{i}.{"xyzw"[k]})'

    def tmp(self, expr):
        name = f't{self.n}'
        self.n += 1
        self.lines.append(f'    uint {name} = {expr};')
        return name

    # ---- operand helpers -------------------------------------------------------------
    def src(self, tok):
        tok = tok.strip()
        neg = abs_ = False
        m = re.fullmatch(r'-(.+)', tok)
        if m and not re.fullmatch(r'-?\d+(\.\d+)?', tok):
            neg, tok = True, m[1]
        m = re.fullmatch(r'\|(.+)\|', tok)
        if m:
            abs_, tok = True, m[1]
        regs = reg_list(tok)
        if regs:
            kind, idx = regs
            if len(idx) != 1:
                raise ValueError(f'multi-register operand {tok} used as scalar')
            table = self.v if kind == 'v' else self.s
            if idx[0] not in table:
                raise ValueError(f'read of undefined {kind}{idx[0]}')
            value = table[idx[0]]
            if isinstance(value, tuple):
                raise ValueError(f'descriptor word {value} used as arithmetic value')
        elif tok in ('vcc_lo', 'exec_lo'):
            raise ValueError(f'unsupported special operand {tok}')
        else:
            value = f'0x{lit(tok):08x}u'
        if abs_:
            value = f'asuint(abs(asfloat({value})))'
        if neg:
            value = f'asuint(-asfloat({value}))'
        return value

    def f(self, tok):
        return f'asfloat({self.src(tok)})'

    def setv(self, tok, expr):
        kind, idx = reg_list(tok)
        if kind != 'v' or len(idx) != 1:
            raise ValueError(f'bad vector destination {tok}')
        self.v[idx[0]] = self.tmp(expr)

    # ---- translation -----------------------------------------------------------------
    def run(self):
        for ins in self.instructions:
            op, a = ins['mnemonic'], operands(ins['asm'])
            handler = getattr(self, 'op_' + op.removesuffix('_e32').removesuffix('_e64'), None)
            if handler is None:
                raise ValueError(f'unsupported instruction at {ins["offset"]:#x}: {ins["asm"]}')
            handler(a, ins)
        return self.emit()

    def ignore(self, a, ins):
        pass
    op_s_inst_prefetch = op_s_waitcnt = op_s_nop = op_s_endpgm = op_s_wqm_b64 = op_s_and_b64 = ignore
    op_s_swappc_b64 = ignore   # fetch-shader call: VS inputs are modelled directly (see module docstring)

    def op_s_mov_b32(self, a, ins):
        if a[0].strip() == 'm0':
            return
        kind, idx = reg_list(a[0])
        if reg_list(a[1]):
            self.s[idx[0]] = self.src(a[1]) if reg_list(a[1])[1][0] in self.s else None
            return
        self.s[idx[0]] = f'0x{lit(a[1]):08x}u'
        self.sgpr_const[idx[0]] = lit(a[1])

    def op_s_movk_i32(self, a, ins):
        value = lit(a[1])
        if value & 0x8000:
            value |= 0xffff0000
        _, idx = reg_list(a[0])
        self.s[idx[0]] = f'0x{value & 0xffffffff:08x}u'
        self.sgpr_const[idx[0]] = value & 0xffffffff

    def op_s_mov_b64(self, a, ins):
        if a[1].strip() != 'exec':
            raise ValueError('only s_mov_b64 ..., exec (WQM bookkeeping) is supported')

    def op_s_load_dwordx4(self, a, ins):
        self.table_load(a, 4)

    def op_s_load_dwordx8(self, a, ins):
        self.table_load(a, 8)

    def table_load(self, a, count):
        _, dst = reg_list(a[0])
        base = a[1].strip()
        if base != 's[8:9]':
            raise ValueError(f'descriptor load from unsupported base {base}')
        off = 0 if a[2].strip() == 'null' else lit(a[2])
        if off in self.slots and self.slots[off]['dwords'] != count:
            raise ValueError('same table slot loaded with different widths')
        self.slots.setdefault(off, {'dwords': count, 'kind': None})
        for i, r in enumerate(dst):
            self.s[r] = ('desc', off, i)

    def desc_slot(self, tok, count):
        kind, regs = reg_list(tok)
        words = [self.s.get(r) for r in regs]
        if len(regs) != count or not all(isinstance(w, tuple) and w[0] == 'desc' for w in words) \
                or len({w[1] for w in words}) != 1 or [w[2] for w in words] != list(range(count)):
            raise ValueError(f'{tok} is not a whole loaded descriptor')
        return words[0][1]

    def set_kind(self, off, kind):
        cur = self.slots[off]['kind']
        if cur not in (None, kind):
            raise ValueError(f'table slot {off:#x} used as both {cur} and {kind}')
        self.slots[off]['kind'] = kind

    def buf_id(self, off):
        self.set_kind(off, 'buffer')
        order = sorted(o for o, s in self.slots.items() if s['kind'] == 'buffer')
        return order.index(off)

    def op_s_buffer_load_dwordx4(self, a, ins):
        self.sbuffer(a, 4)

    def op_s_buffer_load_dwordx16(self, a, ins):
        self.sbuffer(a, 16)

    def sbuffer(self, a, count):
        _, dst = reg_list(a[0])
        off = self.desc_slot(a[1], 4)
        self.set_kind(off, 'buffer')
        byte = 0 if a[2].strip() == 'null' else lit(a[2])
        if byte % 4:
            raise ValueError('unaligned s_buffer_load offset')
        for i, r in enumerate(dst):
            self.s[r] = ('buf', off, byte + 4 * i)

    def sval(self, tok):
        regs = reg_list(tok)
        if regs and regs[0] == 's':
            w = self.s.get(regs[1][0])
            if isinstance(w, tuple) and w[0] == 'buf':
                return self.tmp_sload(w)
        return self.src(tok)

    def tmp_sload(self, w):
        key = ('sload',) + w
        if key not in self.__dict__.setdefault('_cache', {}):
            self.set_kind(w[1], 'buffer'); self._cache[key] = self.tmp(f'Buf@B{w[1]}@.Load({w[2]}u)')
        return self._cache[key]

    def bufname(self, off):
        return f'{self.buf_id(off)}'

    def op_v_mov_b32(self, a, ins):
        self.setv(a[0], self.sval(a[1]))

    def fsrc(self, tok):
        return f'asfloat({self.sval(tok)})'

    def op_v_fma_f32(self, a, ins):
        self.setv(a[0], f'asuint(mad({self.fsrc(a[1])}, {self.fsrc(a[2])}, {self.fsrc(a[3])}))')

    def op_v_fmac_f32(self, a, ins):
        self.setv(a[0], f'asuint(mad({self.fsrc(a[1])}, {self.fsrc(a[2])}, {self.fsrc(a[0])}))')

    def op_v_mul_legacy_f32(self, a, ins):
        self.setv(a[0], f'asuint(mul_legacy({self.fsrc(a[1])}, {self.fsrc(a[2])}))')

    def op_v_add_f32(self, a, ins):
        self.setv(a[0], f'asuint({self.fsrc(a[1])} + {self.fsrc(a[2])})')

    def op_v_cvt_i32_f32(self, a, ins):
        self.setv(a[0], f'asuint((int)clamp({self.fsrc(a[1])}, -2147483648.0, 2147483520.0))')

    def op_v_med3_f32(self, a, ins):
        self.setv(a[0], f'asuint(med3f({self.fsrc(a[1])}, {self.fsrc(a[2])}, {self.fsrc(a[3])}))')

    def op_v_cvt_pkrtz_f16_f32(self, a, ins):
        self.setv(a[0], f'pack_rtz({self.fsrc(a[1])}, {self.fsrc(a[2])})')

    def attr(self, tok):
        m = re.fullmatch(r'attr(\d+)\.([xyzw])', tok.strip())
        if not m or self.stage != 'PS':
            raise ValueError(f'unsupported interpolant {tok}')
        n = int(m[1])
        if n >= len(self.sig_in):
            raise ValueError('interpolant beyond input signature')
        return n, m[2]

    def op_v_interp_p1_f32(self, a, ins):
        n, c = self.attr(a[2])
        _, d = reg_list(a[0])
        self.pending_interp[d[0]] = (n, c)

    def op_v_interp_p2_f32(self, a, ins):
        n, c = self.attr(a[2])
        _, d = reg_list(a[0])
        if self.pending_interp.pop(d[0], None) != (n, c):
            raise ValueError('v_interp_p2 without matching p1 (different attribute/destination)')
        self.setv(a[0], f'asuint(inp.i{n}.{c})')

    def op_buffer_load_dwordx4(self, a, ins):
        # buffer_load_dwordx4 v[d:d+3], vindex, s[v#], soffset idxen
        _, dst = reg_list(a[0])
        off = self.desc_slot(a[2], 4)
        if 'idxen' not in ins['asm'] or a[3].split()[0] != '0' or 'offen' in ins['asm'] or 'offset:' in ins['asm']:
            raise ValueError(f'unsupported buffer_load addressing: {ins["asm"]}')
        self.set_kind(off, 'buffer')
        index = self.sval(a[1])
        for i, r in enumerate(dst):
            self.v[r] = self.tmp(f'Buf@B{off}@.Load(({index}) * vs[@B{off}@].x + {4 * i}u)')

    def op_image_sample(self, a, ins):
        if 'dmask:0xf' not in ins['asm'] or 'SQ_RSRC_IMG_2D' not in ins['asm'] or any(x in ins['asm'] for x in ('lwe', 'tfe', 'a16', 'd16')):
            raise ValueError(f'unsupported image_sample form: {ins["asm"]}')
        _, dst = reg_list(a[0])
        _, coords = reg_list(a[1])
        if len(dst) != 4 or len(coords) != 2:
            raise ValueError('only 4-component 2D sample with 2 coordinates is supported')
        off = self.desc_slot(a[2], 8)
        self.set_kind(off, 'texture2d')
        words = []
        _, sregs = reg_list(a[3].split()[0])
        for r in sregs:
            if r not in self.sgpr_const:
                raise ValueError('sampler S# is not built from constants')
            words.append(self.sgpr_const[r])
        if words not in self.samplers:
            self.samplers.append(words)
        j = self.samplers.index(words)
        u, w = (f'asfloat({self.sval(f"v{c}")})' for c in coords)
        name = self.tmp_vec(f'Tex@T{off}@.Sample(Smp{j}, float2({u}, {w}))')
        for i, r in enumerate(dst):
            self.v[r] = f'asuint({name}.{"xyzw"[i]})'

    def tmp_vec(self, expr):
        name = f'q{self.n}'
        self.n += 1
        self.lines.append(f'    float4 {name} = {expr};')
        return name

    def op_exp(self, a, ins):
        target = a[0].strip()
        toks = [t.split()[0] for t in a[1:5]]
        mods = ' '.join(a[1:]).split()
        compr = 'compr' in ins['asm'].split(',')[-1] or ' compr' in ins['asm']
        vals = []
        if compr:
            lo, hi = self.src(toks[0]), self.src(toks[2])
            vals = [f'unpack_h2({lo}).x', f'unpack_h2({lo}).y', f'unpack_h2({hi}).x', f'unpack_h2({hi}).y']
        else:
            for t in toks:
                vals.append('0.0' if t == 'off' else f'asfloat({self.sval(t)})')
        self.exports[target] = vals

    def emit(self):
        out = []
        out.append('// Generated from checked native RDNA instructions; no name/hash replacement.')
        out.append(f'// Original GPU program hash: {self.ins["hash"]}')
        out.append(HELPERS)
        bufs = sorted(o for o, s in self.slots.items() if s['kind'] == 'buffer')
        texs = sorted(o for o, s in self.slots.items() if s['kind'] == 'texture2d')
        unknown = [hex(o) for o, s in self.slots.items() if s['kind'] is None]
        if unknown:
            raise ValueError(f'descriptor slots loaded but never used: {unknown}')
        for k, _ in enumerate(bufs):
            out.append(f'ByteAddressBuffer Buf{k} : register(t{k}, space1);')
        for k, _ in enumerate(texs):
            out.append(f'Texture2D<float4> Tex{k} : register(t{k}, space2);')
        for j, _ in enumerate(self.samplers):
            out.append(f'SamplerState Smp{j} : register(s{j}, space1);')
        out.append(f'cbuffer VSharp : register(b0, space2) {{ uint4 vs[{max(1, len(bufs))}]; }}')
        if self.stage == 'VS':
            out.append('struct In { ' + ' '.join(f'float4 i{i} : {e["semantic"].upper()}{e["index"]};' for i, e in enumerate(self.sig_in)) + ' };')
            out.append('struct Out { float4 position : SV_Position; ' + ' '.join(f'float4 o{i} : {e["semantic"].upper()}{e["index"]};' for i, e in enumerate(self.sig_out)) + ' };')
            out.append('Out main(In inp) {')
            out.append('    Out output;')
            out += self.lines
            pos = self.exports.get('pos0')
            if not pos:
                raise ValueError('VS has no pos0 export')
            out.append(f'    output.position = float4({", ".join(pos)});')
            for i in range(len(self.sig_out)):
                p = self.exports.get(f'param{i}')
                out.append(f'    output.o{i} = float4({", ".join(p)});' if p else f'    output.o{i} = 0;')
            extra = set(self.exports) - {'pos0'} - {f'param{i}' for i in range(len(self.sig_out))}
            if extra:
                raise ValueError(f'exports beyond the output signature: {sorted(extra)}')
            out.append('    return output;')
        elif self.stage == 'PS':
            out.append('struct In { float4 position : SV_Position; ' + ' '.join(f'float4 i{i} : {e["semantic"].upper()}{e["index"]};' for i, e in enumerate(self.sig_in)) + ' };')
            out.append('float4 main(In inp) : SV_Target0 {')
            out += self.lines
            mrt = self.exports.get('mrt0')
            if not mrt or set(self.exports) != {'mrt0'}:
                raise ValueError('PS must export exactly mrt0')
            out.append(f'    return float4({", ".join(mrt)});')
        else:
            raise ValueError(f'unsupported stage {self.stage}')
        out.append('}')
        bindings = {'stage': self.stage,
                    'buffers': [{'slot_byte_offset': o, 'dwords': self.slots[o]['dwords'], 'register': k, 'space': 1} for k, o in enumerate(bufs)],
                    'textures': [{'slot_byte_offset': o, 'dwords': self.slots[o]['dwords'], 'register': k, 'space': 2} for k, o in enumerate(texs)],
                    'samplers': [{'words': w, 'register': j, 'space': 1} for j, w in enumerate(self.samplers)],
                    'vsharp_cbuffer': {'register': 0, 'space': 2, 'count': max(1, len(bufs))},
                    'vs_inputs': [{'semantic': e['semantic'], 'index': e['index'], 'mask': e['mask'], 'first_vgpr': 4 + 4 * i} for i, e in enumerate(self.sig_in)] if self.stage == 'VS' else [],
                    'hash': self.ins['hash']}
        text = '\n'.join(out) + '\n'
        text = re.sub(r'@B(\d+)@', lambda m: str(bufs.index(int(m[1]))), text)
        text = re.sub(r'@T(\d+)@', lambda m: str(texs.index(int(m[1]))), text)
        return {'hlsl': text, 'bindings': bindings, 'instructions': self.instructions}


def translate_graphics(blob):
    inspection, payloads = inspect_shader(blob)
    if inspection['stage'] not in ('VS', 'PS') or len(payloads) != 1:
        raise ValueError('need one native VS or PS variant')
    return Translator(inspection, payloads[0]['code']).run()
