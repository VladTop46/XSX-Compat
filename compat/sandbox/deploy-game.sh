#!/usr/bin/env bash
# Раскладка sandbox/game: exe + оригинальные игровые DLL + наши shim'ы. Хост-пути в песочницу не попадают (только копии).
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"; G="$ROOT/sandbox/game"; mkdir -p "$G"
cp -n "$ROOT/game_scarlett_master.exe" "$G/"                       # exe копируется один раз (-n); не затирает
# оригинальные DLL игры, кроме FFX (loader заменяет ffx_min, upscaler не нужен)
for f in "$ROOT"/game_dlls/*.dll; do b=$(basename "$f"); case "$b" in amd_fidelityfx_*) continue;; esac; cp -f "$f" "$G/"; done
# графика (VKD3D-Proton / DXVK, как в tests/03-fidelityfx)
for f in d3d12.dll d3d12core.dll dxgi.dll PIXEvt.dll; do cp -f "$ROOT/tests/03-fidelityfx/$f" "$G/"; done
# shim'ы и заглушки
cp -f "$ROOT/compat/build/d3d12_xs.dll" "$G/"
cp -f "$ROOT"/compat/build/sys/*.dll "$G/"
cp -f "$ROOT/compat/build/ffx_min/amd_fidelityfx_loader_xbox.dll" "$G/"
# GameRuntime (PC); XCRDAPI — наш shim (из compat/build/sys)
cp -f "$ROOT/tests/01-runtime/xgameruntime.dll" "$G/"
ls -la "$G" | sed -n 1,60p
# GameRuntime грузится игрой иногда только из System32 (LoadLibraryExW flags 0x800) -> копия и в префикс
cp -f "$ROOT/tests/01-runtime/xgameruntime.dll" "$ROOT/sandbox/prefix/drive_c/windows/system32/xgameruntime.dll"
# Прокси xgameruntime (XPackage-провайдер) поверх настоящего runtime в system32: игра грузит G:\xgameruntime.dll первым.
# Отключить: XSX_XGR_PROXY=0 compat/sandbox/run-game.sh
if [[ "${XSX_XGR_PROXY:-1}" != 0 && -f "$ROOT/compat/build/xgr/xgameruntime.dll" ]]; then cp -f "$ROOT/compat/build/xgr/xgameruntime.dll" "$G/xgameruntime.dll"; fi
