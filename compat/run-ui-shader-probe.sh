#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$ROOT/tests/16-ui-shaders"
set +e
XSX_GRAPHICS=1 WINEDEBUG="${WINEDEBUG:--all}" \
  timeout 120 "$ROOT/compat/run-test.sh" "$OUT/ui-shader-readback.exe" 2>&1 | tee "$OUT/readback.log"
result=${PIPESTATUS[0]}
set -e
echo "[EXIT] ui-shader-readback rc=$result"
exit "$result"
