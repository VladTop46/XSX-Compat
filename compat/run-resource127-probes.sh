#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="$ROOT/tests/11-resource127"
mkdir -p "$DEST/logs"
cd "$ROOT"
result=0
for name in resource127-probe resource127-fallback-probe; do
    rc=0
    XSX_GRAPHICS=1 WINEDEBUG=-all timeout 45s "$ROOT/compat/run-test.sh" \
        "$DEST/$name.exe" >"$DEST/logs/$name.log" 2>&1 || rc=$?
    rg --no-line-number '\[(XMEM|RESOURCE127|GPU|PASS|FAIL)\]' "$DEST/logs/$name.log" || true
    printf '[EXIT] %s rc=%d\n' "$name" "$rc"
    if (( rc != 0 )); then
        result=1
        tail -n 8 "$DEST/logs/$name.log"
    fi
done
exit "$result"
