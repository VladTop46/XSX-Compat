#!/usr/bin/env bash
# Сборка минимальной замены amd_fidelityfx_loader_xbox.dll -> compat/build/ffx_min/
set -euo pipefail
cd "$(dirname "$0")/.."
FFX="$PWD/sdk/Xbox-GDK-Samples/Kits/FidelityFX"
mkdir -p compat/build/ffx_min
x86_64-w64-mingw32-g++ -std=c++17 -O2 -shared -static -fpermissive \
  -I"$FFX/api/include" -I"$FFX/upscalers/include" -I"$FFX/api/include/xbox" \
  compat/ffx_min/ffx_min.cpp -o compat/build/ffx_min/amd_fidelityfx_loader_xbox.dll
