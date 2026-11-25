#!/usr/bin/env bash
# Use the game's existing sandbox/prefix with an isolated null playback PCM.
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
LOG="$ROOT/tests/15-audio-output/audio-output.log"
cp "$ROOT/tests/15-audio-output/audio-output-probe.exe" "$ROOT/sandbox/game/"
set +e
XSX_SB_GUI=0 XSX_SB_AUDIO=null WINEDEBUG="${WINEDEBUG:--all}" \
  timeout 40 "$ROOT/compat/sandbox/xsx-sandbox.sh" run 'G:\audio-output-probe.exe' 2>&1 | tee "$LOG"
result=${PIPESTATUS[0]}
set -e
echo "[EXIT] audio-output-probe rc=$result"
exit "$result"
