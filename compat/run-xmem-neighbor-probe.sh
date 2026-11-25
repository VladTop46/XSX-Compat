#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$ROOT/tests/18-xmem-neighbors"
rc=0
XSX_GRAPHICS=0 WINEDEBUG=-all timeout 60s "$ROOT/compat/run-test.sh" \
  "$OUT/xmem-neighbor-probe.exe" >"$OUT/readback.log" 2>&1 || rc=$?
rg --no-line-number '\[(PASS|FAIL)\]' "$OUT/readback.log" || true
printf '[EXIT] xmem-neighbor rc=%d\n' "$rc"
if ((rc)); then tail -n 12 "$OUT/readback.log"; fi
exit "$rc"
