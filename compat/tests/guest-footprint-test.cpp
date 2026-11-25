#include "../d3d12_xs/guest_footprint.h"
#include <cstdio>

static int checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::printf("[FAIL] line=%d %s\n", __LINE__, #x); } } while (0)
int main()
{
    using xsx::memory::Footprint;
    using xsx::memory::Color64K;
    struct Case { uint32_t width, height, layers, bytes; uint64_t size; };
    const Case cases[]{
        {8, 8, 1, 4, 65536}, {128, 128, 1, 4, 65536},
        {129, 128, 1, 4, 131072}, {128, 129, 1, 4, 131072},
        {1024, 1024, 1, 4, 0x400000}, {3840, 2160, 1, 4, 0x1fe0000},
        {3840, 2160, 2, 4, 0x3fc0000}, {1280, 720, 1, 8, 0x780000},
        {128, 64, 1, 8, 65536}, {129, 64, 1, 8, 131072},
        {64, 64, 1, 16, 65536}, {256, 128, 1, 2, 65536},
        {256, 256, 1, 1, 65536},
    };
    Footprint result;
    for (const auto& test : cases) {
        CHECK(Color64K(test.width, test.height, test.layers, test.bytes, 0, 0, result));
        CHECK(result.size == test.size && result.alignment == 65536);
    }
    // The fixed title pool must contain both backbuffers, independent of
    // host image compression/metadata padding (observed native0x2010000 each).
    CHECK(Color64K(3840, 2160, 2, 4, 0x11b, 0, result));
    CHECK(0x4000000 - result.size == 0x40000);
    CHECK(Color64K(8, 8, 1, 4, 0, 0x200000, result));
    CHECK(result.size == 0x200000 && result.alignment == 0x200000);
    CHECK(!Color64K(0, 128, 1, 4, 0, 0, result));
    CHECK(!Color64K(~uint64_t{0}, 128, 1, 4, 0, 0, result));
    CHECK(!Color64K(128, 0, 1, 4, 0, 0, result));
    CHECK(!Color64K(128, 128, 0, 4, 0, 0, result));
    CHECK(!Color64K(128, 128, 1, 3, 0, 0, result));
    CHECK(!Color64K(128, 128, 1, 4, 0x118, 0, result)); // Depth layout is outside this contract.
    CHECK(!Color64K(128, 128, 1, 4, 1, 0, result)); // Row-major is not a64K tiled surface.
    CHECK(!Color64K(128, 128, 1, 4, 0, 65537, result));
    CHECK(!Color64K(128, 128, 1, 4, 0, ~uint64_t{0}, result));
    // Sweep odd dimensions: growth cannot shrink the allocation, every
    // allocation covers its texels, and sizes consist of complete64K blocks.
    bool valid = true;
    unsigned swept = 0;
    for (unsigned bytes : {1u, 2u, 4u, 8u, 16u}) {
        for (unsigned height = 1; height < 17000; height += 127) {
            if (height > 16384) break;
            uint64_t last = 0;
            for (unsigned width = 1; width <= 16384; width += 131) {
                valid &= Color64K(width, height, 1, bytes, 0, 0, result);
                valid &= result.size >= uint64_t(width) * height * bytes && result.size >= last;
                valid &= !(result.size & 65535); last = result.size; ++swept;
            }
        }
    }
    CHECK(valid);
    std::printf("[%s] guest footprint checks=%d failures=%d swept=%u two4K_buffers_bytes=0x3fc0000\n",
        failures ? "FAIL" : "PASS", checks, failures, swept);
    return failures ? 1 : 0;
}
