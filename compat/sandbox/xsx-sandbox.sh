#!/usr/bin/env bash
# Изолированный запуск Wine для game_scarlett_master.exe.
# Контракт изоляции (проверяется compat/sandbox/verify.sh):
#   - хостовые / и ~ НЕ видны: корень — пустой tmpfs; смонтированы только
#       /usr (ro, нужен Wine/Mesa), /sys (ro, для Vulkan), /proc, /dev (минимальный + render-node GPU),
#       sandbox/game -> /game (rw), sandbox/prefix -> /prefix (rw), sandbox/home -> /home/game (rw),
#       выбранные /etc файлы (ro) и ОДИН сокет Wayland;
#   - сеть отключена (--unshare-net), PID/IPC/UTS/cgroup отдельные, --new-session (нет TIOCSTI), --die-with-parent;
#   - переменные окружения хоста сброшены (--clearenv);
#   - в Wine-префиксе остаются только диски C: (prefix) и G: (game); Z:/E:/D: удалены.
# Явно ВИДНО игре: содержимое хостового /usr (read-only), версия GPU/железа через /sys и каталог загрузки игры (read-only, /gamedata).
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
SB="$ROOT/sandbox"
UIDN="$(id -u)"

bw() {
  local args=(
    --unshare-all --die-with-parent --new-session
    --clearenv
    --ro-bind /usr /usr
    --symlink usr/lib /lib --symlink usr/lib /lib64 --symlink usr/bin /bin --symlink usr/bin /sbin
    --ro-bind /etc/ld.so.cache /etc/ld.so.cache
    --ro-bind /etc/fonts /etc/fonts
    --ro-bind "$SB/etc/passwd" /etc/passwd --ro-bind "$SB/etc/group" /etc/group
    --proc /proc --dev /dev
    --dev-bind /dev/dri/renderD128 /dev/dri/renderD128
    --ro-bind /sys /sys
    --tmpfs /tmp --dir "/run/user/$UIDN"
    --bind "$SB/home" /home/game
    --bind "$SB/prefix" /prefix
    --bind "$SB/game" /game
    --setenv HOME /home/game --setenv USER game --setenv LOGNAME game
    --setenv PATH /usr/bin
    --setenv LANG "${LANG:-C.UTF-8}"
    --setenv WINEPREFIX /prefix --setenv WINEARCH win64
    --setenv XDG_RUNTIME_DIR "/run/user/$UIDN"
    --setenv WINEDEBUG "${WINEDEBUG:--all}"
    --setenv WINEDLLOVERRIDES "${WINEDLLOVERRIDES:-mscoree,mshtml=;d3d12,d3d12core,dxgi,xgameruntime=n}"
    --chdir /game
  )
  # Каталог загрузки игры (ТОЛЬКО ЧТЕНИЕ) -> /gamedata; путь в compat/sandbox/gamedata.path. Единственное расширение видимости хоста, согласовано с пользователем.
  if [[ -f "$ROOT/compat/sandbox/gamedata.path" ]]; then
    GD="$(cat "$ROOT/compat/sandbox/gamedata.path")"
    [[ -d "$GD" ]] && args+=(--ro-bind "$GD" /gamedata)
  fi
  # Графика: только сокет Wayland; X11 не пробрасывается.
  if [[ "${XSX_SB_GUI:-1}" == 1 && -n "${XDG_RUNTIME_DIR:-}" && -S "${XDG_RUNTIME_DIR}/${WAYLAND_DISPLAY:-wayland-1}" ]]; then
    args+=(--bind "$XDG_RUNTIME_DIR/${WAYLAND_DISPLAY:-wayland-1}" "/run/user/$UIDN/${WAYLAND_DISPLAY:-wayland-1}"
           --setenv WAYLAND_DISPLAY "${WAYLAND_DISPLAY:-wayland-1}")
  fi
  # Передача выбранных XSX_* переменных (только они).
  for v in $(compgen -e | grep '^XSX_FFX_\|^XSX_VM_\|^XSX_GRAPHICS$\|^XSX_DEV_\|^XSX_DEVICE_\|^XSX_TRAP$\|^XSX_XGR_\|^XSX_DS_\|^XSX_XMEM_'); do args+=(--setenv "$v" "${!v}"); done
  exec bwrap "${args[@]}" "$@"
}

case "${1:-}" in
  init)   # создать префикс внутри песочницы и вычистить диски
    mkdir -p "$SB/prefix" "$SB/home" "$SB/game"
    ( XSX_SB_GUI=0 bw /usr/bin/bash -c 'wine wineboot --init; wineserver -w' ) 2>&1 | tail -5 || true
    for l in "$SB"/prefix/dosdevices/*; do
      n="$(basename "$l")"; [[ "$n" == "c:" ]] || rm -f "$l"
    done
    ln -s ../../game "$SB/prefix/dosdevices/g:"   # относительная ссылка внутри prefix: -> /game в песочнице
    mkdir -p "$SB/home/T"; ln -sfn /home/game/T "$SB/prefix/dosdevices/t:"   # T: = temp-диск Xbox (crash.log, dumps); цель существует только внутри песочницы
    echo "dosdevices:"; ls -la "$SB/prefix/dosdevices" ;;
  run)    shift; bw /usr/bin/bash -c 'wine "$@"; rc=$?; wineserver -w; exit $rc' _ "$@" ;;   # wineserver -w: дать Wine сохранить реестр до завершения песочницы
  raw)    shift; bw "$@" ;;      # произвольная команда внутри песочницы (для проверки)
  *) echo "usage: $0 init | run <exe> [args] | raw <cmd>"; exit 2 ;;
esac
