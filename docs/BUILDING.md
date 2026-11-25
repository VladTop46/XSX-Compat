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

Use the x86-64 MinGW-w64 C and C++ toolchains. Its Windows/D3D headers must cover the
interfaces referenced by the source. These adapters do not require game files
to compile:

```bash
bash compat/build-d3d12_xs.sh
bash compat/build-xmem.sh
bash compat/build-xcrdapi.sh
bash compat/build-xgr_proxy.sh
bash compat/build-audio.sh
```

The storage build compiles the bundled miniz C source with `x86_64-w64-mingw32-gcc` before linking the adapter.

Generated DLLs go under `compat/build/`; the graphics script also copies its DLL
into `tests/03-fidelityfx/`. Both the outputs and deployment are local.

GameInput has its own builder: `bash compat/build-gameinput.sh`. XG currently
has source/export definitions without a dedicated build script. They can be built using the same compiler:

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
remap, and unmap. This source is historical. The current runtime uses PFN IDs
in steps of 16 and literal Scatter identifiers; its updated probe records 16/16.
Build that probe from `compat/tests/xmem-alias-test.cpp` against the current
`xmem.dll` to check the newer contract.

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
contract. The current command/message FIFO is not covered by that result.

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


## November26 additions: three graphics builds

The main adapter, VKD3D graphics fork and direct AMDGPU branch remain separate:

```bash
bash compat/build-d3d12_xs.sh
bash local-agent-work/build-dll.sh
bash native-gfx/build.sh
bash compat/build-gameinput.sh
```

The fork builder creates `local-agent-work/build/`. Native graphics builds Unix `.so`, builtin PE and two probes under `native-gfx/build/`; dependencies are Wine development tools/headers and libdrm_amdgpu/libdrm in addition to GCC and MinGW. It does not supply shader inputs or initialize a Wine prefix. See [NATIVE_GRAPHICS.md](NATIVE_GRAPHICS.md).

Additional portable checks without game/SDK fixtures:

```bash
mkdir -p compat/build/host
c++ -std=c++17 -O2 -pthread compat/tests/frame-clock-test.cpp -o compat/build/host/frame-clock
compat/build/host/frame-clock
c++ -std=c++17 -O2 -pthread compat/tests/return-trace-test.cpp -o compat/build/host/return-trace
compat/build/host/return-trace
c++ -std=c++17 -O2 compat/tests/guest-footprint-test.cpp -o compat/build/host/footprint
compat/build/host/footprint
c++ -std=c++17 -O2 compat/tests/gfx-buffer-descriptor-cpu.cpp -o compat/build/host/buffer-descriptor
compat/build/host/buffer-descriptor
bash compat/run-task-trace-cpu.sh
```

`gfx-input-layout-cpu.cpp` and `gfx-pso-description-cpu.cpp` require private captured fixtures; their source is included, the fixtures are not. The historical Linux physical-view model predates the optional native BO hook and may need its mock declarations updated for the new header. The new Windows neighbor probe and direct BO probe are separate tests.

Additional probe builders cover frame events, resource127/title fallback, buffer127, SurfaceBlit, audio output, UI readback and the selected-program bridge. Builders that copy local PC DLLs or synthesize captured fixture headers require those private dependencies first; their absence in this export is intentional. Build UI readback before the bridge so the frozen `tests/16-ui-shaders` program manifest exists.

For the prepared main game sandbox, retain the working null endpoint prerequisite:

```bash
compat/sandbox/run-game.sh 90 XSX_SB_AUDIO=null XSX_DEV_NOOP_PSO=1 XSX_TRACE_RETURNS=1
```

`XSX_SB_AUDIO=null` exposes the authored ALSA null-playback config without host audio sockets/capture. It discards sound while supporting the tested WASAPI/spatial initialization. It does not implement the game's DSP/codec work.

The fork runner uses `XSX_DEV_NATIVE_GFX=1`, the private translated cache and `tests/17-native-ui-bridge` candidate. This differs from direct AMDGPU flags `XSX_SB_NATIVE_GFX=1` (sandbox bind) and `XSX_NATIVE_GFX=1` (XMem BO hook). Those native hooks are opt-in. Newer game logs show native XMem activation and successful physical-destination storage; dedicated M2/readback coverage is still pending.

Original FFX regression, with local headers/libraries and PC graphics dependencies:

```bash
XSX_GRAPHICS=1 WINEDEBUG=-all ./compat/run-test.sh tests/03-fidelityfx/ffx-context.exe --xbox 3 --heap-abi
```

The historical failure remains native PSO (`0x8000000D`, process8). A failure before test entry is an environment failure, not a new FFX result. The selected translation bridge probe uses `XSX_DEV_GFX_REFRESH=0` to retain its draw-time snapshot contract; the game uses submit-time refresh by default. Do not compare those as equivalent coherence tests.

The fork also carries `XSX_NO_WATCHDOG=1` as an opt-in debugging flag. It changes a game flag to bypass the watchdog; leave it unset for ordinary stability evidence. Source synchronization does not enable it.
