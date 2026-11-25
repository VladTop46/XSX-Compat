#!/usr/bin/env bash
# Private integration fixtures: never copy generated headers/ISA/DXIL to git_repo.
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$ROOT/tests/17-native-ui-bridge"
mkdir -p "$OUT/shader-cache"
python3 - "$ROOT" "$OUT" <<'PY'
import hashlib,json,shutil,struct,sys
from pathlib import Path
root,out=map(Path,sys.argv[1:]);data=root/'local-agent-work/data'
text=['#pragma once','// PRIVATE captured ISA and layout fixtures; not distributable project sources.']
manifest=[]
for i in range(1,5):
 for j,q in enumerate((1,3,5,7,9,11)):
  # Fourth captured q1 is corrupted by interleaved logging. PSO creation for
  # pair4 uses pair3's explicit UI state; it is not claimed as recovered state4.
  source=data/f'pso{3 if i==4 and q==1 else i}_q{q}.bin'
  captured=source.read_bytes();b=captured
  if q==3:
   # USEDPSO searched both neighbors and incorrectly used the preceding state
   # size for this layout. Keep only the semantic records and DWORD padding.
   count=struct.unpack_from('<I',b,0x4c)[0];end=0x50
   assert 0<count<=16
   for _ in range(count):
    length=struct.unpack_from('<I',b,end)[0];end+=4+length+24
    assert end<=len(b)
   b=b[:(end+3)&~3]
  name=f'pso{i}_part{j}'
  text.append(f'alignas(8) static const unsigned char {name}[]={{')
  text.extend(','.join(f'0x{x:02x}' for x in b[p:p+16])+',' for p in range(0,len(b),16));text.append('};')
  manifest.append({'pair':i,'part':j,'source':str(source.relative_to(root)),'captured_bytes':len(captured),'bytes':len(b),'sha256':hashlib.sha256(b).hexdigest()})
text.append('struct FixtureBlob { const unsigned char* data; size_t size; };')
text.append('static const FixtureBlob fixtures[4][6]={')
for i in range(1,5):text.append('{'+','.join(f'{{pso{i}_part{j},sizeof pso{i}_part{j}}}' for j in range(6))+'},')
text.append('};');(out/'native-ui-fixtures.h').write_text('\n'.join(text)+'\n')
programs=root/'tests/16-ui-shaders/programs'
for entry in json.loads((root/'tests/16-ui-shaders/program-manifest.json').read_text())['programs']:
 h=entry['program_hash']
 for ext,key in [('dxil','dxil_sha256'),('bind','binding_sha256')]:
  source=programs/f'{h}.{ext}';assert hashlib.sha256(source.read_bytes()).hexdigest()==entry[key]
  shutil.copy2(source,out/'shader-cache'/source.name)
(out/'fixture-manifest.json').write_text(json.dumps({'description_layout':'opaque qword then six address,size records; 104 bytes; code blobs are bounded captured hash-prefix fixtures, not recovered complete native shader allocations','blobs':manifest,'input_layout':'bounded semantic records plus DWORD padding; neighbor-size overread tail removed','pair4_fixed_state':'explicit pair3 state; corrupted captured q1 is unused','shader_source':'frozen tests/16-ui-shaders/programs'},indent=2)+'\n')
PY
x86_64-w64-mingw32-g++ -std=c++17 -O2 -static -Wall -Wextra -Werror \
  -I"$OUT" "$ROOT/compat/tests/native-ui-bridge-probe.cpp" -o "$OUT/native-ui-bridge-probe.exe" -ld3d12 -ldxguid
for dll in d3d12.dll d3d12core.dll dxgi.dll; do
  cp "$ROOT/tests/03-fidelityfx/$dll" "$OUT/$dll"
done
cp "$ROOT/local-agent-work/build/d3d12_xs.dll" "$OUT/d3d12_xs.dll"
sha256sum "$OUT/native-ui-bridge-probe.exe" "$OUT/d3d12_xs.dll"
