# Building the source snapshot

This tree supplies source, not the locally configured research installation.
There is no complete installer. Run commands from the repository root.

## Checks without game or SDK files

Python 3 is sufficient for the publication audit and synthetic parser/provenance
tests. A native C++ compiler builds the standalone APU memory test.

```bash
python3 tools/audit-public-tree.py --verify-manifest
PYTHONDONTWRITEBYTECODE=1 python3 compat/tests/test-native-shader-pack.py RecordTests
PYTHONDONTWRITEBYTECODE=1 python3 compat/tests/test-xsx-shader.py ProvenanceTests

mkdir -p compat/build/host
c++ -std=c++17 -O2 -pthread compat/tests/apu-memory-test.cpp \
  -o compat/build/host/apu-memory-test
compat/build/host/apu-memory-test
```

An independent PE export-parser test compiles its own synthetic DLL. It requires
MinGW-w64 and `llvm-readobj`, but no original game report or libraries:

```bash
PYTHONDONTWRITEBYTECODE=1 python3 compat/tools/test-inspect-game-report.py ExportFixtureTests
```

## Windows-facing adapters

Use the x86-64 MinGW-w64 C++ toolchain. Its Windows/D3D headers must cover the
interfaces referenced by the source. These adapters do not require game files
to compile:

```bash
bash compat/build-d3d12_xs.sh
bash compat/build-xmem.sh
bash compat/build-xcrdapi.sh
bash compat/build-xgr_proxy.sh
bash compat/build-audio.sh
```

Generated DLLs go under `compat/build/`; the graphics script also copies its DLL
into `tests/03-fidelityfx/`. Both the outputs and deployment are local.

XG and GameInput currently have source/export definitions but no dedicated build
scripts. They can be built using the same compiler:

```bash
mkdir -p compat/build/sys
x86_64-w64-mingw32-g++ -std=c++17 -O2 -static -shared \
  compat/runtime/xg/xg_xs.cpp compat/runtime/xg/xg_xs.def \
  -o compat/build/sys/xg_xs.dll
x86_64-w64-mingw32-g++ -std=c++17 -O2 -static -shared \
  compat/runtime/gameinput/gameinput.cpp compat/runtime/gameinput/gameinput.def \
  -o compat/build/sys/GameInput.dll
```

To build the current graphics regression executable:

```bash
mkdir -p tests/03-fidelityfx
x86_64-w64-mingw32-g++ -std=c++17 -O2 -fpermissive -static \
  compat/tests/d3d12-xs-roundtrip.cpp \
  -o tests/03-fidelityfx/d3d12-xs-roundtrip.exe
```

## Frozen 62-check graphics source

The preserved snapshot omits the concurrent device80/81 hooks and is separate
from the current graphics source. Build both its adapter and its probe together:

```bash
snapshot=compat/reports/graphics-resume-20261124/bootstrap-isolated
mkdir -p tests/08-graphics-bootstrap
x86_64-w64-mingw32-g++ -std=c++17 -O2 -shared -static \
  "$snapshot/d3d12_xs.cpp" "$snapshot/d3d12_xs.def" \
  -o tests/08-graphics-bootstrap/d3d12_xs.dll
x86_64-w64-mingw32-g++ -std=c++17 -O2 -fpermissive -static \
  "$snapshot/d3d12-xs-roundtrip.cpp" \
  -o tests/08-graphics-bootstrap/d3d12-xs-roundtrip.exe
```

For runtime tests, separately supply the matching Wine/Vulkan graphics stack,
local PC `d3d12.dll`, `d3d12core.dll`, `dxgi.dll`, and the compiler DLL used by the
probes. These dependencies are not bundled here. After preparing the directory:

```bash
XSX_GRAPHICS=1 WINEDEBUG=-all ./compat/run-test.sh \
  tests/08-graphics-bootstrap/d3d12-xs-roundtrip.exe
```

Compilation does not reproduce the historical 62/0 runtime evidence on its own.

## Earlier XMem alias regression

The earlier individual-page contract can be built from its preserved source:

```bash
snapshot=compat/reports/xmem-alias-v1
mkdir -p compat/build/xmem-alias-v1
x86_64-w64-mingw32-g++ -std=c++17 -O2 -static -shared \
  "$snapshot/xmem.cpp" "$snapshot/xmem.def" \
  -o compat/build/xmem-alias-v1/xmem.dll
x86_64-w64-mingw32-g++ -std=c++17 -O2 -static \
  "$snapshot/xmem-alias-test.cpp" \
  -o compat/build/xmem-alias-v1/xmem-alias-test.exe
```

