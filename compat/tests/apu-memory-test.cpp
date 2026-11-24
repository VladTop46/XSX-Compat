#include "../runtime/audio/apu_memory.h"
#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>
#include <stdexcept>

#define CHECK(x) do { if (!(x)) throw std::runtime_error(#x); ++checks; } while (0)
int main() {
    using namespace xsx::audio;
    unsigned checks = 0;
    try {
        Memory memory;
        void* p = reinterpret_cast<void*>(1);
        uint32_t a = 99;
        CHECK(memory.allocate(&p, &a, 100, 3) == invalid && p == nullptr && a == 0);
        CHECK(memory.allocate(nullptr, &a, 100, 64) == invalid && a == 0);
        CHECK(memory.allocate(&p, &a, 0, 64) == invalid && !p && !a);
        for (uint32_t alignment : {1, 64, 2048, 65536, 131072}) {
            CHECK(memory.allocate(&p, &a, 8129, alignment) == ok);
            auto* bytes = static_cast<unsigned char*>(p);
            CHECK(reinterpret_cast<uintptr_t>(p) % alignment == 0 && a % alignment == 0);
            CHECK(bytes[0] == 0 && bytes[8128] == 0);
            CHECK(memory.address(p, 8129) == a && memory.pointer(a) == p);
            CHECK(memory.address(bytes + 8128) == a + 8128);
            CHECK(memory.pointer(a + 8128) == bytes + 8128);
            CHECK(!memory.address(bytes + 8128, 2) && !memory.pointer(a + 8129));
            CHECK(!memory.address(bytes + 8129) && !memory.address(p, UINT32_MAX));
            bytes[8128] = 0x7b;
            CHECK(*static_cast<unsigned char*>(memory.pointer(a + 8128)) == 0x7b);
            CHECK(memory.release(bytes + 1) == invalid && memory.live() == 1);
            CHECK(memory.release(p) == ok && memory.live() == 0);
            CHECK(!memory.pointer(a) && !memory.address(p) && memory.release(p) == invalid);
        }
        CHECK(memory.release(nullptr) == ok && !memory.pointer(0));
        Memory limited(0x10100);
        void* p2; uint32_t a2;
        CHECK(limited.allocate(&p, &a, 256, 64) == ok);
        CHECK(limited.allocate(&p2, &a2, 1, 64) == no_memory && !p2 && !a2);
        CHECK(limited.release(p) == ok);
        CHECK(limited.allocate(&p2, &a2, 128, 64) == ok && a == a2);
        CHECK(limited.release(p2) == ok);
        std::atomic<unsigned> failures{0};
        std::vector<std::thread> workers;
        for (int t = 0; t < 8; ++t) workers.emplace_back([&] {
            for (int n = 0; n < 500; ++n) {
                void* cpu; uint32_t bus;
                if (memory.allocate(&cpu, &bus, 1024, 256) != ok) { ++failures; continue; }
                if (memory.pointer(bus + 1000) != static_cast<char*>(cpu) + 1000 ||
                    memory.address(cpu, 1024) != bus || memory.release(cpu) != ok) ++failures;
            }
        });
        for (auto& thread : workers) thread.join();
        CHECK(failures == 0 && memory.live() == 0);
        std::printf("[PASS] APU memory checks=%u threaded_allocations=4000 live=0\n", checks);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[FAIL] %s\n", e.what());
        return 1;
    }
}
