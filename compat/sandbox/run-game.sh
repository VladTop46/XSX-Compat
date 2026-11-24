#!/usr/bin/env bash
# Запуск игры в песочнице с логом и разбором падения.
# Использование: compat/sandbox/run-game.sh [таймаут_сек=240] [-- доп. переменные окружения в виде VAR=val ...]
# Примеры:  compat/sandbox/run-game.sh
#           compat/sandbox/run-game.sh 120 XSX_DEV_TRACE=1
#           WINEDEBUG=err+all,+file compat/sandbox/run-game.sh 90
# Окно появится на вашем Wayland-экране; лог пишется в compat/logs/sandbox/game-<время>.log (и выводится в конце сводка).
set -uo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"; cd "$ROOT"
T="${1:-240}"; shift || true
for kv in "$@"; do [[ "$kv" == *=* ]] && export "$kv"; done
LOG="compat/logs/sandbox/game-$(date +%H%M%S).log"; mkdir -p compat/logs/sandbox
# Preserve diagnostic capture directories (e.g. native-pso); remove only old
# top-level crash/dump files and links, as the previous rm -f intended.
for tmp_file in sandbox/home/T/*; do
  if [[ -f "$tmp_file" || -L "$tmp_file" ]]; then rm -f -- "$tmp_file"; fi
done
compat/sandbox/deploy-game.sh >/dev/null 2>&1                      # актуальные shim'ы из compat/build
echo "[run] лог: $LOG  (таймаут ${T}s, WINEDEBUG=${WINEDEBUG:-err+all})"
WINEDEBUG="${WINEDEBUG:-err+all}" timeout "$T" compat/sandbox/xsx-sandbox.sh run 'G:\game_scarlett_master.exe' > "$LOG" 2>&1
echo "[run] выход: $?"
echo "--- shim-логи (последние 25) ---"; grep -a "^\[\(D3D12_XS\|XCRDAPI\|XMem\|GameInput\|FFX_MIN\|STUB\)\|UNMAPPED" "$LOG" | cut -c1-200 | tail -25
for f in sandbox/home/T/*.dmp; do [ -e "$f" ] && { echo "--- падение игры ---"; python3 -I compat/tools/minidump-info.py "$f" | sed -n 2,3p | cut -c1-200; }; done
ls sandbox/home/T/*.crash.log >/dev/null 2>&1 && echo "crash-лог игры: $(ls sandbox/home/T/*.crash.log | head -1)"
