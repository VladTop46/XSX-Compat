#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="$ROOT/tests/11-resource127"
mkdir -p "$DEST"
for variant in title fallback; do
    options=()
    name=resource127-probe
    if [[ "$variant" == fallback ]]; then
        options+=(-DXSX_NO_TITLE_HOOKS=1 -Wno-unused-function)
        name=resource127-fallback-probe
    fi
    x86_64-w64-mingw32-g++ -std=c++17 -O2 -static -Wall -Wextra -Werror \
        "${options[@]}" "$ROOT/compat/tests/resource127-probe.cpp" -o "$DEST/$name.exe"
done
cp "$ROOT/compat/build/d3d12_xs.dll" "$DEST/"
cp "$ROOT/compat/build/sys/xmem.dll" "$DEST/"
for name in d3d12.dll d3d12core.dll dxgi.dll PIXEvt.dll; do
    cp "$ROOT/tests/03-fidelityfx/$name" "$DEST/"
done
