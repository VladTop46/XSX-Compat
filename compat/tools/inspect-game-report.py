#!/usr/bin/env python3
"""Audit an LLVM COFF report against local DLL exports, without loading code.

Named Symbol: Foo (N) entries carry a hint, not an ordinal. Only an empty
symbol name denotes an ordinal import in llvm-readobj --coff-imports output.
"""
import argparse
from collections import Counter
import hashlib
import json
import os
from pathlib import Path
import re
import struct

PROJECT_MODULES = {
    'xmem.dll': 'Xbox memory', 'd3d12_xs.dll': 'Xbox graphics bootstrap',
    'xg_xs.dll': 'Xbox texture layout', 'xboxbvhbuilder.dll': 'Xbox ray tracing',
    'xcrdapi.dll': 'DirectStorage', 'chad.dll': 'Xbox DSP',
    'xapu.dll': 'Xbox APU', 'acphal.dll': 'Xbox audio HAL',
    'amd_fidelityfx_loader_xbox.dll': 'Xbox FidelityFX',
    'libhttpclient.gdk.dll': 'HTTP runtime', 'bink2scarlett.dll': 'Xbox Bink',
    'gameinput.dll': 'GameInput',
}


def number(text, key):
    match = re.search(r'^\s*' + re.escape(key) +
                      r': (?:[A-Za-z_][A-Za-z0-9_]* \()?(0x[0-9a-fA-F]+|[0-9]+)\b',
                      text, re.M)
    if not match:
        raise ValueError(f'missing numeric field {key}')
    return int(match[1], 0)


def blocks(text, label, indent=''):
    """LLVM braces are on separate lines; reject incomplete named blocks."""
    start = re.escape(indent + label + ' {')
    found = list(re.finditer('^' + start + r'\n(.*?)^' + re.escape(indent) + r'}$',
                             text, re.M | re.S))
    if len(found) != len(re.findall('^' + start + '$', text, re.M)):
        raise ValueError(f'incomplete {label} block')
    return [match[1] for match in found]


def parse_report(path):
    raw = path.read_bytes()
    text = raw.decode('utf-8').replace('\r\n', '\n')
    file_match = re.search(r'^File: (.+)$', text, re.M)
    if not file_match or len(re.findall(r'^File:', text, re.M)) != 1:
        raise ValueError('expected a report for exactly one file')
    headers = blocks(text, 'ImageOptionalHeader')
    if len(headers) != 1 or number(headers[0], 'Magic') != 0x20b:
        raise ValueError('expected PE32+ optional header')
    header = headers[0]
    modules = []
    for block in blocks(text, 'Import'):
        match = re.search(r'^  Name: ([A-Za-z0-9_.-]+\.dll)$', block, re.M | re.I)
        if not match:
            raise ValueError('invalid import module name')
        symbols = []
        for line in block.splitlines():
            if not line.startswith('  Symbol:'):
                continue
            symbol = re.fullmatch(r'  Symbol: (.*?) \((\d+)\)', line)
            if not symbol or not 0 <= int(symbol[2]) <= 0xffff:
                raise ValueError(f'invalid symbol line: {line}')
            name, value = symbol[1], int(symbol[2])
            symbols.append({'name': name, 'hint': value} if name else {'ordinal': value})
        if not symbols:
            raise ValueError(f'{match[1]} has no imports')
        key = match[1].lower()
        modules.append({'name': match[1], 'kind': 'API set contract' if key.startswith(
            ('api-', 'ext-')) else PROJECT_MODULES.get(key, 'Windows/CRT'),
            'lookup_rva': number(block, 'ImportLookupTableRVA'),
            'iat_rva': number(block, 'ImportAddressTableRVA'), 'symbols': symbols})
    if not modules or len({m['name'].lower() for m in modules}) != len(modules):
        raise ValueError('missing or duplicate import descriptors')
    sections = []
    for block in blocks(text, 'Section', '  '):
        match = re.search(r'^    Name: (\S+)', block, re.M)
        if not match:
            raise ValueError('section name missing')
        sections.append({'number': number(block, 'Number'), 'name': match[1],
                         'rva': number(block, 'VirtualAddress'),
                         'virtual_size': number(block, 'VirtualSize'),
                         'raw_size': number(block, 'RawDataSize'),
                         'raw_offset': number(block, 'PointerToRawData'),
                         'executable': 'IMAGE_SCN_MEM_EXECUTE' in block})
    if len(sections) != number(text, 'SectionCount'):
        raise ValueError('section count differs from report header')
    entry = number(header, 'AddressOfEntryPoint')
    containing = [s for s in sections if s['rva'] <= entry < s['rva'] + s['virtual_size']]
    if len(containing) != 1 or not containing[0]['executable']:
        raise ValueError('entry point is not in a unique executable section')
    section = containing[0]
    if entry - section['rva'] >= section['raw_size']:
        raise ValueError('entry point has no reported raw bytes')
    if any(s['rva'] + s['virtual_size'] > number(header, 'SizeOfImage') for s in sections):
        raise ValueError('section outside image')
    imports = sum(len(m['symbols']) for m in modules)
    iat_size = sum((len(m['symbols']) + 1) * 8 for m in modules)
    image_base = number(header, 'ImageBase')
    return {'target': file_match[1], 'evidence': 'text report; target binary not inspected',
            'report_sha256': hashlib.sha256(raw).hexdigest(),
            'machine': number(text, 'Machine'), 'optional_magic': 0x20b,
            'image_base': image_base, 'entry_rva': entry, 'entry_preferred_va': image_base + entry,
            'entry_raw_offset_from_report': section['raw_offset'] + entry - section['rva'],
            'size_of_image': number(header, 'SizeOfImage'),
            'minimum_raw_file_size_from_sections': max(s['raw_offset'] + s['raw_size'] for s in sections),
            'sections': sections,
            'directories': {key: number(header, key) for key in (
                'TLSTableRVA', 'TLSTableSize', 'ExceptionTableRVA', 'ExceptionTableSize',
                'BaseRelocationTableRVA', 'BaseRelocationTableSize', 'LoadConfigTableRVA',
                'LoadConfigTableSize', 'DelayImportDescriptorRVA', 'DelayImportDescriptorSize')},
            'import_summary': {'dlls': len(modules), 'symbols': imports,
                               'by_name': sum('name' in s for m in modules for s in m['symbols']),
                               'by_ordinal': sum('ordinal' in s for m in modules for s in m['symbols']),
                               'kinds': dict(Counter(m['kind'] for m in modules))},
            'consistency_checks': {
                'iat_size_matches_all_thunks_and_terminators': iat_size == number(header, 'IATSize'),
                'import_directory_matches_descriptors_and_terminator':
                    (len(modules) + 1) * 20 == number(header, 'ImportTableSize')},
            'modules': modules,
            'limitations': ['No target code, TLS callbacks or dynamic loads were inspected.',
                            'Export presence does not validate ABI, semantics or dependency initialization.',
                            'Local candidates are inventory, not the Windows/Wine DLL search result.']}


