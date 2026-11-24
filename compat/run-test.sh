#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
GDK_BIN="$ROOT/PGDK-extracted/PC Development-x86_en-us/Program Files/Microsoft GDK/261000/windows/bin/x64"

export WINEPREFIX="${WINEPREFIX:-$HOME/.wine-xsx-research}"
export WINEPATH="$GDK_BIN"
export WINEDEBUG="${WINEDEBUG:--all}"

# По умолчанию подключаем только GameRuntime.
# Графический стек включается отдельно.
case "${XSX_GRAPHICS:-0}" in
    0)
        export WINEDLLOVERRIDES="xgameruntime=n"
        ;;
    1)
        export WINEDLLOVERRIDES="xgameruntime,dxgi,d3d12,d3d12core=n"
        ;;
    *)
        echo "XSX_GRAPHICS must be 0 or 1" >&2
        exit 2
        ;;
esac

if (( $# == 0 )); then
    echo "Usage: $0 executable.exe [args...]"
    exit 2
fi

exec wine "$@"
