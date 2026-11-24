#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
REPORT="${1:-$ROOT/exe-report.txt}"

python3 "$ROOT/compat/tools/inspect-game-report.py" "$REPORT" --root "$ROOT" \
    --out "$ROOT/tests/05-game/target-profile.json" \
    --probe-header "$ROOT/tests/05-game/game-imports.generated.h"
x86_64-w64-mingw32-g++ -std=c++17 -O2 -static -Wall -Wextra -municode \
    -I"$ROOT/tests/05-game" "$ROOT/compat/tests/game-import-probe.cpp" \
    -o "$ROOT/tests/05-game/game-import-probe.exe"