class ExportImage:
    """Bounded PE32/PE32+ export reader, including forwarders and ordinal holes."""
    def __init__(self, path):
        self.data = path.read_bytes()
        if self.take(0, 2) != b'MZ':
            raise ValueError('not MZ')
        pe = self.unpack('<I', 0x3c)[0]
        if self.take(pe, 4) != b'PE\0\0':
            raise ValueError('not PE')
        self.machine, sections = self.unpack('<HH', pe + 4)
        opt = pe + 24
        opt_size = self.unpack('<H', pe + 20)[0]
        magic = self.unpack('<H', opt)[0]
        if magic not in (0x10b, 0x20b):
            raise ValueError('not PE32/PE32+')
        directory_offset = 112 if magic == 0x20b else 96
        if opt_size < directory_offset + 8:
            raise ValueError('optional header has no export directory')
        self.take(opt, opt_size)
        self.headers = self.unpack('<I', opt + 60)[0]
        self.sections = [self.unpack('<IIII', opt + opt_size + i * 40 + 8)
                         for i in range(sections)]
        export_rva, export_size = self.unpack('<II', opt + directory_offset)
        self.names, self.ordinals = {}, {}
        if not export_rva:
            return
        header = self.rva(export_rva, 40)
        base, count, named, functions, names, ordinals = struct.unpack_from('<6I', header, 16)
        eat = self.rva(functions, count * 4)
        for i in range(count):
            address = struct.unpack_from('<I', eat, i * 4)[0]
            if address:  # Zero EAT slots are holes, not available ordinal exports.
                self.ordinals[base + i] = self.string(address) if (
                    export_rva <= address < export_rva + export_size) else None
        npt, ot = self.rva(names, named * 4), self.rva(ordinals, named * 2)
        for i in range(named):
            ordinal = base + struct.unpack_from('<H', ot, i * 2)[0]
            if ordinal not in self.ordinals:
                raise ValueError('named export points outside EAT or at an empty slot')
            self.names[self.string(struct.unpack_from('<I', npt, i * 4)[0])] = self.ordinals[ordinal]

    def take(self, offset, size):
        if offset < 0 or size < 0 or offset + size > len(self.data):
            raise ValueError('truncated PE file')
        return self.data[offset:offset + size]

    def unpack(self, fmt, offset):
        return struct.unpack(fmt, self.take(offset, struct.calcsize(fmt)))

    def mapping(self, rva):
        if 0 <= rva < min(self.headers, len(self.data)):
            return rva, min(self.headers, len(self.data))
        for _, va, size, raw in self.sections:
            if va <= rva < va + size:
                return raw + rva - va, min(raw + size, len(self.data))
        raise ValueError(f'unmapped RVA {rva:#x}')

    def rva(self, rva, size):
        if not size:
            return b''
        offset, end = self.mapping(rva)
        if offset + size > end:
            raise ValueError('RVA range outside raw data')
        return self.take(offset, size)

    def string(self, rva):
        offset, limit = self.mapping(rva)
        end = self.data.find(b'\0', offset, limit)
        if end < 0:
            raise ValueError('unterminated export string')
        return self.data[offset:end].decode('ascii')


