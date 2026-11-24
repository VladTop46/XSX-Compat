#!/usr/bin/env python3
"""Container corruption checks and the first native VS lowering contract."""
import copy
import json
from pathlib import Path
import random
import runpy
import struct
import subprocess
import tempfile
import unittest
from unittest.mock import patch
import zlib

from xsx_shader_bindings import operands
from xsx_shader_pack import (bounded, dxbc_parts, fssm, inflate, inflate_record,
                             inspect_shader, one, records)
from xsx_shader_translate import translate_compute_reset, translate_vertex, vertex_program

REPO = Path(__file__).resolve().parents[2]
REPORT = REPO / 'compat/reports/native-shaders-20261124'


def change(data, offset, value, fmt='<I'):
    result = bytearray(data)
    struct.pack_into(fmt, result, offset, value)
    return bytes(result)


def record(name, raw=b'DXBC fixture'):
    name = name.encode('ascii')
    compressor = zlib.compressobj(wbits=-15)
    compressed = compressor.compress(raw) + compressor.flush()
    payload = compressed + bytes.fromhex('0102030405060708090a0b0c')
    return (bytes([len(name) + 1]) + name + b'\x00\x32' +
            struct.pack('<4I', len(payload), 0xddce7, len(payload), len(raw)) + payload)


class RecordTests(unittest.TestCase):
    def test_realistic_stream_and_opaque_trailer(self):
        blob = b'9FPR' + record('Alpha:VS_Fullscreen')
        found = list(records(blob))
        self.assertEqual(len(found), 1)
        raw, trailer = inflate_record(blob, found[0])
        self.assertEqual(raw, b'DXBC fixture')
        self.assertEqual(trailer.hex(), '0102030405060708090a0b0c')

    def test_ascii_length_byte_and_effect_qualifier(self):
        names = ['VS_' + 'x' * 62, 'Autofocus:' + 'CS_' + 'a' * 47 + '_Wrapped']
        # 65-char name has ASCII B as its length byte. A suffix of the second
        # name also has a plausible length byte ':'; preserve the full name.
        blob = b'9FPR' + b''.join(record(n) for n in names)
        self.assertEqual([r['name'] for r in records(blob)], names)

    def test_nested_record_bytes_do_not_create_aliases(self):
        nested = record('PS_FakeInner')
        outer = record('CS_Outer', nested)
        blob = b'9FPR' + outer + record('VS_Next')
        self.assertEqual([r['name'] for r in records(blob)], ['CS_Outer', 'VS_Next'])

    def test_all_record_truncations_are_bounded(self):
        blob = b'9FPR' + record('CS_Truncated')
        for end in range(4, len(blob)):
            self.assertEqual(list(records(blob[:end])), [])
        self.assertEqual(len(list(records(blob))), 1)

    def test_decompression_size_eof_and_bomb_limit(self):
        data = zlib.compress(b'x' * 4096)
        self.assertEqual(inflate(data, 4096, wbits=15), b'x' * 4096)
        for payload, size in ((data, 10), (data, 4097), (data[:-1], 4096),
                              (data + b'junk', 4096), (data, 65 << 20)):
            with self.subTest(size=size), self.assertRaises(ValueError):
                inflate(payload, size, wbits=15)


