#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
SOURCE="$ROOT/sandbox/prefix"
OUT="$ROOT/tests/07-services"
TARGET="$OUT/prefix-ready"
if [[ -f "$TARGET/.xsx-services-copy" ]]; then
  echo "[PREFIX] Reusing $TARGET"
  exit 0
fi
if [[ -e "$TARGET" ]]; then
  echo "Refusing to overwrite unmarked $TARGET" >&2
  exit 1
fi
[[ -f "$SOURCE/system.reg" && -d "$SOURCE/drive_c/windows" ]]
mkdir -p "$OUT"
COPY="$OUT/prefix-ready.copying.$$"
cp -a --reflink=auto "$SOURCE" "$COPY"
# Keep only this copy's C: and the directory containing our test executables.
# In particular, remove sandbox-only absolute paths and the game's G: mapping.
for drive in "$COPY"/dosdevices/*; do
  [[ "$(basename "$drive")" == 'c:' ]] || rm -f -- "$drive"
done
ln -s "$OUT" "$COPY/dosdevices/s:"
printf '%s\n' "$SOURCE" > "$COPY/.xsx-services-copy"
mv "$COPY" "$TARGET"
echo "[PREFIX] Copied prepared sandbox prefix to $TARGET; source unchanged."
