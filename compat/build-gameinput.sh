#!/usr/bin/env bash
# GameInput.dll shim with a keyboard-driven virtual Xbox gamepad.
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"; OUT="$ROOT/compat/build/sys"; mkdir -p "$OUT"
x86_64-w64-mingw32-g++ -std=c++17 -O2 -static -shared -Wall -Wno-unused-parameter "$ROOT/compat/runtime/gameinput/gameinput.cpp" "$ROOT/compat/runtime/gameinput/gameinput.def" -o "$OUT/GameInput.dll"
sha256sum "$OUT/GameInput.dll"
