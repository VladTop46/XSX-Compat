#!/usr/bin/env bash
# Compile a private snapshot of the eight translated UI programs; no runtime DLL changes.
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$ROOT/tests/16-ui-shaders"
mkdir -p "$OUT"
python3 - "$ROOT" "$OUT" <<'PY'
import hashlib,json,shutil,subprocess,sys
from pathlib import Path
root,out=map(Path,sys.argv[1:])
pairs=[('04cf10b425938ecca5b19f2ae14592ab','b2e813c0805d55cf8c1656ea3e1a1f52'),
       ('903b71572235f771cc3d56d82dca4eb0','b8e1f7f9f90f9d2198eca2e382969b5a'),
       ('2ef3393457fdae7f2ba94a5a434966d4','e607033bd3c74a21cb4d1d66a57466bd'),
       ('acf05b2957c6c338e605f5afa2e2405d','b741098a3d02fac32daec6003e069416')]
assets=out/'programs';assets.mkdir(exist_ok=True)
text=['#pragma once','// Private generated fixtures; contains translated game programs.']
manifest=[]
for pair in pairs:
 for stage,h in zip(['VS','PS'],pair):
  source=root/'compat/shader-cache'/f'{h}.hlsl'
  bind=root/'compat/shader-cache'/f'{h}.bind'
  binding=bind.read_text().splitlines()
  assert f'stage {stage}' in binding and f'hash {h}' in binding,h
  snapshot=source.read_bytes()
  (assets/source.name).write_bytes(snapshot)
  shutil.copy2(bind,assets/bind.name)
  dxil=assets/f'{h}.dxil'
  subprocess.run([str(root/'sdk/dxc/bin/dxc'),'-T',stage.lower()+'_6_0','-E','main','-Ges','-WX','-Fo',str(dxil),str(assets/source.name)],check=True)
  assert source.read_bytes()==snapshot,'shared source changed during compilation'
  data=dxil.read_bytes()
  assert data[:4]==b'DXBC' and len(data)>32
  symbol='code_'+h
  text.append(f'alignas(4) static const unsigned char {symbol}[] = {{')
  text.extend(','.join(f'0x{v:02x}' for v in data[i:i+16])+',' for i in range(0,len(data),16))
  text.append('};')
  manifest.append({'stage':stage,'program_hash':h,'source_sha256':hashlib.sha256(snapshot).hexdigest(),'binding_sha256':hashlib.sha256(bind.read_bytes()).hexdigest(),'dxil_sha256':hashlib.sha256(data).hexdigest(),'dxil_bytes':len(data)})
text.append('struct ShaderPair { D3D12_SHADER_BYTECODE vs,ps; const char *vsHash,*psHash; };')
text.append('static const ShaderPair shaderPairs[] = {')
for vs,ps in pairs:
 text.append('{{code_'+vs+',sizeof(code_'+vs+')},{code_'+ps+',sizeof(code_'+ps+')},"'+vs+'","'+ps+'"},')
text.append('};')
(out/'ui-shader-data.h').write_text('\n'.join(text)+'\n')
(out/'program-manifest.json').write_text(json.dumps({'programs':manifest,'source':'existing native ISA lowering; no synthetic VS/PS','gpu_tested':False},indent=2)+'\n')
print('[BUILD] DXC programs=8 pairs=4 PASS')
PY
x86_64-w64-mingw32-g++ -std=c++17 -O2 -static -Wall -Wextra -Werror \
  -I"$OUT" "$ROOT/compat/tests/ui-shader-readback.cpp" -o "$OUT/ui-shader-readback.exe" -ld3d12 -ldxguid
for dll in d3d12.dll d3d12core.dll dxgi.dll; do
  cp "$ROOT/tests/03-fidelityfx/$dll" "$OUT/$dll"
done
echo "Built $OUT/ui-shader-readback.exe"
