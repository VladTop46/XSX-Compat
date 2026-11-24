#!/usr/bin/env bash
# Прокси xgameruntime.dll: проброс в настоящий runtime (system32) + собственный провайдер XPackage.
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$ROOT/compat/build/xgr"; mkdir -p "$OUT"
x86_64-w64-mingw32-g++ -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -static -shared \
  "$ROOT/compat/runtime/xgr_proxy/xgr_proxy.cpp" "$ROOT/compat/runtime/xgr_proxy/xgr_proxy.def" \
  -o "$OUT/xgameruntime.dll"
echo "Built $OUT/xgameruntime.dll (proxy -> system32 real runtime; XPackage provider)."
