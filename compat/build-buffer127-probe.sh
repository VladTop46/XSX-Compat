#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="$ROOT/tests/13-buffer127"
mkdir -p "$DEST"
x86_64-w64-mingw32-g++ -std=c++17 -O2 -static -Wall -Wextra -Werror \
    "$ROOT/compat/tests/buffer127-probe.cpp" -o "$DEST/buffer127-probe.exe"
cp "$ROOT/compat/build/d3d12_xs.dll" "$DEST/"
for name in d3d12.dll d3d12core.dll dxgi.dll PIXEvt.dll; do
    cp "$ROOT/tests/03-fidelityfx/$name" "$DEST/"
done
