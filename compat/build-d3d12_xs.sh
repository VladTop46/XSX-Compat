#!/usr/bin/env bash
# Сборка d3d12_xs.dll (обязательно -static, иначе Wine не найдёт libwinpthread-1.dll)
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p build ../tests/03-fidelityfx
x86_64-w64-mingw32-g++ -std=c++17 -O2 -shared -static d3d12_xs/d3d12_xs.cpp d3d12_xs/d3d12_xs.def -o build/d3d12_xs.dll
cp build/d3d12_xs.dll ../tests/03-fidelityfx/d3d12_xs.dll
