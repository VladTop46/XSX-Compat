#pragma once

// Local callsites: +0x4ed46a3 writes32(address,value,flush,flags0);
// +0x2482b31 writes64(address,0,flush0,flags1). Cross-checked against
// VIctory list.rs newer-list mapping: 71/72 are bottom-of-pipe writes.
// Keep these pending until the real submitted GPU work completes. CPU-time
// timestamps match the queue's tick rate but do not measure GPU positions.
struct ListPendingWrite { UINT64 address, value; UINT bytes, flags; };
static SRWLOCK g_listWritesLock = SRWLOCK_INIT;
static std::map<Wrap*, std::vector<ListPendingWrite>> g_listWrites;

static void DiscardListWrites(Wrap* list)
{
    ExclusiveSrwLock held(g_listWritesLock);
    g_listWrites.erase(list);
}
static void RecordListWrite(Wrap* list, UINT64 address, UINT64 value, UINT bytes, UINT flags)
{
    if (!IsXboxList(list) || !address) return;
    if ((bytes == 4 && flags) || (bytes == 8 && flags > 2)) {
        XS_LOG("list write%u unsupported flags=%#x address=%#llx (not recorded)", bytes * 8, flags, (unsigned long long)address);
        return;
    }
    {
        ExclusiveSrwLock held(g_listWritesLock);
        g_listWrites[list].push_back({address, value, bytes, flags});
    }
    static std::atomic<unsigned> calls{0};
    const unsigned n = ++calls;
    if (n <= 8 || !(n % 10000)) XS_LOG("list write%u deferred n=%u address=%#llx value=%#llx flags=%#x", bytes * 8, n, (unsigned long long)address, (unsigned long long)value, flags);
}
static void __stdcall ListWrite32_71(Wrap* list, UINT64 address, UINT value, UINT /*flush*/, UINT flags)
{
    RecordListWrite(list, address, value, 4, flags);
}
static void __stdcall ListWrite64_72(Wrap* list, UINT64 address, UINT64 value, UINT /*flush*/, UINT flags)
{
    RecordListWrite(list, address, value, 8, flags);
}
static HRESULT SubmitListsWithWrites(Wrap* wrapper, UINT count, ID3D12CommandList* const* lists)
{
    std::vector<ID3D12CommandList*> native(count);
    std::vector<ListPendingWrite> writes;
    {
        ExclusiveSrwLock held(g_listWritesLock);
        for (UINT i = 0; i < count; ++i) {
            native[i] = (ID3D12CommandList*)Unwrap(lists[i]);
            auto it = g_listWrites.find((Wrap*)lists[i]);
            // A closed command list can be submitted more than once; keep its
            // commands until successful Reset, rather than consuming them here.
            if (it != g_listWrites.end()) writes.insert(writes.end(), it->second.begin(), it->second.end());
        }
    }
    auto* queue = (ID3D12CommandQueue*)wrapper->real;
    if (writes.empty()) { queue->ExecuteCommandLists(count, native.data()); return S_OK; }

    // Bootstrap implementation: submissions carrying CPU-visible writes wait
    // synchronously. A timer/early CPU store must not invent GPU completion.
    ID3D12Device* device = nullptr;
    ID3D12Fence* fence = nullptr;
    HRESULT hr = queue->GetDevice(__uuidof(ID3D12Device), (void**)&device);
    if (SUCCEEDED(hr)) { hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void**)&fence); device->Release(); }
    if (FAILED(hr)) return hr;
    HANDLE completed = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!completed) { const DWORD error = GetLastError(); fence->Release(); return HRESULT_FROM_WIN32(error); }
    hr = fence->SetEventOnCompletion(1, completed);
    if (SUCCEEDED(hr)) {
        queue->ExecuteCommandLists(count, native.data());
        hr = queue->Signal(fence, 1);
    }
    if (SUCCEEDED(hr)) {
        const DWORD waited = WaitForSingleObject(completed, 10000);
        if (waited != WAIT_OBJECT_0) hr = waited == WAIT_TIMEOUT ? HRESULT_FROM_WIN32(WAIT_TIMEOUT) : waited == WAIT_FAILED ? HRESULT_FROM_WIN32(GetLastError()) : E_FAIL;
        else if (fence->GetCompletedValue() == UINT64_MAX) hr = DXGI_ERROR_DEVICE_REMOVED;
    }
    CloseHandle(completed); fence->Release();
    if (FAILED(hr)) {
        XS_LOG("deferred writes GPU completion failed hr=0x%08lX; no CPU writes published", (unsigned long)hr);
        return hr;
    }
    LARGE_INTEGER frequency{}, counter{};
    UINT64 gpuFrequency = 0;
    QueryPerformanceFrequency(&frequency);
    const HRESULT clockHr = queue->GetTimestampFrequency(&gpuFrequency);
    unsigned stored = 0;
    for (const auto& write : writes) {
        UINT64 value = write.value;
        if (write.flags) {
            if (!QueryPerformanceCounter(&counter) || frequency.QuadPart <= 0 ||
                (write.flags == 1 && FAILED(clockHr))) { hr = E_FAIL; continue; }
            value = write.flags == 1 ? (UINT64)((unsigned __int128)(UINT64)counter.QuadPart * gpuFrequency / (UINT64)frequency.QuadPart) : (UINT64)counter.QuadPart;
        }
        SIZE_T done = 0;
        if (!WriteProcessMemory(GetCurrentProcess(), (void*)(uintptr_t)write.address, &value, write.bytes, &done) || done != write.bytes) {
            hr = E_FAIL;
            XS_LOG("deferred write%u address=%#llx failed after GPU completion", write.bytes * 8, (unsigned long long)write.address);
        } else ++stored;
    }
    static std::atomic<unsigned> batches{0};
    const unsigned n = ++batches;
    if (n <= 8 || !(n % 1000) || FAILED(hr)) XS_LOG("deferred writes GPU-complete batch=%u stored=%u/%zu hr=0x%08lX (timestamps=CPU timeline)", n, stored, writes.size(), (unsigned long)hr);
    return hr;
}
