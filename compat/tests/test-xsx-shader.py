#!/usr/bin/env python3
"""CPU-only checks of native FXCP metadata, ISA bounds and resource provenance."""
from collections import Counter
import copy
from pathlib import Path
import runpy
import struct
import subprocess
import unittest

from xsx_shader_metadata import (inspect_sections, match_bindings, nop_hash,
                                 root_signature, shader_metadata, words)
from xsx_shader_isa import LLVMDisassembler, control_flow, inspect_isa
from xsx_shader_bindings import analyze_bindings, transfer


HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
FRONTEND = runpy.run_path(str(HERE / 'inspect-xsx-shader.py'))
DLL = REPO / 'tests/03-fidelityfx/amd_fidelityfx_upscaler_xbox.dll'


def overwrite(data, offset, value):
    result = bytearray(data)
    struct.pack_into('<I', result, offset, value)
    return bytes(result)


class Fixture(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        pe = FRONTEND['fxcp']['PE'](DLL)
        cls.blob = next(blob for rva, blob in FRONTEND['containers'](pe) if rva == 0xa2f320)
        sections = FRONTEND['fxcp']['parse_fxcp'](cls.blob)
        cls.sections = {s['name']: cls.blob[s['offset']:s['offset'] + s['size']] for s in sections}
        cls.root = cls.sections['root-signature.dxbc']
        cls.meta = cls.sections['shader-meta.bin']
        cls.code = cls.sections['instructions.bin']
        cls.shader, _ = FRONTEND['inspect_container'](cls.blob)


class MetadataTests(Fixture):
    def test_runtime_selected_root(self):
        root = root_signature(self.root)
        self.assertEqual(root['version'], 2)
        self.assertEqual([p['type'] for p in root['parameters']], ['table', 'table', 'CBV', 'CBV'])
        self.assertEqual([p['ranges'][0]['count'] for p in root['parameters'][:2]], [58, 58])
        self.assertEqual([s['filter'] for s in root['static_samplers']], [0, 0x15])
        self.assertEqual([s['register'] for s in root['static_samplers']], [0, 1])

    def test_resource_identity_and_slot_offsets(self):
        bindings = self.shader['matched_bindings']
        self.assertEqual(Counter(b['type'] for b in bindings), {'sampler': 1, 'SRV': 1, 'UAV': 4, 'CBV': 2})
        tables = [b for b in bindings if b['source']['kind'] == 'descriptor_table']
        self.assertCountEqual([(b['type'], b['register'], b['source']['slot']) for b in tables],
                         [('SRV', 0, 0), ('UAV', 0, 0), ('UAV', 1, 1), ('UAV', 2, 2), ('UAV', 3, 3)])
        self.assertEqual({b['location_register'] for b in tables if b['type'] == 'UAV'}, {4})

    def test_prefix_hash(self):
        self.assertEqual(nop_hash(self.code)['hash_hex'], 'c02758a23cb201faca9881c0b56a6669')
        self.assertEqual(nop_hash(self.code)['hash_words'], self.shader['metadata']['hash_words'])

    def test_reject_damaged_prefix(self):
        for offset, value in ((0, 0), (4, 0), (24, 0), (32, 0)):
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                nop_hash(overwrite(self.code, offset, value))
        with self.assertRaises(ValueError):
            nop_hash(self.code[:32])

    def test_reject_changed_hash_even_with_valid_nops(self):
        changed = overwrite(self.code, 8, words(self.code, 8, 1)[0] ^ 1)
        nop_hash(changed)  # Still a recognized encoding, but no longer the same shader.
        with self.assertRaisesRegex(ValueError, 'hash differs'):
            inspect_sections(self.root, changed, self.meta)

    def test_reject_damaged_metadata(self):
        mutations = [(0, 0x404), (4, 0), (8, 0), (20, 0x440), (24, 0x44),
                     (28, 0xffff), (24 + 6 * 4, 0), (len(self.meta) - 12, 1)]
        for offset, value in mutations:
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                shader_metadata(overwrite(self.meta, offset, value), len(self.code))
        for data in (b'', self.meta[:20], self.meta[:-1], self.meta[:-32]):
            with self.subTest(length=len(data)), self.assertRaises(ValueError):
                shader_metadata(data, len(self.code))

    def test_reject_root_bounds_and_types(self):
        rts = words(self.root, 32, 1)[0] + 8
        params = words(self.root, rts + 8, 1)[0]
        for offset, value in ((24, 0), (32, 0), (rts, 3),
                              (rts + 8, 0xfffffff0), (rts + params, 99),
                              (rts + params + 8, 0xfffffff0)):
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                root_signature(overwrite(self.root, offset, value))
        for data in (self.root[:24], self.root[:-4]):
            with self.assertRaises(ValueError):
                root_signature(data)

    def test_reject_inconsistent_bindings(self):
        metadata = self.shader['metadata']
        for field, value in (('descriptor_offset_dwords', 1), ('register', 1000),
                             ('location_index', 100), ('location_dwords', 7)):
            mutated = copy.deepcopy(metadata)
            next(b for b in mutated['bindings'] if b['type'] == 'SRV')[field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                match_bindings(self.shader['root_signature'], mutated)
        mutated = copy.deepcopy(metadata)
        next(b for b in mutated['bindings'] if b['type'] == 'sampler')['register'] = 0
        with self.assertRaises(ValueError):
            match_bindings(self.shader['root_signature'], mutated)

    def test_corpus_consistency(self):
        corpus = FRONTEND['survey'](DLL)
        self.assertEqual(corpus['errors'], [])
        self.assertEqual(corpus['containers_checked'], 927)
        self.assertEqual(corpus['matched_bindings'], {'SRV': 4556, 'UAV': 2683, 'CBV': 1047, 'sampler': 520})
        self.assertEqual(corpus['root_versions'], {'2': 927})


class ISATests(Fixture):
    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        cls.decoder = LLVMDisassembler('gfx1030', 32)
        cls.isa = inspect_isa(cls.code, cls.decoder)

    @classmethod
    def tearDownClass(cls):
        cls.decoder.close()

    def test_selected_body_and_control_flow(self):
        self.assertEqual((self.isa['body_start'], self.isa['body_end_candidate']), (36, 0x2098))
        self.assertEqual(len(self.isa['instructions']), 1473)
        self.assertEqual(len(self.isa['opcode_counts']), 75)
        self.assertEqual(self.isa['control_flow']['branch_targets_checked'], 62)
        self.assertEqual(self.isa['control_flow']['unmodeled_control_flow'], [])
        end = 36
        for ins in self.isa['instructions']:
            self.assertEqual(ins['offset'], end)
            self.assertEqual(bytes.fromhex(ins['bytes']), self.code[end:end + ins['size']])
            end += ins['size']
        self.assertEqual(end, 0x2098)

    def test_exact_opaque_tail(self):
        tail = self.isa['opaque_tail']
        self.assertEqual(tail['size'], 232)
        self.assertEqual(bytes.fromhex(tail['hex']), self.code[0x2098:])
        self.assertEqual(words(bytes.fromhex(tail['hex']), 0x20c0 - 0x2098, 1), (0x105601,))

    def test_no_silent_resynchronization(self):
        with self.assertRaisesRegex(ValueError, 'undecodable instruction at 0x24'):
            self.decoder.decode(overwrite(self.code, 36, 0xffffffff), 36, 0x2098)
        for code in (self.code[:36], self.code[:-1]):
            with self.assertRaises(ValueError):
                inspect_isa(code, self.decoder)

    def test_reject_branch_into_middle_of_instruction(self):
        branch = {'offset': 0, 'size': 4, 'bytes': '010082bf', 'mnemonic': 's_branch', 'asm': 's_branch 1'}
        wide = {'offset': 4, 'size': 8, 'bytes': '00' * 8, 'mnemonic': 'v_nop_e64', 'asm': 'v_nop_e64'}
        end = {'offset': 12, 'size': 4, 'bytes': '000081bf', 'mnemonic': 's_endpgm', 'asm': 's_endpgm'}
        with self.assertRaisesRegex(ValueError, 'non-instruction 0x8'):
            control_flow([branch, wide, end], 0, 16)
        branch['bytes'] = '020082bf'
        flow = control_flow([branch, wide, end], 0, 16)
        self.assertEqual(flow['branches'][0]['target'], 12)

    def test_indirect_control_flow_is_reported(self):
        instructions = [{'offset': 0, 'size': 4, 'bytes': '00' * 4,
                         'mnemonic': 's_setpc_b64', 'asm': 's_setpc_b64 s[0:1]'},
                        self.isa['instructions'][-1]]
        flow = control_flow(instructions, 0, 0x2098)
        self.assertEqual(len(flow['unmodeled_control_flow']), 1)

    def test_llvm_cli_matches_ctypes_body(self):
        source = ' '.join(f'0x{byte:02x}' for byte in self.code[36:0x2098])
        result = subprocess.run(['llvm-mc', '--disassemble', '-triple=amdgcn-amd-amdpal',
                                 '-mcpu=gfx1030', '-mattr=+wavefrontsize32,-wavefrontsize64'],
                                input=source, text=True, capture_output=True, check=True)
        self.assertEqual(result.stderr, '')
        mnemonics = [line.strip().split()[0] for line in result.stdout.splitlines()
                     if line.strip() and not line.strip().startswith('.')]
        self.assertEqual(dict(Counter(mnemonics)), self.isa['opcode_counts'])

    def test_nop_opcode_against_assembler(self):
        result = subprocess.run(['llvm-mc', '-triple=amdgcn-amd-amdpal', '-mcpu=gfx1030', '--show-encoding'],
                                input='v_nop_e64\n', text=True, capture_output=True, check=True)
        self.assertIn('[0x00,0x00,0x80,0xd5,0x80,0x00,0x01,0x02]', result.stdout)

    def test_all_selected_image_ops_have_resource_provenance(self):
        shader = dict(self.shader, isa=self.isa)
        result = analyze_bindings(shader)
        self.assertEqual(result['reachable_instructions'], 1473)
        self.assertEqual(len(result['image_operations']), 102)
        self.assertEqual(result['unresolved_image_offsets'], [])
        self.assertEqual(result['image_binding_counts'], {'SRV:0': 16, 'UAV:1': 39, 'UAV:3': 28, 'UAV:2': 17, 'UAV:0': 2})
        self.assertEqual(len(result['descriptor_loads']), 39)
        self.assertTrue(all(load['binding_record'] is not None for load in result['descriptor_loads']))
        self.assertEqual(len(result['cbv_reads']), 20)
        self.assertTrue(all(read['CBV_register'] in (0, 1) for read in result['cbv_reads']))


class ProvenanceTests(unittest.TestCase):
    def test_scalar_unknowns_copies_and_signed_immediates(self):
        state = tuple([None] * 128)
        def apply(asm):
            nonlocal state
            state = transfer(state, {'asm': asm, 'mnemonic': asm.split()[0]}, [])
        apply('s_movk_i32 s0, 0xffff')
        apply('s_mov_b32 s1, s0')
        self.assertEqual(state[:2], (0xffffffff, 0xffffffff))
        apply('s_add_u32 s1, s1, 1')
        self.assertIsNone(state[1])  # Arithmetic is intentionally not evaluated.
        apply('s_mov_b64 s[2:3], s[0:1]')
        self.assertEqual(state[2:4], (0xffffffff, None))

    def diamond(self, right_slot):
        bindings = [{'record_offset': 24 + i * 32, 'type': 'UAV', 'register': i,
                     'location_register': 4, 'location_dwords': 8,
                     'source': {'kind': 'descriptor_table', 'root_parameter': 0, 'slot': i}}
                    for i in (0, 1)]
        asms = ['s_mov_b32 s5, 0', 's_cbranch_scc0 3',
                's_load_dwordx8 s[16:23], s[4:5], 0', 's_branch 2',
                f's_load_dwordx8 s[16:23], s[4:5], {right_slot * 32}',
                'image_load v0, v[1:2], s[16:23] dmask:0x1 dim:SQ_RSRC_IMG_2D', 's_endpgm']
        instructions = [{'offset': i * 4, 'asm': asm, 'mnemonic': asm.split()[0]} for i, asm in enumerate(asms)]
        return analyze_bindings({'matched_bindings': bindings, 'isa': {'instructions': instructions,
            'control_flow': {'unmodeled_control_flow': [], 'branches': [
                {'source': 4, 'target': 16, 'conditional': True},
                {'source': 12, 'target': 20, 'conditional': False}]}}})

    def test_identical_resource_at_cfg_join_survives(self):
        result = self.diamond(0)
        self.assertEqual(result['unresolved_image_offsets'], [])
        self.assertEqual(result['image_binding_counts'], {'UAV:0': 1})

    def test_different_resources_at_cfg_join_become_unknown(self):
        result = self.diamond(1)
        self.assertEqual(result['unresolved_image_offsets'], [20])
        self.assertIsNone(result['image_operations'][0]['binding_record'])


class GameACCCTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        dll = (REPO / 'game_dlls/amd_fidelityfx_upscaler_xbox.dll').read_bytes()
        start = 0x6f1b0  # First container in the game agent's extraction manifest.
        size = words(dll, start + 24, 1)[0]
        cls.blob = dll[start:start + size]
        offsets = words(cls.blob, 32, words(cls.blob, 28, 1)[0])
        cls.accc = next(o + 8 for o in offsets if cls.blob[o:o + 4] == b'ACCC')

    def test_game_accc_reuses_metadata_and_hash_dialect(self):
        shader, code = FRONTEND['inspect_accc'](self.blob)
        self.assertEqual(len(code), 3328)
        self.assertEqual(shader['instructions_sha256'][:16], '8154698fa648c64c')
        self.assertEqual(len(shader['matched_bindings']), 4)
        self.assertEqual(shader['prefix']['hash_hex'], shader['metadata']['hash_hex'])

    def test_reject_invalid_fspx_ranges_and_dialect(self):
        for offset, value in ((4, 2), (12, 0), (0x30, 1), (0x34, 0x70),
                              (0x34, 0xfffffff0), (0x38, 0xffffffff), (0x3c, 1)):
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                FRONTEND['inspect_accc'](overwrite(self.blob, self.accc + offset, value))
        with self.assertRaises(ValueError):
            FRONTEND['inspect_accc'](self.blob[:-1])

    def test_reject_hash_mutation_inside_accc(self):
        code_offset = words(self.blob, self.accc + 0x34, 1)[0]
        offset = self.accc + code_offset + 8
        changed = overwrite(self.blob, offset, words(self.blob, offset, 1)[0] ^ 1)
        with self.assertRaisesRegex(ValueError, 'hash differs'):
            FRONTEND['inspect_accc'](changed)


if __name__ == '__main__':
    unittest.main(verbosity=2)
