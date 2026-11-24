#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$ROOT/tests/07-services"
bash "$ROOT/compat/prepare-services-prefix.sh"
export WINEPREFIX="$OUT/prefix-ready"
export WINEDEBUG="${WINEDEBUG:--all}"
export WINEDLLOVERRIDES="mscoree,mshtml=;msvcp140,vcruntime140,vcruntime140_1,xgameruntime=n"
mkdir -p "$OUT/logs"
case "${1:-all}" in
  all) probes=(apu-runtime-probe network-probe) ;;
  network) probes=(network-probe) ;;
  audio) probes=(apu-runtime-probe) ;;
  *) echo "Usage: $0 [all|network|audio]" >&2; exit 2 ;;
esac
status=0
for probe in "${probes[@]}"; do
  code=0
  timeout 60 wine "S:\\$probe.exe" > "$OUT/logs/$probe.log" 2>&1 || code=$?
  cat "$OUT/logs/$probe.log"
  echo "[EXIT] $probe rc=$code"
  if (( code != 0 )); then status=1; fi
done
exit "$status"
