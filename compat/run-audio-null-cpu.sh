#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
audio_tmp="$(mktemp -d "${TMPDIR:-/tmp}/xsx-audio-null.XXXXXX")"
trap 'rm -rf -- "$audio_tmp"' EXIT
g++ -std=c++17 -O2 -Wall -Wextra -Werror "$ROOT/compat/tests/alsa-null-probe.cpp" -lasound -o "$audio_tmp/alsa-null-probe"
"$audio_tmp/alsa-null-probe" "$ROOT/compat/sandbox/audio-null.conf"
