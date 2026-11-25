#!/usr/bin/env bash
# Сборка форка d3d12_xs.dll из локальной копии (корневой compat/ не участвует).
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p build
x86_64-w64-mingw32-g++ -std=c++17 -O2 -shared -static \
  src/d3d12_xs/d3d12_xs.cpp src/d3d12_xs/d3d12_xs.def -o build/d3d12_xs.dll
sha256sum build/d3d12_xs.dll
