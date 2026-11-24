#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>

template<class T> T symbol(HMODULE module, const char* name) {
    auto address = GetProcAddress(module, name);
    if (!address) throw std::runtime_error(name);
    T result;
    static_assert(sizeof(result) == sizeof(address));
    std::memcpy(&result, &address, sizeof(result));
    return result;
}
#define CHECK(x) do { if (!(x)) throw std::runtime_error(#x); ++checks; } while (0)
int main() {
    unsigned checks = 0;
    try {
        auto module = LoadLibraryW(L"AcpHal.dll");
        CHECK(module != nullptr);
        auto create = symbol<HRESULT(WINAPI*)(uint32_t, uint32_t)>(module, "ApuCreateHeap");
        auto alloc = symbol<HRESULT(WINAPI*)(void**, uint32_t*, uint32_t, uint32_t, uint32_t)>(module, "ApuAlloc");
        auto free = symbol<HRESULT(WINAPI*)(void*)>(module, "ApuFree");
        auto valid = symbol<BOOL(WINAPI*)(const void*, uint32_t)>(module, "ApuIsVirtualAddressValid");
        auto map = symbol<uint32_t(WINAPI*)(const void*)>(module, "ApuMapVirtualAddress");
        auto reverse = symbol<void*(WINAPI*)(uint32_t)>(module, "ApuMapApuAddress");
        CHECK(create(8 << 20, 0x226e00) == S_OK);
        void* cpu = nullptr; uint32_t bus = 0;
        CHECK(alloc(&cpu, &bus, 0x222e00, 0x800, 2) == S_OK && cpu && bus);
        CHECK(reinterpret_cast<uintptr_t>(cpu) % 0x800 == 0);
        CHECK(valid(cpu, 0x222e00) && !valid(cpu, 0x222e01));
        CHECK(map(cpu) == bus && reverse(bus) == cpu);
        auto* data = static_cast<uint8_t*>(cpu);
        data[0x222dff] = 0xc7;
        CHECK(map(data + 0x222dff) == bus + 0x222dff);
        CHECK(*static_cast<uint8_t*>(reverse(bus + 0x222dff)) == 0xc7);
        CHECK(!map(data + 0x222e00) && !reverse(bus + 0x222e00));
        CHECK(FAILED(free(data + 1)) && valid(cpu, 1));
        CHECK(free(cpu) == S_OK && !valid(cpu, 1) && !reverse(bus));
        struct Shape { uint32_t counts[5], reserved; void* arrays[5]; } shape{};
        static_assert(sizeof(Shape) == 64 && offsetof(Shape, arrays) == 24);
        auto contexts = symbol<HRESULT(WINAPI*)(Shape*)>(module, "AcpHalAllocateShapeContexts");
        auto release = symbol<HRESULT(WINAPI*)()>(module, "AcpHalReleaseShapeContexts");
        shape.counts[4] = 350;
        CHECK(contexts(&shape) == S_OK && shape.arrays[4]);
        CHECK(valid(shape.arrays[4], 350 * 64) && !valid(shape.arrays[4], 350 * 64 + 1));
        CHECK(!shape.arrays[0] && !shape.arrays[1] && !shape.arrays[2] && !shape.arrays[3]);
        CHECK(FAILED(contexts(&shape))); // Must not silently replace live arrays.
        CHECK(release() == S_OK && !valid(shape.arrays[4], 1));
        CHECK(release() == S_OK);
        shape = {}; shape.counts[0] = 1;
        CHECK(contexts(&shape) == E_NOTIMPL && !shape.arrays[0]);
        auto hal = symbol<HRESULT(WINAPI*)(void**)>(module, "AcpHalCreate");
        void* object = reinterpret_cast<void*>(1);
        CHECK(hal(&object) == E_NOTIMPL && !object);
        FreeLibrary(module);
        std::printf("[PASS] APU runtime checks=%u memory_roundtrip=1 shape_xma=350 unsupported_channel=explicit\n", checks);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[FAIL] APU %s win32=%lu\n", e.what(), GetLastError());
        return 1;
    }
}
