#!/usr/bin/env python3
"""Validate LLVM report parsing and PE exports against a real MinGW fixture."""
from pathlib import Path
import re
import runpy
import shutil
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
api = runpy.run_path(str(Path(__file__).with_name('inspect-game-report.py')))


class ReportTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.profile = api['parse_report'](ROOT / 'exe-report.txt')

    def test_known_counts_and_directories(self):
        p = self.profile
        self.assertEqual((p['import_summary']['dlls'], p['import_summary']['symbols']), (68, 587))
        self.assertEqual((p['import_summary']['by_name'], p['import_summary']['by_ordinal']), (563, 24))
        self.assertTrue(all(p['consistency_checks'].values()))
        self.assertEqual(p['entry_preferred_va'], 0x1456a1ec8)
        self.assertEqual(p['entry_raw_offset_from_report'], 0x56a12c8)
        self.assertEqual(p['directories']['TLSTableRVA'], 0x6771dd0)
        self.assertEqual(p['directories']['DelayImportDescriptorRVA'], 0)

    def test_duplicate_section_names_preserved(self):
        self.assertEqual(sum(s['name'] == '.xbld' for s in self.profile['sections']), 2)

    def test_hint_is_not_ordinal(self):
        modules = {m['name']: m for m in self.profile['modules']}
        self.assertEqual(modules['xmem.dll']['symbols'][0], {'name': 'XMemAlloc', 'hint': 0})
        self.assertIn({'name': 'freeaddrinfo', 'hint': 165}, modules['WS2_32.dll']['symbols'])
        self.assertEqual(modules['GameInput.dll']['symbols'], [{'ordinal': 1}])
        self.assertIn({'ordinal': 111}, modules['WS2_32.dll']['symbols'])

    def test_generated_probe_preserves_names_and_ordinals(self):
        header = api['emit_header'](self.profile)
        self.assertIn('{"XMemAlloc", 0}', header)
        self.assertEqual(header.count('{nullptr, '), 24)
        self.assertEqual(len(re.findall(r'^    \{".*", 0},$', header, re.M)), 563)

    def test_reject_incomplete_or_inconsistent_report(self):
        original = (ROOT / 'exe-report.txt').read_text()
        mutations = [original.replace('SectionCount: 10', 'SectionCount: 9'),
                     original.replace('Symbol: XMemAlloc (0)', 'Symbol: XMemAlloc (invalid)'),
                     original.replace('Name: xmem.dll', 'Name: ../xmem.dll'),
                     original.replace('AddressOfEntryPoint: 0x56A1EC8', 'AddressOfEntryPoint: 0xB06D100'),
                     original[:original.index('  Symbol: XMemAlloc (0)')]]
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory, 'bad.txt')
            for index, text in enumerate(mutations):
                with self.subTest(mutation=index):
                    path.write_text(text)
                    with self.assertRaises(ValueError):
                        api['parse_report'](path)

    def test_existing_xmem_exports(self):
        pe = api['ExportImage'](ROOT / 'tests/03-fidelityfx/xmem.dll')
        self.assertEqual(pe.machine, 0x8664)
        self.assertEqual(set(pe.names), {'XMemAlloc', 'XMemFree', 'XMemGetWorkingSetStatistics'})

    def test_tooling_xcrdapi_is_not_storage_provider(self):
        path = ROOT / ('PGDK-extracted/Microsoft GDK x86 Common-x86_en-us/Program Files/'
                       'Microsoft GDK/bin/xcrdapi.dll')
        pe = api['ExportImage'](path)
        self.assertEqual(pe.machine, 0x8664)
        self.assertNotIn('DStorageGetFactory', pe.names)
        self.assertIn('XCrdCreateXVD', pe.names)


@unittest.skipUnless(shutil.which('x86_64-w64-mingw32-g++') and shutil.which('llvm-readobj'),
                     'MinGW and LLVM required for independent export fixture')
class ExportFixtureTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory()
        cls.path = Path(cls.directory.name)
        cls.path.joinpath('fixture.cpp').write_text('extern "C" int fixture() { return 7; }\n')
        cls.path.joinpath('fixture.def').write_text(
            'LIBRARY fixture\nEXPORTS\n Named=fixture @2\n Hidden=fixture @9 NONAME\n'
            ' Forwarded=KERNEL32.GetTickCount @12\n')
        cls.dll = cls.path / 'fixture.dll'
        subprocess.run(['x86_64-w64-mingw32-g++', '-shared', str(cls.path / 'fixture.cpp'),
                        str(cls.path / 'fixture.def'), '-o', str(cls.dll)], check=True,
                       capture_output=True)
        cls.pe = api['ExportImage'](cls.dll)

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    def test_named_and_noname_ordinals(self):
        self.assertEqual(set(self.pe.names), {'Named', 'Forwarded'})
        self.assertEqual(set(self.pe.ordinals), {2, 9, 12})
        self.assertEqual(self.pe.names['Forwarded'], 'KERNEL32.GetTickCount')

    def test_export_holes_rejected(self):
        for ordinal in (1, 3, 8, 10, 11, 13):
            self.assertNotIn(ordinal, self.pe.ordinals)

    def test_agrees_with_llvm(self):
        result = subprocess.run(['llvm-readobj', '--coff-exports', str(self.dll)],
                                capture_output=True, text=True, check=True)
        nonzero = []
        for block in api['blocks'](result.stdout, 'Export'):
            # LLVM also prints unused EAT slots, with RVA 0.
            if 'ForwardedTo:' in block or api['number'](block, 'RVA'):
                nonzero.append(api['number'](block, 'Ordinal'))
        self.assertEqual(set(nonzero), set(self.pe.ordinals))

    def test_truncated_pe_rejected(self):
        path = self.path / 'truncated.dll'
        path.write_bytes(self.dll.read_bytes()[:80])
        with self.assertRaises(ValueError):
            api['ExportImage'](path)

    def test_unmapped_export_directory_rejected(self):
        blob = bytearray(self.dll.read_bytes())
        pe_offset = struct.unpack_from('<I', blob, 0x3c)[0]
        struct.pack_into('<I', blob, pe_offset + 24 + 112, 0xfffffff0)
        path = self.path / 'bad-rva.dll'
        path.write_bytes(blob)
        with self.assertRaises(ValueError):
            api['ExportImage'](path)


if __name__ == '__main__':
    unittest.main()
