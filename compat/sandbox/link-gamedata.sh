#!/usr/bin/env bash
# Симлинки в sandbox/game на /gamedata/* (read-only mount каталога загрузки; на хосте ссылки «висячие» — это нормально).
# Не трогает DLL, exe и xvd (их кладёт deploy-game.sh / не нужны).
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"; SRC="$(cat "$ROOT/compat/sandbox/gamedata.path")"; G="$ROOT/sandbox/game"
for p in "$SRC"/*; do
  n="$(basename "$p")"
  case "$n" in *.dll|*.exe|*.xvd) continue;; esac
  [[ -e "$G/$n" && ! -L "$G/$n" ]] && { cmp -s "$p" "$G/$n" 2>/dev/null && rm -f "$G/$n" || { echo "SKIP (отличается от оригинала): $n"; continue; }; }
  ln -sfn "/gamedata/$n" "$G/$n"
done
ls -la "$G" | grep -- '->' | awk '{print $9,$10,$11}' | head -50
