#!/usr/bin/env bash
# Запуск игры под winedbg внутри песочницы: cont -> остановка на первом исключении -> команды из файла (по одной на строку).
# Использование: dbg-int3.sh <файл-команд-на-хосте>   (копируется в sandbox/game/_dbg.cmd)
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
{ [ -z "${DBG_RAW:-}" ] && echo cont; cat "$1"; echo quit; } > "$ROOT/sandbox/game/_dbg.cmd"
"$ROOT/compat/sandbox/xsx-sandbox.sh" raw /usr/bin/bash -c 'wine winedbg --file G:\\_dbg.cmd G:\\game_scarlett_master.exe; wineserver -w'
