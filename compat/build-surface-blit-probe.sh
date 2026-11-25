#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="$ROOT/tests/12-present-blit"
mkdir -p "$DEST"
x86_64-w64-mingw32-g++ -std=c++17 -O2 -static -Wall -Wextra -Werror \
    "$ROOT/compat/tests/surface-blit-probe.cpp" -o "$DEST/surface-blit-probe.exe"
for name in d3d12.dll d3d12core.dll dxgi.dll PIXEvt.dll; do
    cp "$ROOT/tests/03-fidelityfx/$name" "$DEST/"
done
# Retain the exact game adapter alongside the probe for provenance. The probe
# uses native d3d12.dll and the shared SurfaceBlit implementation directly.
cp "$ROOT/compat/build/d3d12_xs.dll" "$DEST/"
