# Research checkpoint — November 26, 2026

**The first visible account-selection UI is captured, and original Xbox game VS/PS have executed directly on Navi21/gfx1030 with exact GPU readback.** The separate Wine unixlib also records a passing direct draw. These advance beyond the November 24 Game Running / Frame 82 checkpoint; complete menu/world rendering and gameplay remain unverified.

[The reviewed screenshot](../screenshots/player_select_first_screen.png) visibly contains the account-selection label, `PLAYER` and button prompts on a largely blank background. It does not identify the exact DLL build or establish the full renderer.

This document distinguishes recorded development evidence from current source. Raw logs, dumps, captures and shader payloads stay private. Export compilation does not rerun historical GPU results.

## Direct ISA path

| Experiment | Recorded result | Conditions |
|---|---|---|
| E1 | PASS 1,024/1,024, guards0, fence complete | Authored compute ISA; COMPUTE PM4; identity CPU/GPU VA |
| E2 | PASS 4,096/4,096, max_error0, guard_errors0 | Original game blit VS+PS; GFX PM4; legacy VS wave64; RECTLIST; linear 64x64 RGBA8 |
| E2 variants | All pass | s_nop prefix, original 24-byte hash prefix, game V# word3 |
| E3 | PASS 4,096/4,096, max_error0, guard_errors0 | Original textured pair; two matrix V# buffers; explicit linear T# and inline sampler |
| E3 negative control | Expected FAIL: ok0/4,096, untouched512 | Geometry shifted by eight pixels, demonstrating oracle sensitivity |
| BO fixed mapping | Recorded PASS | CPU aliases and GPU mapping of GTT BO memory at matching addresses |
| M1/M1b Wine unixlib | Recorded PASS, draw4,096/4,096 | Builtin PE + Unix `.so`; direct draw inside Wine |
| M1c/M2 | Source present; newer game logs show native XMem activation | Sandbox opt-in, BO physical map/unmap, compute helper and probe; standalone M2 pass is not recorded here |

The original game shaders are not supplied. The authored fetch subroutine and compute assembly are supplied. Tiled textures, DCC, depth, general blending, mesh programs, game-native compute, complete command recording and VKD3D interoperability remain open. PSO field interpretations are proven only for the exercised states. M2 overlays also have a documented Wine virtual-memory bookkeeping limitation.

## Game execution and selected-program VKD3D path

The earlier fatal writer assertion at executable RVA `0x4dcbc9d` was a nonconsuming audio path, not an established graphics upload-capacity problem. Tracing found zero output rings and a lock retained by an early return. Enabling a real null audio endpoint produced a nonnull endpoint/ring, repeated updates and a 90-second run ending in timeout (`124`) without that assertion. Its sampled PresentX source counter reached at least 2,400, missing0. Null PCM discards playback.

Later exact-build checks recorded:

| Result | Meaning |
|---|---|
| UI readback:8 programs /4 pairs /8 cases /512 pixels, exit0 | Selected translated VS/PS execute with controlled buffers, textures and blending |
| Bridge:112 checks /4 PSOs /6 cases /384 pixels, exit0 | Earlier pointer-first PSO description and first-child vtable initialization verified |
| Game:42 native PSO success records | Real selected translated PSOs created in the game path; not general Xbox PSO support |
| Draw A offscreen -> Draw B sampled resource -> PresentX | Resource identity chain inspected; a visible magenta diagnostic marker corroborated target-to-window routing |
| Viewport/scissor | Exercised slots21/22 accepted the observed PC layout |
| Earlier game image | Black in earlier inspected runs; draw A's96-byte constant buffer held zero tint at recording and submit |
| Latest visual evidence | Supplied screenshot shows account-selection UI text/buttons; exact source/binary association is not pinned |
| Earlier known game stop | Watchdog at executable RVA `0x2215c35`; cause of the stalled heartbeat remains unresolved |

