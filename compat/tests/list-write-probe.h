#pragma once
// Used by list-reset-probe.cpp with XSX_WRITE_PROBE=1. The destination stays
// alive in main until every submitted list/fence completes.
struct ListWriteProbe {
    UINT64 words[10];
    LARGE_INTEGER before{};
    static constexpr UINT64 guard = 0xabcddcba13572468;
    static constexpr UINT64 value = 0x1020304050607080;
    void Fill() { for (auto& word : words) word = guard; }
    void Record(ID3D12GraphicsCommandList* list) {
        using W32 = void (WINAPI*)(void*, UINT64, UINT, UINT, UINT);
        using W64 = void (WINAPI*)(void*, UINT64, UINT64, UINT, UINT);
        auto vt = *(void***)list;
        Fill(); QueryPerformanceCounter(&before);
        ((W32)vt[71])(list, (UINT64)(uintptr_t)&words[1], 0xfeed1234, 0, 0);
        ((W64)vt[72])(list, (UINT64)(uintptr_t)&words[2], 1, 0, 0);
        ((W64)vt[72])(list, (UINT64)(uintptr_t)&words[2], value, 0, 0);
        ((W64)vt[72])(list, (UINT64)(uintptr_t)&words[3], 0, 0, 1);
        ((W64)vt[72])(list, (UINT64)(uintptr_t)&words[4], 0, 0, 2);
        unsigned changed = 0;
        for (auto word : words) if (word != guard) ++changed;
        CHECK(changed == 0); // Recording must not imply GPU completion.
    }
    void Verify(ID3D12CommandQueue* queue) {
        LARGE_INTEGER after{}, qpcFrequency{};
        QueryPerformanceCounter(&after);
        CHECK(QueryPerformanceFrequency(&qpcFrequency) && qpcFrequency.QuadPart > 0);
        if (qpcFrequency.QuadPart <= 0) return;
        UINT64 gpuFrequency = 0;
        CHECK(SUCCEEDED(queue->GetTimestampFrequency(&gpuFrequency)) && gpuFrequency);
        CHECK(words[1] == ((guard & 0xffffffff00000000ull) | 0xfeed1234));
        CHECK(words[2] == value); // Recording order, not the first value.
        const auto scale = [&](UINT64 qpc) { return (UINT64)((unsigned __int128)qpc * gpuFrequency / (UINT64)qpcFrequency.QuadPart); };
        CHECK(words[3] >= scale(before.QuadPart) && words[3] <= scale(after.QuadPart));
        CHECK(words[4] >= (UINT64)before.QuadPart && words[4] <= (UINT64)after.QuadPart);
        unsigned damaged = words[0] != guard;
        for (unsigned i = 5; i < 10; ++i) if (words[i] != guard) ++damaged;
        CHECK(damaged == 0);
    }
    HRESULT ExecuteX(ID3D12CommandQueue* queue, ID3D12CommandList* list) {
        using Execute = HRESULT (WINAPI*)(void*, UINT, ID3D12CommandList* const*, UINT64, UINT64);
        return ((Execute)(*(void***)queue)[35])(queue, 1, &list, 0, 0);
    }
};
