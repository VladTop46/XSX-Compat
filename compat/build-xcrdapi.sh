#!/usr/bin/env bash
# XCRDAPI.dll (DirectStorage Xbox-эмуляция: файлы + zlib inflate).
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"; OUT="$ROOT/compat/build/sys"; mkdir -p "$OUT"
x86_64-w64-mingw32-g++ -std=c++17 -O2 -static -shared "$ROOT/compat/runtime/xcrdapi/xcrdapi.cpp" "$ROOT/compat/runtime/xcrdapi/xcrdapi.def" -o "$OUT/XCRDAPI.dll"
echo "Built $OUT/XCRDAPI.dll"
