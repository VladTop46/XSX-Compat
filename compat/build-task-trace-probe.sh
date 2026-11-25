#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="$ROOT/tests/14-writer-task"
mkdir -p "$DEST"
x86_64-w64-mingw32-g++ -std=c++17 -O2 -static -Wall -Wextra -Werror \
    "$ROOT/compat/tests/task-trace-probe.cpp" -o "$DEST/task-trace-probe.exe"
cp "$ROOT/compat/build/d3d12_xs.dll" "$DEST/"
