#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$ROOT/tests/15-audio-output"
mkdir -p "$OUT"
x86_64-w64-mingw32-g++ -std=c++17 -O2 -static -Wall -Wextra -Werror \
  "$ROOT/compat/tests/audio-output-probe.cpp" -lole32 -luuid -o "$OUT/audio-output-probe.exe"
echo "Built $OUT/audio-output-probe.exe"
