#pragma once
#include <cstdint>
#include <algorithm>

namespace xsx::memory {
struct Footprint { uint64_t size = 0, alignment = 0; };
// Size only, not a swizzle/address translator. Single-mip, single-sample,
// uncompressed 2D color surfaces in 64KiB GFX10 thin blocks. The block's
// texel area is 65536 / bytesPerTexel; split its exponent across X and Y.
// This is the ComputeThinBlockDimension rule from AMD addrlib, independently
// corroborated by VIctory's texture layout. Unknown layouts are rejected.
inline bool Color64K(uint64_t width, uint32_t height, uint32_t layers,
    uint32_t bytesPerTexel, uint32_t layout, uint64_t requestedAlignment,
    Footprint& result)
{
    result = {};
    if (!width || width > 16384 || !height || height > 16384 || !layers || layers > 2048 ||
        !bytesPerTexel || bytesPerTexel > 16 || (bytesPerTexel & (bytesPerTexel - 1))) return false;
    switch (layout) {
    case 0: // Default color render target: SW_64KB_R_X.
    case 0x109: case 0x10a: case 0x111: case 0x112:
    case 0x119: case 0x11a: case 0x11b: break;
    default: return false;
    }
    if (requestedAlignment && (requestedAlignment & (requestedAlignment - 1))) return false;
    const uint64_t alignment = std::max<uint64_t>(65536, requestedAlignment);
    if (alignment > (1ull << 28)) return false;
    unsigned elementBits = 0;
    for (uint32_t bytes = bytesPerTexel; bytes > 1; bytes >>= 1) ++elementBits;
    const unsigned texelBits = 16 - elementBits;
    const uint64_t blockWidth = 1ull << ((texelBits + 1) / 2);
    const uint64_t blockHeight = 1ull << (texelBits / 2);
    const uint64_t columns = (width + blockWidth - 1) / blockWidth;
    const uint64_t rows = (height + blockHeight - 1) / blockHeight;
    const uint64_t bytes = columns * rows * 65536 * layers;
    result = {(bytes + alignment - 1) & ~(alignment - 1), alignment};
    return true;
}
} // namespace xsx::memory
