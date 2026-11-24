#!/usr/bin/env python3
"""Lower a supported native VS to HLSL and compile actual PC DXIL with DXC."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / 'compat/tests'))
from xsx_shader_translate import translate_vertex


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('shader', type=Path)
    parser.add_argument('--out-dir', type=Path, required=True)
    parser.add_argument('--dxc', type=Path, default=REPO / 'sdk/dxc/bin/dxc')
    args = parser.parse_args()
    try:
        result = translate_vertex(args.shader.read_bytes())
        args.out_dir.mkdir(parents=True, exist_ok=True)
        hlsl = args.out_dir / 'vertex.hlsl'
        dxil = args.out_dir / 'vertex.dxil'
        hlsl.write_text(result.pop('hlsl'))
        command = [str(args.dxc), '-T', 'vs_6_0', '-E', 'main', '-Ges', '-Fo', str(dxil),
                   '-Fc', str(args.out_dir / 'vertex.dxil.txt'), str(hlsl)]
        compiled = subprocess.run(command, capture_output=True, text=True)
        (args.out_dir / 'dxc.log').write_text(compiled.stdout + compiled.stderr)
        if compiled.returncode:
            raise ValueError(f'DXC failed with exit {compiled.returncode}')
        result.update(dxc_command=command, dxil_sha256=hashlib.sha256(dxil.read_bytes()).hexdigest(),
                      gpu_execution_tested=False)
        (args.out_dir / 'manifest.json').write_text(json.dumps(result, indent=2) + '\n')
        print(f'Lowered {len(result["instructions"])} native instructions; PC DXIL: {dxil}')
    except (OSError, ValueError) as error:
        parser.exit(1, f'{error}\n')


if __name__ == '__main__':
    main()