def inventory(profile, root):
    names = {m['name'].lower() for m in profile['modules']}
    candidates = {name: [] for name in names}
    for directory, dirs, files in os.walk(root):
        dirs[:] = sorted(d for d in dirs if not d.startswith('.') and
                         Path(directory, d) != root / 'compat/reports')
        for name in sorted(files):
            if name.lower() in candidates:
                candidates[name.lower()].append(Path(directory, name))
    for module in profile['modules']:
        module['local_candidates'] = []
        for path in sorted(candidates[module['name'].lower()]):
            item = {'path': path.relative_to(root).as_posix()}
            try:
                pe = ExportImage(path)
                missing, forwarders = [], {}
                for symbol in module['symbols']:
                    key = symbol.get('name', symbol.get('ordinal'))
                    exports = pe.names if 'name' in symbol else pe.ordinals
                    if key not in exports:
                        missing.append(symbol)
                    elif exports[key]:
                        forwarders[str(key)] = exports[key]
                item.update({'machine': pe.machine, 'machine_matches': pe.machine == profile['machine'],
                             'sha256': hashlib.sha256(pe.data).hexdigest(),
                             'required_exports_present': len(module['symbols']) - len(missing),
                             'missing': missing, 'forwarders_not_resolved': forwarders})
            except (OSError, ValueError, struct.error) as error:
                item['error'] = str(error)
            module['local_candidates'].append(item)
        module['status'] = 'requires_loader_probe'
        if any(c.get('machine_matches') and not c['missing'] for c in module['local_candidates']):
            module['status'] = 'exports_present_runtime_unverified'
        elif module['local_candidates']:
            module['status'] = 'local_candidates_incomplete_or_wrong_architecture'
        elif module['name'].lower() in PROJECT_MODULES:
            module['status'] = 'no_candidate_in_checkout'


def emit_header(profile):
    quote = json.dumps
    lines = ['#pragma once', '// Generated by inspect-game-report.py; do not edit.',
             'namespace GameTarget {',
             f'inline constexpr char image[] = {quote(profile["target"])};',
             f'inline constexpr char report_sha256[] = {quote(profile["report_sha256"])};',
             'struct Import { const char* name; unsigned short ordinal; };',
             'struct Module { const char* name; const Import* imports; unsigned count; };']
    for index, module in enumerate(profile['modules']):
        lines.append(f'inline constexpr Import imports_{index}[] = {{')
        for symbol in module['symbols']:
            lines.append('    {' + quote(symbol['name']) + ', 0},' if 'name' in symbol
                         else '    {nullptr, ' + str(symbol['ordinal']) + '},')
        lines.append('};')
    lines.append('inline constexpr Module modules[] = {')
    for index, module in enumerate(profile['modules']):
        lines.append('    {' + quote(module['name']) + f', imports_{index}, {len(module["symbols"])}' + '},')
    lines.extend(['};', '}'])
    return '\n'.join(lines) + '\n'


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('report', type=Path)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument('--out', type=Path)
    parser.add_argument('--probe-header', type=Path)
    args = parser.parse_args()
    try:
        profile = parse_report(args.report)
        inventory(profile, args.root.resolve())
        result = json.dumps(profile, indent=2, ensure_ascii=False) + '\n'
        if args.out:
            args.out.write_text(result)
        else:
            print(result, end='')
        if args.probe_header:
            args.probe_header.write_text(emit_header(profile))
    except (OSError, ValueError, struct.error) as error:
        parser.exit(1, f'{error}\n')
