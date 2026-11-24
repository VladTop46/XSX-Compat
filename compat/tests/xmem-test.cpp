#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>

using Alloc = void* (*)(size_t, uint64_t);
using Free = void (*)(void*, uint64_t);

int main() {
    HMODULE dll = LoadLibraryA("xmem.dll");
    if (!dll) {
        printf("FAIL: LoadLibrary error %lu\n",
               GetLastError());
        return 1;
    }

    auto alloc = reinterpret_cast<Alloc>(
        GetProcAddress(dll, "XMemAlloc"));

    auto free_mem = reinterpret_cast<Free>(
        GetProcAddress(dll, "XMemFree"));

    if (!alloc || !free_mem) {
        puts("FAIL: Missing exports");
        return 2;
    }

    constexpr uint64_t attrs = 0xA8004000;

    for (size_t size : {
        size_t(1),
        size_t(256),
        size_t(4096),
        size_t(65536),
        size_t(1048576)
    }) {
        void* p = alloc(size, attrs);

        if (!p) {
            printf("FAIL: alloc(%zu)\n", size);
            return 3;
        }

        auto address = reinterpret_cast<uintptr_t>(p);

        printf("size=%zu ptr=%p align256=%s\n",
               size, p,
               (address % 256 == 0) ? "YES" : "NO");

        std::memset(p, 0xA5, size);

        auto bytes = static_cast<uint8_t*>(p);
        for (size_t i = 0; i < size; ++i) {
            if (bytes[i] != 0xA5) {
                puts("FAIL: data corruption");
                return 4;
            }
        }

        free_mem(p, attrs);
    }

    puts("PASS: allocation/write/read/free");
    FreeLibrary(dll);
    return 0;
}
