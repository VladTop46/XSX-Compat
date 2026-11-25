#!/usr/bin/env bash
# Linux x86-64: real SIGTRAPs and the same Resume core used by Windows VEH.
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
task_tmp="$(mktemp -d "${TMPDIR:-/tmp}/xsx-task-trace.XXXXXX")"
trap 'rm -rf -- "$task_tmp"' EXIT
python3 - "$ROOT/compat/tests/task-trace-probe.cpp" "$task_tmp/fixture.bin" <<'PY'
from pathlib import Path
import re, struct, sys
source = Path(sys.argv[1]).read_text()
code = bytearray(128)
for name, offset in [('wrapper', 0), ('update', 64)]:
    contents = source.split('const unsigned char ' + name + '[]{', 1)[1].split('};', 1)[0]
    contents = re.sub(r'//[^\n]*', '', contents)
    data = bytes(int(n, 0) for n in re.findall(r'0x[0-9a-fA-F]+|\b[0-9]+\b', contents))
    assert offset + len(data) <= len(code)
    code[offset:offset + len(data)] = data
struct.pack_into('<i', code, 30, 64 - 34)
for offset, opcode in [(5, '4889c8'), (27, '7505'), (65, '4889ce'), (76, '4889f1')]:
    expected = bytes.fromhex(opcode)
    assert code[offset:offset + len(expected)] == expected
Path(sys.argv[2]).write_bytes(code)
PY
g++ -std=c++17 -O2 -pthread -Wall -Wextra -Werror \
    "$ROOT/compat/tests/task-trace-posix.cpp" -o "$task_tmp/optimized"
"$task_tmp/optimized" "$task_tmp/fixture.bin"
g++ -std=c++17 -O2 -g -pthread -Wall -Wextra -Werror -fno-omit-frame-pointer \
    -fsanitize=address,undefined "$ROOT/compat/tests/task-trace-posix.cpp" -o "$task_tmp/sanitized"
ASAN_OPTIONS=detect_leaks=0 "$task_tmp/sanitized" "$task_tmp/fixture.bin"
