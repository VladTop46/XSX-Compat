#!/usr/bin/env bash
# Deploy the isolated graphics candidate after normal dependencies; main build is untouched.
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
SECONDS_LIMIT="${1:-90}"
[[ "$SECONDS_LIMIT" =~ ^[0-9]+$ ]] || { echo 'Timeout must be a non-negative integer (0 = no timeout)' >&2; exit 2; }
PROBE_DLL="$ROOT/tests/17-native-ui-bridge/d3d12_xs.dll"
CANDIDATE_DLL="$ROOT/local-agent-work/build/d3d12_xs.dll"
# XSX_RUN_DLL=<path>: run an explicit DLL (bisecting/baselines) instead of the probe-checked candidate.
if [[ -n "${XSX_RUN_DLL:-}" ]]; then CANDIDATE_DLL="$XSX_RUN_DLL"
else cmp -s "$PROBE_DLL" "$CANDIDATE_DLL" || { echo 'Candidate and probe DLL differ; rebuild the probe first' >&2; exit 2; }; fi
bash "$ROOT/compat/sandbox/deploy-game.sh" >/dev/null
cp "$CANDIDATE_DLL" "$ROOT/sandbox/game/d3d12_xs.dll"
mkdir -p "$ROOT/sandbox/game/shader-cache" "$ROOT/compat/logs/sandbox"
cp "$ROOT"/tests/17-native-ui-bridge/shader-cache/* "$ROOT/sandbox/game/shader-cache/"
LOG="$ROOT/compat/logs/sandbox/game-native-ui-$(date +%H%M%S).log"
unset XSX_DEV_NATIVE_MINIMAL
export XSX_SB_AUDIO=null XSX_DEV_NATIVE_GFX=1 XSX_DEV_NOOP_PSO=1
export XSX_DEV_SHADER_CACHE='G:\shader-cache' XSX_DEV_GFX_RTV=28 XSX_TRACE_RETURNS=1
# Present snapshots (PPM) for pixel inspection: sandbox/game/snapshots-HHMMSS.
SNAPTAG="snapshots-$(date +%H%M%S)"
export XSX_PRESENT_SNAPSHOT="${XSX_PRESENT_SNAPSHOT:-120}" XSX_PRESENT_SNAPSHOT_DIR="G:\\$SNAPTAG"
printf '[run] снимки: sandbox/game/%s (каждый %s кадр); маркер XSX_DEV_GFX_MARK=%s\n' "$SNAPTAG" "$XSX_PRESENT_SNAPSHOT" "${XSX_DEV_GFX_MARK:-0}"
if [[ -n "${XSX_DEV_VIEW_TRACE_DIR:-}" ]]; then
  export XSX_DEV_VIEW_TRACE_DIR
  printf '[run] descriptor trace: %s\n' "$XSX_DEV_VIEW_TRACE_DIR"
fi
printf '[run] candidate: '
sha256sum "$CANDIDATE_DLL" | cut -d' ' -f1
printf '[run] лог: %s\n' "${LOG#"$ROOT/"}"
rc=0
TIMEOUT=(timeout "$SECONDS_LIMIT"); ((SECONDS_LIMIT)) || TIMEOUT=()
WINEDEBUG="${WINEDEBUG:-err+all}" "${TIMEOUT[@]}" \
  "$ROOT/compat/sandbox/xsx-sandbox.sh" run 'G:\game_scarlett_master.exe' >"$LOG" 2>&1 || rc=$?
printf '[run] выход: %d\n' "$rc"
rg --no-line-number 'GFXNATIVE.*(PSO из кэша|input layout rejected|unsupported fixed|CreateGraphicsPipelineState|RT50|DRAW|T#|V#|PRESENTSRC|VIEWPORT|SCISSOR)|SNAPSHOT|basic_string|terminate called|FIRST-CHANCE' "$LOG" | tail -n 35 || true
echo '[run] --- black-frame диагностика ---'
rg --no-line-number 'PRESENTSRC|VIEWPORT21|SCISSOR22|SNAPSHOT|DRAW ready n=[1-4] |DRAWARGS|T# resource|REFRESH|RECTLIST|skipped'  "$LOG" | head -n 80 || true
exit "$rc"
