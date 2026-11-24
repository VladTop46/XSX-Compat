# Research checkpoint — November 24, 2026

**Game Init passed: the game's crash report records Game Running, Frame 82.**
The inspected presentation trace also records a native DXGI swapchain and
repeated test-pattern `Present` calls returning `S_OK`. A correct game image,
menu, gameplay, and sound remain unverified.

This checkpoint summarizes inspected source, the local engineering report, and
runtime evidence. Game files, raw logs, shader captures, and dumps stay private.
Historical test results apply to the builds and fixtures that produced them.

## Changes that advanced initialization

| Area | Implemented change | Result and scope |
|---|---|---|
| DStorage status | Reset reused slots on enqueue; completion generations prevent an older entry completing a newer batch | Removes stale-buffer parsing behind the `setup2` / truncated-data assertions |
| Storage execution | miniz/tinfl fast path, fallback inflater, configurable worker pool, ordered status/signal barriers | 20/20 storage checks recorded; unsupported formats remain |
| Physical pages | PFN identifiers start at `0x100000` and advance by 16 per 64 KiB page; literal scatter IDs; fragmented-placeholder remap | 16/16 aliasing checks recorded; replaces the temporary group/phase heuristic |
| Resources | Guest-facing wrappers return CPU shadows for texture-address access and unwrap at native D3D12 boundaries | Prevents the observed CPU write through a null texture VA; no shadow-to-GPU synchronization/detiling |
| Formats | Additional Xbox depth/stencil and packed-color fallbacks; consistent allocation/creation normalization | Advances observed resource allocation; not equivalent format/depth-UAV emulation |
| AcpHal | Six-method channel with bounded message FIFO, registration masks, immediate completion, and APU notification writes | `COMMAND_COMPLETED` releases FMOD's startup wait and the game's job barrier |
| Presentation | Window discovery, direct-queue DXGI flip-discard swapchain, compatible resource-copy attempt or test pattern | Inspected test-pattern Present succeeds; original game rendering is not validated |
| Diagnostics | Optional thread-stack capture at the first breakpoint | Supports investigation of the remaining allocator assertion |

The AcpHal initialization call is `REGISTER_MESSAGES` (command 2), with a nonzero
completion identifier. The previous dummy object returned success but never
delivered a message. Implementing its completion path is the final observed
change before the Game Running / frame 82 result.

## Current stop

The game still asserts at executable RVA `0x4dcbc9d`: a linear buffer has `0x50`
bytes remaining for a `0x6c0`-byte request. The precise allocator/upload-capacity
contract remains under investigation. Bring-up currently uses
`XSX_DEV_NOOP_PSO=1`; original game shader execution and correct resource data
are still separate work.

The game-state/frame counter and diagnostic presentation are distinct results.
The inspected DXGI run creates a 1924 × 1058 swapchain and reports repeated
test-pattern `Present hr=0x00000000`, with `native_present=1`. Neither result
establishes that the game produced a correct image or reached its menu.

## Recorded independent results

| Checkpoint | Recorded result | Boundary |
|---|---|---|
| Original Xbox FFX | 27 resources, 5 heaps, 27 SRVs, 64 UAVs, root signature | Native context still fails at native PSO: `0x8000000D`, harness exit 8 |
| Shader-container scan | 26,503 validated discovered containers / 26,698 variants | No encrypted-TOC completeness or general translation claim |
| Selected translated VS | 64 pixels / 256 components, failures 0, max error 0 | Standalone translated native VS and synthetic observing PS |
| Selected translated CS | 4 DWORD writes / 12 guards preserved, failures 0, exit 0 | Standalone selected native compute program |
| Graphics regression | 62 passed / 0 failed | Frozen source below; later resource/presentation changes are outside that result |
| APU host memory | 70 checks, 4,000 threaded allocations, live 0 | Memory backend only |
| Historical APU Wine probe | 19 checks, exit 0 | Earlier explicit-unsupported channel contract, not the current FIFO |
| HC/XCurl | 139 checks, exit 0; 2 good requests, 0 server errors | Original external libraries on plaintext numeric loopback |
| Current XMem probe | 16/16 | PFN spacing, literal scatter, aliases, partial remap/unmap, fragmented reservations |
| Storage probe | 20/20 | Recorded file/memory/zlib and queue/status checks |
| Game state | Game Running, Frame 82 | Internal state/counter; followed by allocator assertion |
| Native presentation | Test-pattern Present returns S_OK | Generated diagnostic pixels |

The historical 62-check source is preserved in
[`bootstrap-isolated/`](../compat/reports/graphics-resume-20261124/bootstrap-isolated/).
The earlier 13-check XMem source remains in
[`xmem-alias-v1/`](../compat/reports/xmem-alias-v1/) as a historical checkpoint.
Current PFN behavior supersedes both the old consecutive identifiers and the
intermediate grouped-scatter interpretation. Prebuilt artifacts are excluded.

## Next work

1. Resolve the linear-buffer capacity assertion and sustain Game Running.
2. Synchronize guest texture shadows with native resources and implement detiling.
3. Connect native shader/PSO translation, descriptor addressing, and real game data.
4. Validate game-resource presentation and complete the frame lifecycle.
5. Extend audio completion transport to actual graph/codec/DSP/output processing.
