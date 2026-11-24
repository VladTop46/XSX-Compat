#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
SOURCE="${1:-$ROOT/compat/reports/native-shaders-20261124/compute-reset/shader.dxbc}"
OUT="$ROOT/tests/06-native-shaders"
mkdir -p "$OUT"
python3 "$ROOT/compat/tools/translate-native-cs.py" "$SOURCE" --out-dir "$OUT/translated-cs"
python3 - "$OUT" <<'PY'
from pathlib import Path
import json
import sys
p = Path(sys.argv[1])
m = json.loads((p / 'translated-cs/manifest.json').read_text())
text = '#pragma once\n#define NATIVE_PROGRAM_HASH "' + m['source_program_hash'] + '"\n'
text += f'#define UAV_REGISTER {m["resource"]["lower"]}\n#define UAV_SPACE {m["resource"]["space"]}\n#define STORE_DWORDS {m["store_dwords"]}u\n'
data = (p / 'translated-cs/compute.dxil').read_bytes()
rows = [','.join(f'0x{b:02x}' for b in data[i:i+16]) for i in range(0, len(data), 16)]
text += 'static const unsigned char kNativeCS[] = {\n' + ',\n'.join(rows) + '\n};\n'
(p / 'native-cs-data.h').write_text(text)
PY
x86_64-w64-mingw32-g++ -std=c++17 -O2 -static -I"$OUT" \
  "$ROOT/compat/tests/native-cs-pso.cpp" -o "$OUT/native-cs-pso.exe" -ld3d12 -ldxguid
for dll in d3d12.dll d3d12core.dll dxgi.dll; do
  cp "$ROOT/tests/03-fidelityfx/$dll" "$OUT/$dll"
done
echo "Built $OUT/native-cs-pso.exe"
