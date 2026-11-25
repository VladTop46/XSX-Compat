# Direct RDNA2 graphics branch

The November25 experiments executed original Xbox game ISA on a Navi21/gfx1030 host through libdrm_amdgpu and raw PM4. This is now a separate research direction alongside the VKD3D selected-translation fork.

## What executed

E1 uses authored compute assembly (`store.s`), identity CPU/GPU virtual addresses and a COMPUTE ring dispatch. E2 executes the game's wrapped blit VS and color PS, an authored fetch subroutine and hardware RECTLIST. E3 executes its textured pair, matrices supplied through two V# buffers, an explicit linear T# and the shader's inline sampler. E2/E3 each produced4096/4096 exact pixels; shifting E3 by eight pixels made the oracle fail as expected.

These experiments use original ISA loaded from external files. No original ISA, metadata, translated shader cache or PSO captures are distributed. The public extractor still needs the private archive manifest at its documented development path, in addition to `shaders.rpf`.

## Source and memory boundary

| Component | Role |
|---|---|
| `compat/tests/native-isa/` | Native Linux PM4 probes, extractor and authored compute/fetch assembly |
| `native-gfx/src/xsxgpu.c` | Wine PE exports forwarding fixed structs through Wine unix calls |
| `xsxgpu_unix.c` / `exp2_core.c` | Direct AMDGPU open and E2 draw in the Wine process |
| `phys_unix.c` | GTT BO chunks,64KiB physical pages, matching CPU/GPU mappings and aliases |
| `gpu_unix.c` / `phys_probe.c` | Authored compute writes and alias-readback probe |
| `compat/runtime/xmem/phys.h` | Opt-in `XSX_NATIVE_GFX=1` integration hook; ordinary section path remains default |

Userptr works for the demonstrated private anonymous allocations, not shared XMem sections or file mappings. The alternative uses GTT BO-backed mappings. M2 source overlays Wine reservations with fixed mmaps; Wine's bookkeeping is documented as unaware of the overlays. Source and probe inclusion do not prove complete Wine memory-manager or game compatibility.

## Build and local fixtures

`bash native-gfx/build.sh` requires GCC, libdrm headers/libraries, Wine development headers, `winegcc`, `winebuild`, and MinGW-w64. It creates PE and Unix libraries plus two probes under `native-gfx/build/`.

A local run installs the builtin PE DLL in the selected scratch prefix's `drive_c/windows/system32/`, sets `WINEDLLPATH` to the absolute native build directory and `XSXGPU_DATA` to the local experiment data directory. The draw probe reserves its own private memory. Data must include the matching `isa/` programs and `fetch.bin`; see `exp2_core.c` for exact filenames. No prefix initialization or fixture download is part of the source export.

The sandbox source has an explicit `XSX_SB_NATIVE_GFX=1` opt-in. It binds this branch's local build/data directories read-only and installs its builtin in that sandbox prefix. It is separate from `XSX_NATIVE_GFX=1`, which selects the XMem BO hook, and from `XSX_DEV_NATIVE_GFX=1`, which selects the VKD3D translation fork. No sandbox or prefix is changed merely by synchronizing this repository.

## Milestones

| Step | Current evidence |
|---|---|
| M1 open / M1b draw in Wine | Recorded PASS |
| M1c sandbox | Hook present; newer game-run logs contain native XMem activation; no standalone M1c result pinned here |
| M2 physical memory | BO helpers/probe/hook present; native XMem and physical-destination storage observed in game logs, dedicated probe proof pending |
| M3 command recording | Connect Xbox list state, PSO registers, fetch programs and actual V#/T# tables |
| M4 presentation | Establish native render target export/copy and Vulkan/DXGI synchronization |
| M5 compute | Execute game dispatches and validate their data dependencies |
| M6 layouts | Validate Xbox/Navi21 tiling, pipe/bank configuration, DCC, mips and depth |

The earlier fork's black image, zero tint and watchdog remain separate observations. A newer supplied screenshot shows account-selection UI text/buttons, without an exact binary association. Direct execution of selected shaders does not explain those failures or establish a native game renderer.