class ContainerTests(unittest.TestCase):
    def test_cli_census_and_exact_record_selection(self):
        api = runpy.run_path(str(REPO / 'compat/tools/inspect-game-shaders.py'))
        shader = (REPORT / 'VS.dxbc').read_bytes()
        with tempfile.TemporaryDirectory(prefix='xsx-pack-') as directory:
            root = Path(directory)
            pack = root / 'sample.rpf'
            pack.write_bytes(b'9FPR' + record('VS_Fixture', shader))
            result = api['census'](pack, root / 'census')
            self.assertEqual(result['counts']['validated_native_containers'], 1)
            self.assertEqual(result['errors'], {})
            selected = api['extract_record'](pack, 4, root / 'selected')
            self.assertEqual(selected['stage'], 'VS')
            self.assertEqual((root / 'selected/shader.dxbc').read_bytes(), shader)
            with self.assertRaises(ValueError):
                api['extract_record'](pack, 5, root / 'wrong')

    def test_real_cs_vs_ps_containers(self):
        for stage in ('CS', 'VS', 'PS'):
            blob = (REPORT / (stage + '.dxbc')).read_bytes()
            result, payloads = inspect_shader(blob)
            self.assertEqual(result['stage'], stage)
            self.assertEqual(len(payloads), 1)
            self.assertEqual(result['native_variants'][0]['prefix']['size'], 24)
            self.assertEqual(result['native_variants'][0]['prefix']['hash_hex'], result['hash'])
            self.assertIn(b'NOBC', one(dxbc_parts(blob), 'DXIL'))

    def test_dxbc_bounds_and_overlap(self):
        blob = (REPORT / 'VS.dxbc').read_bytes()
        offset = struct.unpack_from('<I', blob, 32)[0]
        for data in (blob[:-1], change(blob, 24, len(blob) + 4),
                     change(blob, 32, 16), change(blob, 36, offset),
                     change(blob, offset + 4, 0xffffffff)):
            with self.assertRaises(ValueError):
                dxbc_parts(data)

    def test_xdat_bounds_and_overlapping_entries(self):
        blob = (REPORT / 'VS.dxbc').read_bytes()
        base = next(p['offset'] for p in dxbc_parts(blob) if p['tag'] == 'DXIL')
        for offset, value in ((28, 0xffffffff), (32, 0xfffffffc),
                              (40, 0), (44, 0xffffffff), (52, 0x48)):
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                inspect_shader(change(blob, base + offset, value))

    def test_xbch_corruption(self):
        blob = (REPORT / 'VS.dxbc').read_bytes()
        base = next(p['offset'] for p in dxbc_parts(blob) if p['tag'] == 'DXIL') + 0x58
        for offset, value in ((4, 4), (8, 1), (12, 1), (16, 65 << 20), (24, 0)):
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                inspect_shader(change(blob, base + offset, value))

    def test_fssm_code_pointer_and_hash(self):
        data = (REPORT / 'VS.fssm').read_bytes()
        result, _ = inspect_shader((REPORT / 'VS.dxbc').read_bytes())
        digest = result['hash']
        for data2 in (data[:-4], change(data, 4, 0x102), change(data, 0xc0, 0xfffffffffffffff0, '<Q'),
                      change(data, 0xc8, 0xfffffffffffffff0, '<Q'), change(data, 0x308 + 8, 0)):
            with self.assertRaises(ValueError):
                fssm(data2, digest)


class TranslationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.blob = (REPORT / 'VS.dxbc').read_bytes()
        cls.inspection, cls.instructions = vertex_program(cls.blob)

    def test_native_vs_matches_independent_fullscreen_formula(self):
        def tofloat(x): return struct.unpack('<f', struct.pack('<I', x))[0]
        def tobits(x): return struct.unpack('<I', struct.pack('<f', x))[0]
        randomizer = random.Random(0x26503)
        for vertex in [0, 1, 2, 3, 0xffffffff, 0x80000000] + [randomizer.getrandbits(32) for _ in range(1024)]:
            regs, exported = {'v0': vertex}, {}
            def value(token):
                if token in regs: return regs[token]
                return tobits(float(token)) if '.' in token else int(token, 0) & 0xffffffff
            for ins in self.instructions:
                op, args = ins['mnemonic'], operands(ins['asm'])
                if op in ('s_inst_prefetch', 's_waitcnt', 's_endpgm'): continue
                if op == 'exp':
                    exported[args[0]] = [tofloat(value(a.split()[0])) for a in args[1:]]
                    continue
                src = [value(x) for x in args[1:]]
                if op == 'v_lshlrev_b32_e32': result = (src[1] << (src[0] & 31)) & 0xffffffff
                elif op == 'v_and_b32_e32': result = src[0] & src[1]
                elif op == 'v_mov_b32_e32': result = src[0]
                elif op == 'v_cvt_f32_ubyte0_e32': result = tobits(float(src[0] & 255))
                elif op == 'v_fma_f32': result = tobits(tofloat(src[0]) * tofloat(src[1]) + tofloat(src[2]))
                else: self.fail(op)
                regs[args[0]] = result
            u, v = float(vertex & 2), float((vertex << 1) & 2)
            self.assertEqual(exported['pos0'], [2 * u - 1, 1 - 2 * v, 1, 1])
            self.assertEqual(exported['param0'], [u, v, 0, 1])

    def test_reject_compute_and_pixel_inputs(self):
        for stage in ('CS', 'PS'):
            with self.assertRaises(ValueError):
                translate_vertex((REPORT / (stage + '.dxbc')).read_bytes())

    def test_unsupported_opcode_and_uninitialized_register_fail_closed(self):
        for asm in ('s_barrier', 'v_mov_b32_e32 v1, v255', 's_branch 0'):
            instructions = copy.deepcopy(self.instructions)
            instructions[1].update(asm=asm, mnemonic=asm.split()[0])
            with patch('xsx_shader_translate.vertex_program', return_value=(self.inspection, instructions)):
                with self.subTest(asm=asm), self.assertRaises(ValueError):
                    translate_vertex(self.blob)

    def test_generated_shader_compiles_to_actual_llvm_bitcode(self):
        result = translate_vertex(self.blob)
        self.assertEqual(len(result['instructions']), 14)
        with tempfile.TemporaryDirectory(prefix='xsx-native-vs-') as directory:
            source, output = Path(directory) / 'vs.hlsl', Path(directory) / 'vs.dxil'
            source.write_text(result['hlsl'])
            compiled = subprocess.run([str(REPO / 'sdk/dxc/bin/dxc'), '-T', 'vs_6_0', '-E', 'main',
                                       '-Ges', '-Fo', str(output), str(source)], capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            dxil = one(dxbc_parts(output.read_bytes()), 'DXIL')
            offset, size = struct.unpack_from('<2I', dxil, 16)
            self.assertEqual(bounded(dxil, offset + 8, size)[:4], b'BC\xc0\xde')


class ComputeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.blob = (REPORT / 'compute-reset/shader.dxbc').read_bytes()

    def test_raw_uav_binding_comes_from_psv(self):
        result = translate_compute_reset(self.blob)
        self.assertEqual(len(result['instructions']), 10)
        self.assertEqual(result['store_dwords'], 4)
        self.assertEqual(result['resource'], {'type': 7, 'kind': 11, 'flags': 0, 'space': 1, 'lower': 8, 'upper': 8})
        self.assertIn('register(u8, space1)', result['hlsl'])
        self.assertIn('Store4(0u, uint4(0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u))', result['hlsl'])

    def test_reject_unsupported_compute_abi(self):
        base = next(p['offset'] for p in dxbc_parts(self.blob) if p['tag'] == 'PSV0')
        for offset, value in ((28, 1), (40, 2), (56, 2), (60, 16), (64, 6), (76, 9), (80, 2)):
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                translate_compute_reset(change(self.blob, base + offset, value))

    def test_constant_values_are_read_from_native_instructions(self):
        import xsx_shader_translate
        instructions = copy.deepcopy(translate_compute_reset(self.blob)['instructions'])
        ins = next(i for i in instructions if i['asm'] == 'v_mov_b32_e32 v2, 0')
        ins['asm'] = 'v_mov_b32_e32 v2, 17'
        with patch.object(xsx_shader_translate, 'straight_line', return_value=instructions):
            result = translate_compute_reset(self.blob)
        self.assertIn('uint4(0x00000000u, 0x00000000u, 0x00000011u, 0x00000000u)', result['hlsl'])

    def test_uninitialized_store_rejected(self):
        import xsx_shader_translate
        instructions = copy.deepcopy(translate_compute_reset(self.blob)['instructions'])
        instructions = [i for i in instructions if i['asm'] != 'v_mov_b32_e32 v2, 0']
        with patch.object(xsx_shader_translate, 'straight_line', return_value=instructions):
            with self.assertRaisesRegex(ValueError, 'uninitialized'):
                translate_compute_reset(self.blob)

    def test_compute_dxil_compiles_and_preserves_binding(self):
        result = translate_compute_reset(self.blob)
        with tempfile.TemporaryDirectory(prefix='xsx-native-cs-') as directory:
            source, output = Path(directory) / 'cs.hlsl', Path(directory) / 'cs.dxil'
            source.write_text(result['hlsl'])
            compiled = subprocess.run([str(REPO / 'sdk/dxc/bin/dxc'), '-T', 'cs_6_0', '-E', 'main',
                                       '-Ges', '-Fo', str(output), str(source)], capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            from xsx_shader_pack import psv_resources
            psv = psv_resources(one(dxbc_parts(output.read_bytes()), 'PSV0'))
            self.assertEqual(psv['resources'][0]['space'], 1)
            self.assertEqual((psv['resources'][0]['lower'], psv['resources'][0]['upper']), (8, 8))


if __name__ == '__main__':
    unittest.main(verbosity=2)