The fork now contains RECTLIST expansion and submit-time guest buffer refresh. Its snapshot probe deliberately disables refresh; a recorded refresh-on probe failed after rereading mutated fixture memory. Do not attribute the earlier 112/0 to the later game refresh policy. The latest fork is experimental, not a fully validated renderer. New logs `game-native-ui-003345.log` and `game-native-ui-003613.log` contain changed VB/IB refresh records; their sampled cumulative stale-range counts are 230 and 461 at 2,000 submissions. Stale draw-time data is now observed for those ranges, without proving general GPU coherence. `game-native-ui-003845.log` also records a first-chance breakpoint at executable RVA `0x4ffd879`; its final exit/cause is not established by that line.

## Runtime results retained

| Area | Recorded proof | Boundary |
|---|---|---|
| Map / ResetX / writes / stats81 | 15/0,38/0,60/0,6/0 respective probes | Map fallback, real PC Reset, queue-completed deferred writes, no fabricated statistics |
| Frame events |37/0 Windows;66 portable checks +8,000 tokens | Observed event interval/token/timeout contract; CPU clock, not GPU statistics |
| Resource127 |Title79/0;fallback59/0;GPU64 pixels/guards | Alignment and paired title allocation; two guest3840x2160 buffers fit64MiB |
| Surface blit |133 checks/6 GPU cases;max_error1,guards0 | Native source scaling; separate from correctness of game contents |
| Buffer127 |Corrected probe reported PASS;7 GPU roundtrips | READBACK/UPLOAD sizes observed in game; Width0 explicitly rejected |
| Audio endpoint |259 checks;WASAPI12 callbacks;spatial12 nonempty updates | Null sink; not SHAPE/DSP/codec execution or audible game audio |
| XMem retained neighbors |Portable163 checks,gaps0 | Section-view model; later BO hook is outside that historical result |
| Return/task trace |Typed ABI, concurrent calls and persistent trap probes | Diagnostics only; neither skips game faults nor forces consumer calls |
| Original Xbox FFX |27 resources,5 heaps,27 SRVs,64 UAVs,root signature |`ffxCreateContext=0x8000000D`, process8 at native PSO |
| Shader scan |26,503 discovered containers /26,698 variants validated |No encrypted-TOC completeness or universal ISA claim |
| Network |139 checks,exit0 |Original HC/XCurl libraries on plaintext numeric loopback |
| Earlier graphics/XMem/storage |62/0 frozen graphics;16/16 PFN aliases;20/20 storage |Historical build-specific results, not blanket coverage of current code |

XCRDAPI source now scatters plain/zlib requests to physical destinations through the new XMem `XsxPhysWrite` export. This is newer source, separate from the historical 20/20 storage result; the newer inspected game logs contain `dstPhys OK` records with native XMem/AMDGPU active. Those log records demonstrate the exercised path, not a complete storage correctness/readback test. BCpack, swizzle and physical sources remain unsupported.

GameInput now implements a keyboard-driven virtual pad and UI navigation. The GameRuntime proxy includes a local offline XUser provider and logging. Their presence is a source result; passing gameplay, complete callbacks and real online identity are not established here.

## Preserved regressions

The Xbox `0x39f8` object was identified as `ID3D12DescriptorHeap`; its CPU-handle register-return ABI is adapted by `--heap-abi`. It is no longer the current FFX crash barrier. The original working committed-resource call and fresh Texture2D layout conversion remain. Native VKD3D resource vtables are not patched.

The preserved [62-check graphics snapshot](../compat/reports/graphics-resume-20261124/bootstrap-isolated/) and [earlier XMem source](../compat/reports/xmem-alias-v1/) are historical. Current main runtime, graphics fork and direct native branch are distinct sources with separate build/runtime coverage.

## Next evidence needed

Validate sandbox/native memory against current binaries, then native Xbox list recording, descriptor formats, texture layouts and visibility. Resolve the watchdog and actual image contents. A complete game frame/menu, gameplay, audio DSP and successful original FFX context each require their own proof; no such result is claimed by a timeout or source synchronization.

The latest exported fork also has an opt-in `XSX_NO_WATCHDOG=1` debugging override of the game's watchdog flag. It was not enabled by this synchronization or the FFX smoke. Runs using it cannot establish that the underlying heartbeat stall is fixed.
