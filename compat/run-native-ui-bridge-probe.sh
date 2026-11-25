#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$ROOT/tests/17-native-ui-bridge"
rc=0
# Probe encodes draw-time snapshot semantics (mutates guest bytes before submit);
# the game uses submit-time refresh (Xbox GPU reads memory at execution).
XSX_DEV_GFX_REFRESH=0 XSX_GRAPHICS=1 WINEDEBUG=-all timeout 120s "$ROOT/compat/run-test.sh" \
  "$OUT/native-ui-bridge-probe.exe" >"$OUT/readback.log" 2>&1 || rc=$?
rg --no-line-number '\[(BRIDGE STAGE|BRIDGE PSO|BRIDGE GPU|PASS|FAIL|HRESULT)\]' "$OUT/readback.log" || true
printf '[EXIT] native-ui-bridge rc=%d\n' "$rc"
if ((rc)); then tail -n 12 "$OUT/readback.log"; fi
exit "$rc"
