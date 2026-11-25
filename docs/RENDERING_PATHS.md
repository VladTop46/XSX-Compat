# Verified rendering paths — November 26, 2026

The current game uses the graphics fork of our `d3d12_xs` compatibility adapter, with PC D3D12/VKD3D-Proton handling the exercised draws. Original Xbox ISA also executes directly on the host GPU in separate validated experiments. Native AMDGPU BO-backed XMem is already integrated into game runs. These are three distinct results.

## What the game executes today

1. `GfxNativeCreatePso57` reads the Xbox PS/VS blobs and extracts their program hashes.
2. `GfxLoadProg` loads matching `.bind` and `.dxil` files from the offline translated cache.
3. `GfxNativeCreatePso57` assigns those DXIL bytes to `pd.VS`/`pd.PS` and calls the PC device's `CreateGraphicsPipelineState`.
4. `ListDraw12`/`ListDrawIndexed13` issue PC `DrawInstanced`/`DrawIndexedInstanced`; queue submission forwards native PC command lists. VKD3D-Proton executes this PC D3D12 workload through Vulkan.
5. With the runner's `XSX_DEV_NOOP_PSO=1`, unsupported programs can use bootstrap no-op PSOs. The game dispatch path uses the bootstrap compute PSO in that mode. A draw/dispatch counter is therefore not proof that every original program executed.

Source: [shader cache/PSO creation](../local-agent-work/src/d3d12_xs/gfx_native.h), [draw/dispatch forwarding and fallback](../local-agent-work/src/d3d12_xs/d3d12_xs.cpp), [submission and refresh](../local-agent-work/src/d3d12_xs/gfx_native_draw.h), [runner flags](../compat/run-native-ui-game.sh).

The fork is of our adapter, not of VKD3D-Proton. In this source tree, the game shader blobs are used to identify a supported translated program; passing those original blobs unchanged to the GPU is not the operation performed by this renderer.

## What executes original ISA directly

The standalone Linux experiments load original shader ISA, construct PM4 and submit it with libdrm_amdgpu. E2 and E3 each record 4,096/4,096 exact pixels on Navi21/gfx1030. E3 includes a geometry-shift negative control. These results demonstrate direct execution of selected original programs with controlled state/resources.

The Wine draw probe calls `xsxgpu_exp2`; its PE export invokes the unixlib entry, which calls `xsx_exp2_draw`. The game's graphics adapter does not call that draw export in the inspected source. The unixlib's exported operations cover the fixed experiment, physical-memory operations and a test compute fill; they are not a live game command-list/PSO renderer. M3 is the integration work connecting game recording and shader/resource state to original-ISA submission.

Source: [Linux raw draw](../compat/tests/native-isa/raw-draw.c), [textured draw](../compat/tests/native-isa/raw-tex.c), [Wine probe](../native-gfx/src/probe.c), [exports](../native-gfx/src/xsxgpu.spec), [unixlib dispatch](../native-gfx/src/xsxgpu_unix.c), [draw core](../native-gfx/src/exp2_core.c).

## Flag meanings

| Selector | Actual effect |
|---|---|
| `XSX_DEV_NATIVE_GFX=1` | Selected translated DXIL and real PC PSOs in our adapter fork |
| `XSX_NATIVE_GFX=1` | XMem physical pages backed by AMDGPU GTT BOs through `xsxgpu` |
| `XSX_SB_NATIVE_GFX=1` | Sandbox access to the local native branch's build/data and Wine builtin |
| `xsxgpu_exp2` called by the probe | Direct original-ISA E2 draw through raw PM4 |
| `XSX_DEV_NOOP_PSO=1` | Diagnostic bootstrap fallback for unsupported shaders/dispatches |

The word `NATIVE` in a log or flag does not identify which of these paths ran. Native BO memory can coexist with the translated game renderer. It does not cause PC D3D12/VKD3D draws to consume Xbox ISA directly.

## Inspected runtime evidence

| Evidence | Observation | Supported conclusion |
|---|---|---|
| `game-native-ui-003613.log` | Native XMem activation; 42 cached PC PSO creation records; draw/refresh activity | Native BO memory and translated game rendering coexist |
| Same log, profiler windows | 29.6 and 30.1 PresentX calls/s | Sampled loop cadence; the counter is incremented at PresentX entry |
| [Account-selection screenshot](../screenshots/player_select_first_screen.png) | Account-selection label, `PLAYER`, button prompts | Visible partial UI; exact DLL/run identity is not pinned by the image |
| `game-native-ui-004939.log` | First PresentX followed by zero-frame profiler windows | Later builds/runs can still stall |
| `game-native-ui-005846.log` | Watchdog access violation at game RVA `0x2215c35` | Watchdog failure is recurring, not exclusively historical |
| Original-ISA E2/E3 readback logs | 4,096/4,096 pixels, max error0; E3 negative control fails | Selected original ISA executes directly in probes |

Raw game logs and shader inputs stay private. Tests and screenshots do not establish full menu/world rendering, gameplay or a complete direct-ISA game renderer. BCPack, texture layouts, broad shader coverage and current runtime failures remain open.

## Submission wording

> XSX-Compat runs native Xbox Series X CPU code on Linux through Wine and custom API/ABI adapters. The game reaches partial account-selection UI using selected offline shader translations through PC D3D12/VKD3D-Proton, with native AMDGPU BO-backed XMem active. Independently, original Xbox VS/PS execute directly on Navi21/gfx1030 through raw PM4/libdrm_amdgpu, producing exact 64x64 GPU readbacks in standalone Linux and Wine probes. Connecting live game command lists to this direct-ISA renderer is ongoing. Full game rendering and gameplay are not yet demonstrated.

This describes the implementation and observed progress; it does not assign a leaderboard tier or claim that a particular leaderboard's criteria have been met.
