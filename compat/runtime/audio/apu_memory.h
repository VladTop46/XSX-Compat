#pragma once

// CPU memory and a separate 32-bit APU address space for the observed game ABI.
// No DSP, decoder, physical DMA, cache policy or SHAPE mixing is emulated here.
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <new>

namespace xsx::audio {
constexpr int32_t ok = 0;
constexpr int32_t invalid = static_cast<int32_t>(0x80070057u);
constexpr int32_t no_memory = static_cast<int32_t>(0x8007000eu);

class Memory {
    struct Region { void* raw; uintptr_t cpu; uint32_t size; uint32_t span; };
    std::map<uint32_t, Region> regions_;
    mutable std::mutex mutex_;
    const uint64_t limit_;

public:
    explicit Memory(uint64_t limit = uint64_t{1} << 32) : limit_(limit) {}
    Memory(const Memory&) = delete;
    Memory& operator=(const Memory&) = delete;
    ~Memory() { for (auto& item : regions_) std::free(item.second.raw); }

    int32_t allocate(void** cpu, uint32_t* apu, uint32_t size, uint32_t alignment) noexcept {
        if (cpu) *cpu = nullptr;
        if (apu) *apu = 0;
        if (!cpu || !size || !alignment || (alignment & (alignment - 1))) return invalid;
        const uint64_t align = std::max<uint64_t>(64, alignment);
        const uint64_t span = (uint64_t(size) + 63) & ~uint64_t{63};
        if (span > UINT32_MAX || uint64_t(size) + alignment - 1 > SIZE_MAX) return no_memory;
        std::lock_guard<std::mutex> lock(mutex_);
        uint64_t address = 0x10000;
        auto fit = [&] { address = (address + align - 1) & ~(align - 1); };
        fit();
        for (const auto& item : regions_) {
            if (address + span <= item.first) break;
            address = uint64_t(item.first) + item.second.span;
            fit();
        }
        if (address + span > limit_) return no_memory;
        void* raw = std::malloc(size_t(size) + alignment - 1);
        if (!raw) return no_memory;
        uintptr_t pointer = (reinterpret_cast<uintptr_t>(raw) + alignment - 1) & ~(uintptr_t(alignment) - 1);
        try {
            regions_.emplace(uint32_t(address), Region{raw, pointer, size, uint32_t(span)});
        } catch (...) {
            std::free(raw);
            return no_memory;
        }
        std::memset(reinterpret_cast<void*>(pointer), 0, size);
        *cpu = reinterpret_cast<void*>(pointer);
        if (apu) *apu = uint32_t(address);
        return ok;
    }

    int32_t release(void* pointer) noexcept {
        if (!pointer) return ok;
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = regions_.begin(); it != regions_.end(); ++it) {
            if (it->second.cpu == reinterpret_cast<uintptr_t>(pointer)) {
                std::free(it->second.raw);
                regions_.erase(it);
                return ok;
            }
        }
        return invalid; // An interior/foreign address is never handed to free().
    }

    uint32_t address(const void* pointer, uint32_t size = 1) const noexcept {
        if (!pointer || !size) return 0;
        const uintptr_t value = reinterpret_cast<uintptr_t>(pointer);
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& item : regions_) {
            const auto& r = item.second;
            if (value >= r.cpu && value - r.cpu < r.size && size <= r.size - (value - r.cpu))
                return item.first + uint32_t(value - r.cpu);
        }
        return 0;
    }

    void* pointer(uint32_t address) const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = regions_.upper_bound(address);
        if (it == regions_.begin()) return nullptr;
        --it;
        const uint32_t offset = address - it->first;
        return offset < it->second.size ? reinterpret_cast<void*>(it->second.cpu + offset) : nullptr;
    }

    size_t live() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return regions_.size();
    }
};
} // namespace xsx::audio
