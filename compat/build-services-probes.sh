#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$ROOT/tests/07-services"
BUILD="$ROOT/compat/build/audio"
GDK_INCLUDE="$ROOT/PGDK-extracted/PC Development-x86_en-us/Program Files/Microsoft GDK/261000/windows/include"
mkdir -p "$OUT" "$BUILD"
CXX="${CXX_MINGW:-x86_64-w64-mingw32-g++}"
FLAGS=(-std=c++17 -O2 -Wall -Wextra -Werror -static)
"$CXX" "${FLAGS[@]}" -shared "$ROOT/compat/runtime/audio/acphal.cpp" \
  "$ROOT/compat/runtime/audio/acphal.def" -o "$BUILD/AcpHal.dll"
"$CXX" "${FLAGS[@]}" "$ROOT/compat/tests/apu-runtime-probe.cpp" -o "$OUT/apu-runtime-probe.exe"
"$CXX" "${FLAGS[@]}" -Wno-unknown-pragmas -I"$GDK_INCLUDE" \
  "$ROOT/compat/tests/network-probe.cpp" -o "$OUT/network-probe.exe" -lws2_32 -lwinhttp -ldbghelp
cp "$BUILD/AcpHal.dll" "$OUT/"
for dll in libHttpClient.GDK.dll XCurl.dll msvcp140.dll vcruntime140.dll vcruntime140_1.dll; do
  cp "$ROOT/game_dlls/$dll" "$OUT/"
done
for dll in xgameruntime.dll xgameruntime.thunks.dll; do
  cp "$ROOT/tests/01-runtime/$dll" "$OUT/"
done
cp "$ROOT/compat/tests/services-MicrosoftGame.Config" "$OUT/MicrosoftGame.Config"
echo "Built APU and network probes in $OUT; game deployment unchanged."
