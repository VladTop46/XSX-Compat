#!/usr/bin/env bash
# xmem.dll (XMem*: VirtualAlloc/физические страницы).
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"; OUT="$ROOT/compat/build/sys"; mkdir -p "$OUT"
x86_64-w64-mingw32-g++ -std=c++17 -O2 -static -shared "$ROOT/compat/runtime/xmem/xmem.cpp" "$ROOT/compat/runtime/xmem/xmem.def" -o "$OUT/xmem.dll"
echo "Built $OUT/xmem.dll"
