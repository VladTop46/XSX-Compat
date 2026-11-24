#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$ROOT/compat/build/sys"
mkdir -p "$OUT"
x86_64-w64-mingw32-g++ -std=c++17 -O2 -Wall -Wextra -Werror -static -shared \
  "$ROOT/compat/runtime/audio/acphal.cpp" "$ROOT/compat/runtime/audio/acphal.def" \
  -o "$OUT/AcpHal.dll"
echo "Built $OUT/AcpHal.dll (APU memory; SHAPE channel unsupported)."