Run in a locally configured Wine environment to check shared writes, partial
remap, and unmap. The later runtime's group-based Scatter interpretation is a
separate experiment. Its current probe source still encodes the older repeated
individual-page expectation; the historical 13/13 result does not cover v2.

## Other probes and external fixtures

| Workflow | Locally supplied inputs |
|---|---|
| Original Xbox FFX / `ffx_min` | FidelityFX API/upscaler/Xbox headers under the path used by `build-ffx_min.sh`; original DLLs for native-library tests; PC graphics DLLs |
| Complete shader frontend suite | Original FFX library at the test's expected path; game ACCC library for its separate cases |
| Complete native shader-pack suite | Private CS/VS/PS DXBC/FSSM fixtures under `compat/reports/native-shaders-20261124/`; selected compute fixture; DXC and LLVM |
| Native VS/CS GPU probes | A local supported native shader passed as the build script's first argument; DXC at `sdk/dxc/bin/dxc`; local PC graphics dependencies |
| Networking probe | GDK headers, native PC GameRuntime, original HC/XCurl/CRT DLLs, prepared prefix |
| Import audit | A locally generated LLVM COFF text report, then dependencies being audited |
| Real game | Original local executable, libraries, content, configuration, runtime, and prepared sandbox |

The full fixture-dependent Python suites intentionally retain their original
paths and assertions. They do not silently skip missing private input. Select
the synthetic classes above for source-only checks; do not interpret missing
fixture failures as parser regressions.

`build-services-probes.sh` currently builds both audio and networking and expects
the local GDK include path and DLL layout. The APU memory probe can also be built
alone, but its final channel assertion targets the historical unsupported-object
contract. The current dummy-channel implementation is not covered by that result.

Generated `native-vs-data.h` / `native-cs-data.h` and translated HLSL/DXIL derived
from game programs remain private. The build scripts generate them locally; they
are not checked into this source snapshot.

The test runner retains the development default GDK bin path under
`PGDK-extracted/`. Several scripts require that original local directory layout;
this snapshot does not install dependencies, download game data, or provide SDKs.

## Sandbox preparation

Bubblewrap, Wine, Vulkan/RADV or another suitable driver, and optional Wayland
are host dependencies. The scripts target the recorded Arch-style filesystem
layout and `/dev/dri/renderD128`; portability to other systems is not established.

Before `xsx-sandbox.sh init`, create the project-controlled directories and
synthetic account files used by its bind mounts:

```bash
mkdir -p sandbox/etc sandbox/home sandbox/game sandbox/prefix
printf 'game:x:%s:%s:Game:/home/game:/bin/bash\n' "$(id -u)" "$(id -g)" > sandbox/etc/passwd
printf 'game:x:%s:\n' "$(id -g)" > sandbox/etc/group
compat/sandbox/xsx-sandbox.sh init
```

These are local generated files, not host account-database copies. Prefix
initialization alone does not configure the native CRT/GameRuntime or game
configuration. Supply those local prerequisites separately.

Set `compat/sandbox/gamedata.path` locally to the absolute content-directory path.
That file is ignored and excluded from publication. `link-gamedata.sh` links the
read-only content mount. `deploy-game.sh` expects the private root executable,
`game_dlls/`, local graphics/runtime dependencies, and built shims.

The exported sandbox probe replaces the original hardcoded developer login with
a required compile-time definition. Set `XSX_PROBE_HOST_USER` to the host login
you actually want to check. Example using a placeholder name:

```bash
x86_64-w64-mingw32-g++ -std=c++17 -O2 -static \
  '-DXSX_PROBE_HOST_USER="host-user"' compat/tests/sandbox-probe.cpp \
  -o compat/build/sandbox-probe.exe -lws2_32
```

A result using a placeholder name does not test visibility of your actual host
account. Compile/run locally with the appropriate value for that check.

## Reading results

The public tree distinguishes source compilation, historical host/GPU results,
synthetic bootstrap behavior, and observed game progress. Use
[STATUS.md](STATUS.md) for those boundaries. Shader/PSO captures, debug dumps,
game data, and prefixes generated during these workflows stay local.
