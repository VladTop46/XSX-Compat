#!/usr/bin/env python3
"""Check the local Xbox call's provenance and PC ABI; does not run either DLL."""
import argparse
import hashlib
import json
import struct
import uuid
from pathlib import Path


class PE:
    def __init__(self, path):
        self.path = path
        self.data = path.read_bytes()
        pe = struct.unpack_from('<I', self.data, 0x3c)[0]
        if self.data[pe:pe + 4] != b'PE\0\0':
            raise ValueError(f'{path}: missing PE signature')
        if struct.unpack_from('<H', self.data, pe + 24)[0] != 0x20b:
            raise ValueError(f'{path}: expected PE32+')
        self.base = struct.unpack_from('<Q', self.data, pe + 48)[0]
        count = struct.unpack_from('<H', self.data, pe + 6)[0]
        section = pe + 24 + struct.unpack_from('<H', self.data, pe + 20)[0]
        self.sections = [struct.unpack_from('<IIII', self.data, section + i * 40 + 8)
                         for i in range(count)]

    def read(self, rva, size):
        for _, va, raw_size, raw in self.sections:
            if va <= rva and rva + size <= va + raw_size:
                start = raw + rva - va
                result = self.data[start:start + size]
                if len(result) == size:
                    return result
        raise ValueError(f'{self.path}: unmapped RVA {rva:#x}, size {size}')

    def expect(self, rva, hex_bytes):
        expected = bytes.fromhex(hex_bytes)
        actual = self.read(rva, len(expected))
        if actual != expected:
            raise ValueError(f'{self.path}: changed code at {rva:#x}: {actual.hex(" ")}')

    def rip_target(self, rva):
        return rva + 7 + struct.unpack('<i', self.read(rva + 3, 4))[0]


def inspect(directory):
    xbox = PE(directory / 'amd_fidelityfx_upscaler_xbox.dll')
    pc = PE(directory / 'd3d12core.dll')
    heap_iid = '8efb471d-616c-4f49-90f7-127bb763fa51'
    # Both functions get the backend from the same interface field +0xb8.
    xbox.expect(0x2cef, '48 8b 99 b8 00 00 00')
    xbox.expect(0x33ad, '48 8b 99 b8 00 00 00')
    xbox.expect(0x33e9, '48 89 5c 24 60')  # save backend at rsp+0x60
    xbox.expect(0x2dd4, '4c 8d 4b 28')     # output = backend+0x28
    xbox.expect(0x2ddb, '48 8d 54 24 20')  # descriptor heap desc
    xbox.expect(0x2dea, '4c 8d 05')        # IID argument (R8)
    iid_rva = xbox.rip_target(0x2dea)
    actual_iid = str(uuid.UUID(bytes_le=xbox.read(iid_rva, 16)))
    if actual_iid != heap_iid:
        raise ValueError(f'Create IID changed: {actual_iid}')
    xbox.expect(0x2df4, 'ff 50 70')
    xbox.expect(0x39ec, '48 8b 4c 24 60 48 8b 49 28 48 8b 01 ff 50 48')
    xbox.expect(0x39fb, '33 d2 49 8b ce 48 8b d8 49 8b 06 ff 50 78')
    xbox.expect(0x3a1a, '0f af 06 44 8b c8 49 8b 06 4c 03 cb ff 90 90 00 00 00')
    pc.expect(0x2a9d0, '48 83 ec 28 48 89 d0 48 85 c9')
    pc.expect(0x2a9e0, '48 8d 15')
    pc.expect(0x2a9e7, '48 39 11 75 0c 48 8b 51 28 48 89 10 48 83 c4 28 c3')
    vtable = pc.rip_target(0x2a9e0)
    method = struct.unpack('<Q', pc.read(vtable + 9 * 8, 8))[0] - pc.base
    if method != 0x2a9d0:
        raise ValueError(f'PC heap slot 9 changed to {method:#x}')
    # Texture2D without FFX_RESOURCE_USAGE_DCC_RENDERTARGET selects 0x101,
    # then copies it to Layout at offset 0x2c of the resource desc at rbp+0x40.
    xbox.expect(0x372a, '83 f9 02 75 15')
    xbox.expect(0x3732, 'b9 01 01 00 00 f7 47 34 00 80 00 00 0f 44 c1 89 45 14')
    xbox.expect(0x3805, '8b 45 14')
    xbox.expect(0x3820, '89 45 6c')
    return {
        'evidence': 'static binary checks; no runtime identity measurement',
        'runtime_verified': False,
        'sha256': {pe.path.name: hashlib.sha256(pe.data).hexdigest() for pe in (xbox, pc)},
        'xbox': {'image_base': hex(xbox.base), 'backend_source': '[interface+0xb8]',
                 'backend_stack_slot': '[rsp+0x60]', 'creation_output': '[backend+0x28]',
                 'iid_rva': hex(iid_rva), 'iid': actual_iid,
                 'interface': 'ID3D12DescriptorHeap', 'call_rva': '0x39f8',
                 'return': 'CPU descriptor handle value in RAX',
                 'texture_layout': {'selector_rva': '0x3732', 'value': '0x101',
                                    'condition': 'Texture2D and usage bit 0x8000 absent',
                                    'resource_desc_offset': '0x2c'}},
        'pc': {'image_base': hex(pc.base), 'vtable_rva': hex(vtable),
               'slot_9_rva': hex(method), 'method': 'GetCPUDescriptorHandleForHeapStart',
               'return': 'write handle to RDX; return output pointer in RAX'},
    }


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', nargs='?', type=Path,
                        default=Path(__file__).resolve().parents[2] / 'tests/03-fidelityfx')
    args = parser.parse_args()
    print(json.dumps(inspect(args.directory), indent=2))
