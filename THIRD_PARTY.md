# External components

This source snapshot contains the compatibility implementation, research tools,
probe sources, synthetic test inputs, and public documentation selected from the
development workspace. External dependency trees and their binaries are omitted.

| Component | Use | Included here |
|---|---|---|
| Wine | PE/Win32 execution and virtual-memory APIs | No |
| VKD3D-Proton | PC D3D12 to Vulkan | No |
| DXVK | DXGI used by the graphics setup | No |
| Mesa / Vulkan driver | GPU execution | No |
| MinGW-w64 | Windows headers and cross compiler | No |
| LLVM | PE analysis and AMDGPU disassembly | No |
| DXC | HLSL to DXIL compilation | No |
| Xbox GDK samples / FidelityFX headers | Headers for selected experiments | No |
| Microsoft GDK / GameRuntime / native CRT | Optional local runtime and header dependencies | No |
| DirectXMath | Header dependency of the early hello probe | No |
| miniz / tinfl | Fast zlib decompression for storage requests | C/header source and permission notice |
| Original game and its libraries/content | Private research inputs | No |

The small inflater in `compat/runtime/xcrdapi/inflate.h` describes itself as a
project implementation of RFC 1950/1951 using the approach of puff. Its existing
attribution comment is retained; no external puff source tree is bundled.

The current storage fast path uses the miniz copy from esptool's flasher stub.
The bundled `miniz.c` and `miniz.h` retain their existing notices. The permission
text from the source header is also provided as
[`LICENSE`](compat/runtime/xcrdapi/third_party/miniz/LICENSE). Copyright notices
identify RAD Game Tools, Valve Software, Rich Geldreich, and Tenacious Software.

The current AcpHal source explicitly references the open VIctory implementation
(`crates/victory-hle/src/modules/audio.rs`) for channel layout and semantics;
resource/presentation research also references its GPU modules. The development
report identifies VIctory by Marvisak under GPL-3.0. These references are retained
as implementation provenance; no VIctory source tree is bundled here.

Synthetic HLSL strings in the graphics probes and bootstrap helpers are part of
the test/diagnostic implementation. Generated shader headers and translations
derived from game programs are omitted, together with original shader bytecode.

API names, interface identifiers, inferred layouts, and module-relative callsite
references document the contracts being investigated. They do not supply the
referenced executables, SDK implementations, archive contents, or certificates.

No project-wide license has been selected in this snapshot. No license grant for
external components is implied. Their own notices and licenses apply when those
components are obtained separately.
