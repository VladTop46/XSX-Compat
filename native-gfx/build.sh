#!/usr/bin/env bash
# native-gfx branch: xsxgpu unixlib (PE builtin + .so) and probe.
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p build/x86_64-windows build/x86_64-unix build/inc/wine
ln -sf /usr/include/wine/unixlib.h build/inc/wine/unixlib.h
gcc -shared -fPIC -O2 -Wall -Wno-misleading-indentation -DWINE_UNIX_LIB -I/usr/include/wine/windows -I/usr/include/wine -I/usr/include/libdrm -I src \
  src/xsxgpu_unix.c src/exp2_core.c src/phys_unix.c src/gpu_unix.c -ldrm_amdgpu -ldrm -lm -o build/x86_64-unix/xsxgpu.so
winegcc -b x86_64-w64-mingw32 -shared -O2 -I src -I build/inc src/xsxgpu.c src/xsxgpu.spec -o build/x86_64-windows/xsxgpu.dll -lntdll
winebuild --builtin build/x86_64-windows/xsxgpu.dll
x86_64-w64-mingw32-gcc -O2 -Wall src/probe.c -o build/probe.exe
x86_64-w64-mingw32-gcc -O2 -Wall src/phys_probe.c -o build/phys_probe.exe
sha256sum build/x86_64-windows/xsxgpu.dll build/x86_64-unix/xsxgpu.so build/probe.exe
