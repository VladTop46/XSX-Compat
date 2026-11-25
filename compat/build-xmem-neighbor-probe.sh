#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$ROOT/tests/18-xmem-neighbors"
mkdir -p "$OUT"
x86_64-w64-mingw32-g++ -std=c++17 -O2 -static -Wall -Wextra -Werror \
  "$ROOT/compat/tests/xmem-neighbor-probe.cpp" -o "$OUT/xmem-neighbor-probe.exe"
cp "$ROOT/compat/build/sys/xmem.dll" "$OUT/xmem.dll"
sha256sum "$OUT/xmem-neighbor-probe.exe" "$OUT/xmem.dll"
