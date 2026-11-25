#pragma once
// Opt-in breakpoint tracing of three register MOVs and a short JNZ only.
// Each owned instruction is emulated exactly with the INT3 left installed;
// concurrent threads never pass through a temporary unpatched window.
#include <windows.h>
#include <atomic>
#include <array>
#include <cstring>
#include "instruction_resume.h"

namespace xsx::diagnostic {
class PersistentTrace {
    std::array<Point,4> points_{};
    std::array<std::atomic<UINT64>,4> hits_{};
    std::atomic<unsigned> armed_{0};
    PVOID handler_ = nullptr;
    void (*observe_)(unsigned, const CONTEXT&) noexcept = nullptr;
    inline static PersistentTrace* instance_ = nullptr;
    static const unsigned char* Bytes(Instruction instruction, SIZE_T& length)
    {
        static constexpr unsigned char a[]{0x48,0x89,0xc8}; // mov rax,rcx
        static constexpr unsigned char b[]{0x48,0x89,0xce}; // mov rsi,rcx
        static constexpr unsigned char c[]{0x48,0x89,0xf1}; // mov rcx,rsi
        static constexpr unsigned char d[]{0x75,0x05};      // jnz +5
        length = instruction == Instruction::JnzPlusFive ? 2 : 3;
        switch (instruction) {
        case Instruction::RaxFromRcx: return a;
        case Instruction::RsiFromRcx: return b;
        case Instruction::RcxFromRsi: return c;
        case Instruction::JnzPlusFive: return d;
        }
        return nullptr;
    }
    static bool WriteByte(uintptr_t address, unsigned char byte)
    {
        DWORD old = 0;
        if (!VirtualProtect((void*)address, 1, PAGE_EXECUTE_READWRITE, &old)) return false;
        *(volatile unsigned char*)address = byte;
        DWORD ignored = 0;
        const bool restored = VirtualProtect((void*)address, 1, old, &ignored) != FALSE;
        const bool flushed = FlushInstructionCache(GetCurrentProcess(), (void*)address, 1) != FALSE;
        return restored && flushed;
    }
    int Match(const EXCEPTION_POINTERS* exception) const
    {
        if (!exception || !exception->ExceptionRecord || !exception->ContextRecord ||
            exception->ExceptionRecord->ExceptionCode != EXCEPTION_BREAKPOINT) return -1;
        const auto address = (uintptr_t)exception->ExceptionRecord->ExceptionAddress;
        const auto rip = exception->ContextRecord->Rip;
        const unsigned armed = armed_.load(std::memory_order_acquire);
        for (unsigned i = 0; i < points_.size(); ++i)
            if ((armed & (1u << i)) && address == points_[i].address && (rip == address || rip == address + 1)) return (int)i;
        return -1;
    }
    static LONG CALLBACK Handler(EXCEPTION_POINTERS* exception)
    {
        auto* trace = instance_;
        if (!trace) return EXCEPTION_CONTINUE_SEARCH;
        const int index = trace->Match(exception);
        if (index < 0) return EXCEPTION_CONTINUE_SEARCH;
        const DWORD error = GetLastError();
        CONTEXT& context = *exception->ContextRecord;
        ++trace->hits_[(unsigned)index];
        if (trace->observe_) trace->observe_((unsigned)index, context);
        const auto point = trace->points_[(unsigned)index];
        // These MOVs change one GPR only, and JNZ reads ZF without changing
        // any register/flag. No stack/memory/atomic instruction is emulated.
        Resume(point, context.Rip, context.Rax, context.Rcx, context.Rsi, context.EFlags);
        SetLastError(error);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
public:
    // Install/remove only while the target code is quiescent (DLL startup in
    // the game; before/after joined workers in the probe). No destructor hook.
    HRESULT Install(const std::array<Point,4>& points, void (*observe)(unsigned,const CONTEXT&) noexcept = nullptr)
    {
        if (instance_ || handler_) return E_UNEXPECTED;
        for (unsigned i = 0; i < points.size(); ++i) {
            if (!points[i].address) return E_INVALIDARG;
            for (unsigned j = 0; j < i; ++j) if (points[j].address == points[i].address) return E_INVALIDARG;
            SIZE_T length = 0, read = 0; unsigned char actual[3]{};
            const auto* expected = Bytes(points[i].instruction, length);
            if (!expected || !ReadProcessMemory(GetCurrentProcess(), (void*)points[i].address, actual, length, &read) ||
                read != length || std::memcmp(actual, expected, length)) return E_INVALIDARG;
        }
        points_ = points; observe_ = observe;
        for (auto& hits : hits_) hits.store(0);
        instance_ = this; handler_ = AddVectoredExceptionHandler(1, Handler);
        if (!handler_) { instance_ = nullptr; return E_FAIL; }
        for (unsigned i = 0; i < points_.size(); ++i) {
            // Publish ownership before the one-byte code patch. Target code
            // is not running during installation; handlers are already ready.
            armed_.fetch_or(1u << i, std::memory_order_release);
            if (!WriteByte(points_[i].address, 0xcc)) { Uninstall(); return E_FAIL; }
        }
        return S_OK;
    }
    bool Owns(const EXCEPTION_POINTERS* exception) const { return Match(exception) >= 0; }
    UINT64 Hits(unsigned index) const { return hits_[index].load(); }
    bool Active() const { return armed_.load() != 0; }
    bool Intact() const
    {
        if (armed_.load() != 0xf) return false;
        for (const auto& point : points_) {
            SIZE_T length = 0, read = 0; unsigned char actual[3]{};
            const auto* original = Bytes(point.instruction, length);
            if (!ReadProcessMemory(GetCurrentProcess(), (void*)point.address, actual, length, &read) ||
                read != length || actual[0] != 0xcc || std::memcmp(actual+1,original+1,length-1)) return false;
        }
        return true;
    }
    bool Uninstall()
    {
        bool restored = true;
        const unsigned armed = armed_.load();
        for (unsigned i = 0; i < points_.size(); ++i) if (armed & (1u << i)) {
            SIZE_T length = 0; const auto* original = Bytes(points_[i].instruction, length);
            if (WriteByte(points_[i].address, original[0])) armed_.fetch_and(~(1u << i));
            else restored = false;
        }
        if (restored) {
            if (handler_) RemoveVectoredExceptionHandler(handler_);
            handler_ = nullptr; instance_ = nullptr;
        }
        return restored;
    }
};
} // namespace xsx::diagnostic
