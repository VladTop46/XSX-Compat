#pragma once

// Xbox fence slot12 returns a CPU-visible UINT64 completion counter, consumed
// by game RVA0x4fb0ad0. PC fences have no such method. This read-only mirror
// follows the real PC fence, including UINT64_MAX on device removal. Neither
// queue submission nor a timer may invent a completed value.
static SRWLOCK g_fenceMirrorLock = SRWLOCK_INIT;
static std::vector<Wrap*> g_fenceMirrors;
static HANDLE g_fenceMirrorWake = nullptr;

static UINT64 FenceReadCompleted(Wrap* w)
{
    const UINT64 value = ((ID3D12Fence*)w->real)->GetCompletedValue();
    InterlockedExchange64((volatile LONG64*)&w->pad[4], (LONG64)value);
    return value;
}
static DWORD WINAPI FenceMirrorWorker(void*)
{
    for (;;) {
        bool active = false;
        {
            ExclusiveSrwLock held(g_fenceMirrorLock);
            active = !g_fenceMirrors.empty();
            for (Wrap* w : g_fenceMirrors) FenceReadCompleted(w);
        }
        WaitForSingleObject(g_fenceMirrorWake, active ? 1 : INFINITE);
    }
}
static ULONG __stdcall FenceAddRef(Wrap* w)
{
    return (ULONG)InterlockedIncrement64((volatile LONG64*)&w->pad[0]);
}
static ULONG __stdcall FenceRelease(Wrap* w)
{
    const LONG64 refs = InterlockedDecrement64((volatile LONG64*)&w->pad[0]);
    if (!refs) {
        // Stop polling before releasing the wrapper's one native COM reference.
        ExclusiveSrwLock held(g_fenceMirrorLock);
        for (auto i = g_fenceMirrors.begin(); i != g_fenceMirrors.end(); ++i)
            if (*i == w) { g_fenceMirrors.erase(i); break; }
        ((IUnknown*)w->real)->Release();
        w->real = nullptr;
    }
    return (ULONG)refs;
}
static HRESULT __stdcall FenceQI(Wrap* w, REFIID iid, void** out)
{
    if (!out) return E_POINTER;
    *out = nullptr;
    void* native = nullptr;
    const HRESULT hr = ((IUnknown*)w->real)->QueryInterface(iid, &native);
    if (FAILED(hr)) return hr;
    // The PC fence's base interfaces share one native identity. Preserve the
    // wrapper's independent COM count while owning exactly one native ref.
    if (native != w->real) { ((IUnknown*)native)->Release(); return E_NOINTERFACE; }
    ((IUnknown*)native)->Release();
    FenceAddRef(w); *out = w;
    return S_OK;
}
static UINT64 __stdcall FenceCompleted8(Wrap* w) { return FenceReadCompleted(w); }
static HRESULT __stdcall FenceSignal10(Wrap* w, UINT64 value)
{
    const HRESULT hr = ((ID3D12Fence*)w->real)->Signal(value);
    if (SUCCEEDED(hr)) FenceReadCompleted(w);
    return hr;
}
static volatile UINT64* __stdcall FenceAddress12(Wrap* w)
{
    ExclusiveSrwLock held(g_fenceMirrorLock);
    if (!g_fenceMirrorWake) {
        g_fenceMirrorWake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!g_fenceMirrorWake) return nullptr;
        // A running worker must not execute code from an unloaded module.
        HMODULE pinned = nullptr;
        if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                (LPCSTR)&FenceMirrorWorker, &pinned)) {
            CloseHandle(g_fenceMirrorWake); g_fenceMirrorWake = nullptr; return nullptr;
        }
        HANDLE worker = CreateThread(nullptr, 0, FenceMirrorWorker, nullptr, 0, nullptr);
        if (!worker) { CloseHandle(g_fenceMirrorWake); g_fenceMirrorWake = nullptr; return nullptr; }
        CloseHandle(worker); // Worker/module live until process exit, fences are removed on final Release.
    }
    if (!w->pad[1]) {
        g_fenceMirrors.push_back(w); w->pad[1] = 1;
        char who[300]; CallerStr(__builtin_return_address(0), who, sizeof who);
        XS_LOG("fence slot12 completion address wrapper=%p real=%p caller=%s (actual PC fence mirror)", (void*)w, w->real, who);
    }
    FenceReadCompleted(w);
    SetEvent(g_fenceMirrorWake);
    return &w->pad[4];
}
