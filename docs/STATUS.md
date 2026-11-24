# Research checkpoint — November 24, 2026

This is a summary of inspected development source, engineering notes, and local
runtime evidence. Original logs, disassembly, game data, shader captures, and
crash dumps are not part of this source publication. Historical passing results
apply to their tested builds, not automatically to the current shared source.

## Latest game boundary

The current checkpoint again reaches initialization assertions around
`game_scarlett_master.exe+0x1dac734` and `+0x9dfc18`. Serialized contents for
`common://setup2.xml` and an affected tuning file do not match their reported
sizes. The game's archive read/cache path is being investigated; an offset,
stale-buffer, cache-key, or asynchronous completion problem is still a hypothesis.
Crash reports show **Game Init / Frame 0**, with the corrected-scatter run exiting 3.

An intermediate alias implementation failed earlier at `+0xb5a5f8` through a
null effect-program pointer. The initial conclusion that the preceding file/data
assertions were fixed was incorrect: execution had stopped before reaching them.
The engineering report explicitly corrects that conclusion.

The observed game Scatter array repeats a physical group base across consecutive
virtual pages. The revised implementation computes page offsets within that
group. This avoids the buffer collisions of the first interpretation: the `im`
effect now creates 48/48 program objects and the null-program fault disappears.
That result does not establish PC shader translation or rendering correctness.

No correct GTA VI frame, menu, gameplay, sound, or working input is demonstrated.
PresentX requests are observed; the adapter explicitly records `native_present=0`.

## Changes since the first public README draft

| Area | Source/evidence inspected | Scope and remaining limits |
|---|---|---|
| XMem page identity | `runtime/xmem/phys.h`, `xmem.cpp`, aliasing probe, game mapping trace | Shared section and revised grouped-scatter interpretation; 13/13 belongs to the earlier individual-page version |
| Game data integrity | Engineering report and game traces after the aliasing change | Earlier claimed resolution withdrawn; file/data assertions return after avoiding an earlier crash |
| Storage decompression | Dump implementation and checkpoint documenting 200 comparisons | Captured game zlib results match reference zlib; BCpack/swizzle still unsupported |
| GameRuntime | `runtime/xgr_proxy/`, build/deployment scripts, XPackage calls in trace | PC runtime forwarding plus synthetic local package/content provider; full mount/service behavior absent |
| Audio channel | Current `runtime/audio/acphal.cpp` vs earlier implementation | Later 32-slot dummy object replaces explicit unsupported creation; no audio processing |
| Frame lifecycle | Current device80/81 and queue PresentX implementations | Provisional tokens/~60 Hz pacing, synthetic statistics; no native presentation |
| Graphics instrumentation | Current D3D12 diagnostics and effect trace | Tracks scatter, root-signature input, and archive reads; build-specific diagnostic assumptions |

Paths in this table are relative to `compat/` unless stated otherwise.

## Recorded independent results

| Checkpoint | Recorded result | Evidence boundary |
|---|---|---|
| Original Xbox FFX initialization | 27 resources, 5 heaps, 27 SRVs, 64 UAVs, root signature created/reflected | Native context still fails at native PSO; FFX result `0x8000000D`, harness exit 8 |
| Shader archive scan | 26,503 validated discovered containers / 26,698 variants | No completeness claim against encrypted TOC; no general translation claim |
| Selected translated VS | 64 pixels / 256 components, failures 0, max error 0 | Standalone translated native VS with synthetic observing PS |
| Selected translated CS | 4 DWORD writes / 12 guards preserved, failures 0, exit 0 | Standalone selected native compute program |
| Graphics wrapper regression | 62 passed / 0 failed | Frozen source below; current shared device80/81 changes were excluded |
| APU host memory | 70 checks and 4,000 threaded allocations, live 0 | Memory backend only |
| APU Windows-facing probe | 19 checks, exit 0 | Earlier explicit-unsupported channel build; current dummy object changes that expectation |
| HC/XCurl | 139 checks, exit 0; 2 good server requests, 0 errors | Original external DLLs on numeric plaintext loopback; not remote/TLS game services |
| XMem aliasing v1 | 13/13 in Wine | Earlier individual-page contract; preserved source below; not validation of grouped-scatter v2 |
| Grouped-scatter game experiment | 48/48 `im` program objects; earlier null-program fault avoided | Followed by the earlier serialized-data assertions; no first frame |

The 62-check graphics source is preserved in
[`bootstrap-isolated/`](../compat/reports/graphics-resume-20261124/bootstrap-isolated/).
It includes synthetic Draw/Dispatch, barriers/copies, real native submission and
fence completion, guards, root bindings, IA rendering, and wrapper concurrency.
The prebuilt binaries that produced the historical result are excluded. Rebuilt
binaries require their own runtime validation.

The earlier XMem alias implementation and its probe are preserved in
[`xmem-alias-v1/`](../compat/reports/xmem-alias-v1/). Its repeated-ID test assumes
individual pages. The current runtime interprets the observed game Scatter
entries as group bases and therefore must not inherit that historical result.
The optional duplicate-to-private diagnostic is disabled by default and sacrifices
aliasing when enabled.

An earlier diagnostic game run reached 10,241 synthetic PSOs, 22 draws, eight
submissions, and eight PresentX attempts, with `native_present=0`. Those counters
describe that earlier run, not the later null-program trace or correct rendering.

## Practical next steps

1. Validate the grouped-scatter ABI and trace the current RPF block/cache data failure.
2. Connect validated native containers and bindings to real runtime PSO requests.
3. Establish guest-memory/GPU-resource identity and descriptor correctness.
4. Finish frame statistics and native presentation with an independently checked
   diagnostic surface before claiming a game image.
5. Replace diagnostic audio/package behavior with implemented service contracts.

Keep synthetic bootstrap progress separate from rendering correctness. Several
comments in experimental source retain old interpretations: notably the queue-info
label for device81 and old non-aliasing comments in XMem. The implementations and
this checkpoint describe the newer state; these comments are not API guarantees.
