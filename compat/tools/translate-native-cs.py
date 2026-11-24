#!/usr/bin/env python3
"""Lower the supported native constant-store CS subset and compile PC DXIL."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / 'compat/tests'))
from xsx_shader_translate import translate_compute_reset


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('shader', type=Path)
    parser.add_argument('--out-dir', required=True, type=Path)
    args = parser.parse_args()
    try:
        result = translate_compute_reset(args.shader.read_bytes())
        args.out_dir.mkdir(parents=True, exist_ok=True)
        source, target = args.out_dir / 'compute.hlsl', args.out_dir / 'compute.dxil'
        source.write_text(result.pop('hlsl'))
        command = [str(REPO / 'sdk/dxc/bin/dxc'), '-T', 'cs_6_0', '-E', 'main', '-Ges',
                   '-Fo', str(target), '-Fc', str(args.out_dir / 'compute.dxil.txt'), str(source)]
        compiled = subprocess.run(command, capture_output=True, text=True)
        (args.out_dir / 'dxc.log').write_text(compiled.stdout + compiled.stderr)
        if compiled.returncode:
            raise ValueError(f'DXC failed: exit={compiled.returncode}')
        result.update(dxc_command=command, dxil_sha256=hashlib.sha256(target.read_bytes()).hexdigest(),
                      gpu_execution_tested=False)
        (args.out_dir / 'manifest.json').write_text(json.dumps(result, indent=2) + '\n')
        print(f'Lowered {len(result["instructions"])} native instructions; PC DXIL: {target}')
    except (OSError, ValueError) as error:
        parser.exit(1, f'{error}\n')


if __name__ == '__main__':
    main()
