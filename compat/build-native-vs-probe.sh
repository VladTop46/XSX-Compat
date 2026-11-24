#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
SOURCE="${1:-$ROOT/compat/reports/native-shaders-20261124/pack/first-VS/shader.dxbc}"
OUT="$ROOT/tests/06-native-shaders"
mkdir -p "$OUT"
python3 "$ROOT/compat/tools/translate-native-vs.py" "$SOURCE" --out-dir "$OUT/translated"
cat > "$OUT/probe-ps.hlsl" <<'HLSL'
// Synthetic pixel shader used only to observe the translated native VS output.
float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target0 {
    return float4(uv, 0, 1);
}
HLSL
"$ROOT/sdk/dxc/bin/dxc" -T ps_6_0 -E main -Ges -Fo "$OUT/probe-ps.dxil" "$OUT/probe-ps.hlsl"
python3 - "$OUT" <<'PY'
import json
from pathlib import Path
import sys
p = Path(sys.argv[1])
manifest = json.loads((p / 'translated/manifest.json').read_text())
text = '#pragma once\n#define NATIVE_PROGRAM_HASH "' + manifest['source_program_hash'] + '"\n'
for name, path in [('kNativeVS', p / 'translated/vertex.dxil'), ('kProbePS', p / 'probe-ps.dxil')]:
    data = path.read_bytes()
    rows = [','.join(f'0x{b:02x}' for b in data[i:i+16]) for i in range(0, len(data), 16)]
    text += 'static const unsigned char ' + name + '[] = {\n' + ',\n'.join(rows) + '\n};\n'
(p / 'native-vs-data.h').write_text(text)
PY
x86_64-w64-mingw32-g++ -std=c++17 -O2 -static -I"$OUT" \
  "$ROOT/compat/tests/native-vs-pso.cpp" -o "$OUT/native-vs-pso.exe" -ld3d12 -ldxguid
for dll in d3d12.dll d3d12core.dll dxgi.dll; do
  cp "$ROOT/tests/03-fidelityfx/$dll" "$OUT/$dll"
done
echo "Built $OUT/native-vs-pso.exe"
