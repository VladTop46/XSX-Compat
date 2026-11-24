#include <windows.h>
#include <d3d12.h>
#include <cstdio>
#include <cstring>
#include <cstdint>

#include "ffx_api.h"
#include "ffx_upscale.h"
#include "xbox/ffx_api_xbox.h"
#include "dx12/ffx_api_dx12.h"
#include "xsx_descriptor_heap_proxy.h"
#include "xsx_debug_trace.h"

using CreateDeviceFn = HRESULT (WINAPI*)(
    IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);

using CreateDescriptorHeapFn = HRESULT (STDMETHODCALLTYPE *)(
    ID3D12Device*, const D3D12_DESCRIPTOR_HEAP_DESC*, REFIID, void**);
static CreateDescriptorHeapFn originalCreateDescriptorHeap = nullptr;
using CreateSRVFn = void (STDMETHODCALLTYPE *)(
    ID3D12Device*, ID3D12Resource*, const D3D12_SHADER_RESOURCE_VIEW_DESC*,
    D3D12_CPU_DESCRIPTOR_HANDLE);
using CreateUAVFn = void (STDMETHODCALLTYPE *)(
    ID3D12Device*, ID3D12Resource*, ID3D12Resource*,
    const D3D12_UNORDERED_ACCESS_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
static CreateSRVFn originalCreateSRV = nullptr;
static CreateUAVFn originalCreateUAV = nullptr;
using CreateRootSignatureFn = HRESULT (STDMETHODCALLTYPE *)(
    ID3D12Device*, UINT, const void*, SIZE_T, REFIID, void**);
static CreateRootSignatureFn originalCreateRootSignature = nullptr;
static bool adaptHeapABI = false;

struct HeapObservation {
    void** output;
    void* real;
    void* exposed;
};
static HeapObservation heaps[16]{}; // This single-threaded smoke path creates five.
static unsigned heapCount = 0;
static void* lastRealResource = nullptr;
static void* lastXboxResource = nullptr;

static void PrintAddress(const char* label, uintptr_t address)
{
    HMODULE module = nullptr;
    char path[MAX_PATH]{};
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(address), &module);
    if (module) GetModuleFileNameA(module, path, MAX_PATH);
    const char* name = strrchr(path, '\\');
    printf("[ABI ADDRESS] %s=%p %s+0x%llx\n", label,
           reinterpret_cast<void*>(address), name ? name + 1 : path,
           static_cast<unsigned long long>(address - reinterpret_cast<uintptr_t>(module)));
}

static bool ReadPointer(uintptr_t address, uintptr_t* out)
{
    SIZE_T read = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(address),
                             out, sizeof(*out), &read) && read == sizeof(*out);
}

static HRESULT STDMETHODCALLTYPE TraceXboxRootSignature(
    ID3D12Device* device, UINT nodeMask, const void* data, SIZE_T size,
    REFIID iid, void** output)
{
    PrintAddress("root signature caller", reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
    printf("[ROOT] enter node=%u data=%p size=%llu IID=%08lX-%04X-%04X output=%p\n",
           nodeMask, data, static_cast<unsigned long long>(size),
           static_cast<unsigned long>(iid.Data1), iid.Data2, iid.Data3, output);
    unsigned char bytes[16]{};
    SIZE_T read = 0;
    const SIZE_T count = size < sizeof(bytes) ? size : sizeof(bytes);
    if (data && count && ReadProcessMemory(GetCurrentProcess(), data, bytes, count, &read)) {
        printf("[ROOT] prefix:");
        for (SIZE_T i = 0; i < read; ++i) printf(" %02X", bytes[i]);
        puts("");
    }
    const HRESULT hr = originalCreateRootSignature(device, nodeMask, data, size, iid, output);
    printf("[ROOT] returned hr=0x%08lX root=%p\n", static_cast<unsigned long>(hr),
           SUCCEEDED(hr) && output ? *output : nullptr);
    return hr;
}

static void STDMETHODCALLTYPE TraceXboxSRV(
    ID3D12Device* device, ID3D12Resource* resource,
    const D3D12_SHADER_RESOURCE_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
    PrintAddress("SRV caller", reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
    printf("[VIEW SRV] enter resource=%p vtable=%p CPU=0x%llx format=%u dimension=%u\n",
           resource, resource ? *reinterpret_cast<void***>(resource) : nullptr,
           static_cast<unsigned long long>(handle.ptr),
           desc ? static_cast<unsigned>(desc->Format) : 0,
           desc ? static_cast<unsigned>(desc->ViewDimension) : 0);
    // Creation now returns the native resource, so VKD3D receives its own object.
    originalCreateSRV(device, resource, desc, handle);
    puts("[VIEW SRV] returned");
}

static void STDMETHODCALLTYPE TraceXboxUAV(
    ID3D12Device* device, ID3D12Resource* resource, ID3D12Resource* counter,
    const D3D12_UNORDERED_ACCESS_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
    PrintAddress("UAV caller", reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
    printf("[VIEW UAV] enter resource=%p counter=%p CPU=0x%llx format=%u dimension=%u\n",
           resource, counter, static_cast<unsigned long long>(handle.ptr),
           desc ? static_cast<unsigned>(desc->Format) : 0,
           desc ? static_cast<unsigned>(desc->ViewDimension) : 0);
    originalCreateUAV(device, resource, counter, desc, handle);
    puts("[VIEW UAV] returned");
}

static LONG CALLBACK TraceAbiFault(EXCEPTION_POINTERS* exception)
{
    if (exception->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
        return EXCEPTION_CONTINUE_SEARCH;
    static LONG active = 0;
    if (InterlockedCompareExchange(&active, 1, 0)) return EXCEPTION_CONTINUE_SEARCH;
    const auto& c = *exception->ContextRecord;
    PrintAddress("fault", c.Rip);
    printf("[ABI FAULT] RCX=%p RDX=%p RAX=%p RSP=%p realResource=%p xboxResource=%p\n",
           reinterpret_cast<void*>(c.Rcx), reinterpret_cast<void*>(c.Rdx),
           reinterpret_cast<void*>(c.Rax), reinterpret_cast<void*>(c.Rsp),
           lastRealResource, lastXboxResource);
    const auto core = reinterpret_cast<uintptr_t>(GetModuleHandleW(L"d3d12core.dll"));
    const auto upscaler = reinterpret_cast<uintptr_t>(
        GetModuleHandleW(L"amd_fidelityfx_upscaler_xbox.dll"));
    uintptr_t caller = 0;
    // This stack calculation is specific to the checked +0x2a9d0 method:
    // sub rsp,0x28; the call itself pushed another eight bytes.
    if (core && upscaler && c.Rip == core + 0x2a9f0 &&
        ReadPointer(c.Rsp + 0x28, &caller) && caller == upscaler + 0x39fb) {
        uintptr_t backend = 0, object = 0, vtable = 0, method = 0;
        if (ReadPointer(c.Rsp + 0x30 + 0x60, &backend) &&
            ReadPointer(backend + 0x28, &object) &&
            ReadPointer(object, &vtable) && ReadPointer(vtable + 0x48, &method)) {
            printf("[ABI OBJECT] backend=%p field=%p object=%p RCX-match=%d vtable=%p\n",
                   reinterpret_cast<void*>(backend), reinterpret_cast<void*>(backend + 0x28),
                   reinterpret_cast<void*>(object), object == c.Rcx,
                   reinterpret_cast<void*>(vtable));
            PrintAddress("object slot9", method);
            for (unsigned i = 0; i < heapCount; ++i) {
                printf("[ABI MATCH] heap[%u] output-field=%d native=%d exposed=%d\n", i,
                       reinterpret_cast<uintptr_t>(heaps[i].output) == backend + 0x28,
                       reinterpret_cast<uintptr_t>(heaps[i].real) == object,
                       reinterpret_cast<uintptr_t>(heaps[i].exposed) == object);
            }
        }
    }
    // Also retain a small raw stack window for a later, different failure.
    for (unsigned i = 0; i < 8; ++i) {
        uintptr_t value = 0;
        if (ReadPointer(c.Rsp + i * sizeof(uintptr_t), &value))
            printf("[ABI STACK] +0x%02x=%p\n", i * 8, reinterpret_cast<void*>(value));
    }
    InterlockedExchange(&active, 0);
    return EXCEPTION_CONTINUE_SEARCH; // Log only; never skip a faulting call.
}

static HRESULT STDMETHODCALLTYPE TraceXboxDescriptorHeap(
    ID3D12Device* device, const D3D12_DESCRIPTOR_HEAP_DESC* desc,
    REFIID iid, void** output)
{
    // The upscaler has loaded by its first heap creation, before pipeline setup.
    XsxDebugTrace::Install(GetModuleHandleW(L"amd_fidelityfx_upscaler_xbox.dll"));
    const HRESULT hr = originalCreateDescriptorHeap(device, desc, iid, output);
    printf("[HEAP CREATE] hr=0x%08lX field=%p object=%p type=%u count=%u flags=0x%X\n",
           static_cast<unsigned long>(hr), output, output ? *output : nullptr,
           desc ? static_cast<unsigned>(desc->Type) : 0,
           desc ? desc->NumDescriptors : 0, desc ? static_cast<unsigned>(desc->Flags) : 0);
    if (FAILED(hr) || !output || !*output ||
        !IsEqualGUID(iid, __uuidof(ID3D12DescriptorHeap))) return hr;
    auto* real = static_cast<ID3D12DescriptorHeap*>(*output);
    ID3D12DescriptorHeap* queried = nullptr;
    const HRESULT qi = real->QueryInterface(__uuidof(ID3D12DescriptorHeap),
                                             reinterpret_cast<void**>(&queried));
    printf("[HEAP IDENTITY] IID=8efb471d-616c-4f49-90f7-127bb763fa51 QI=0x%08lX same=%d\n",
           static_cast<unsigned long>(qi), queried == real);
    if (FAILED(qi) || queried != real) {
        if (queried) queried->Release();
        if (!adaptHeapABI) return hr;
        real->Release();
        *output = nullptr;
        return E_UNEXPECTED;
    }
    queried->Release();
    void** vtable = *reinterpret_cast<void***>(real);
    PrintAddress("heap PC CPU start", reinterpret_cast<uintptr_t>(vtable[9]));
    const auto cpu = real->GetCPUDescriptorHandleForHeapStart();
    printf("[HEAP PC] real=%p vtable=%p CPU=0x%llx\n", real, vtable,
           static_cast<unsigned long long>(cpu.ptr));
    const HRESULT result = adaptHeapABI ? WrapXboxDescriptorHeap(output) : hr;
    if (SUCCEEDED(result) && heapCount < sizeof(heaps) / sizeof(heaps[0]))
        heaps[heapCount++] = {output, real, *output};
    return result;
}

static HRESULT STDMETHODCALLTYPE TraceXboxResource(
    ID3D12Device* device,
    const D3D12_HEAP_PROPERTIES* heap,
    D3D12_HEAP_FLAGS flags,
    const D3D12_RESOURCE_DESC* desc,
    D3D12_RESOURCE_STATES state,
    const D3D12_CLEAR_VALUE* clear,
    const GUID* iid,
    void** output)
{
    puts("\n[ABI] === Xbox resource call ===");

    printf("[ABI] device=%p heap=%p flags=0x%X\n",
           device, heap, (unsigned)flags);

    printf("[ABI] desc=%p state=0x%X clear=%p\n",
           desc, (unsigned)state, clear);

    printf("[ABI] iid=%p output=%p\n", iid, output);

    if (heap) {
        printf("[HEAP] Type=%u CPUPageProperty=%u\n",
               (unsigned)heap->Type,
               (unsigned)heap->CPUPageProperty);
    }

    if (desc) {
        printf("[DESC] Dimension=%u Width=%llu Height=%u\n",
               (unsigned)desc->Dimension,
               (unsigned long long)desc->Width,
               (unsigned)desc->Height);

        printf("[DESC] Format=%u Flags=0x%X\n",
               (unsigned)desc->Format,
               (unsigned)desc->Flags);
        printf("[DESC] Layout=0x%X Alignment=%llu DepthOrArraySize=%u Mips=%u Samples=%u Quality=%u\n",
               static_cast<unsigned>(desc->Layout),
               static_cast<unsigned long long>(desc->Alignment),
               static_cast<unsigned>(desc->DepthOrArraySize),
               static_cast<unsigned>(desc->MipLevels),
               desc->SampleDesc.Count, desc->SampleDesc.Quality);
    }

    if (iid) {
        printf("[IID] {%08lX-%04X-%04X-...}\n",
               (unsigned long)iid->Data1,
               iid->Data2, iid->Data3);
    }

    D3D12_RESOURCE_DESC pcDesc = *desc;

    // Временно удаляем неизвестные Xbox-биты.
    pcDesc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(
        static_cast<unsigned>(desc->Flags) & 0x3FFFFF);

    // Xbox +0x3732 selects the explicit swizzle layout 0x101 for ordinary
    // Texture2D resources. This hook allocates fresh committed memory, so
    // use a PC driver-selected layout. This does not translate pre-tiled data
    // or establish byte-layout equivalence for placed/shared resources.
    if (heap && heap->Type == D3D12_HEAP_TYPE_DEFAULT &&
        flags == D3D12_HEAP_FLAG_NONE &&
        desc->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
        static_cast<unsigned>(desc->Layout) == 0x101) {
        pcDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        puts("[LAYOUT] Experimental Xbox 0x101 -> PC UNKNOWN (fresh committed DEFAULT Texture2D)");
    }

    printf("[ABI] Forwarding to PC ID3D12Device::CreateCommittedResource, flags=0x%X layout=0x%X\n",
           (unsigned)pcDesc.Flags, static_cast<unsigned>(pcDesc.Layout));
    fflush(stdout);

    HRESULT hr = device->CreateCommittedResource(
        heap, flags, &pcDesc, state, clear, *iid, output);

    printf("[ABI] PC CreateCommittedResource hr=0x%08lX resource=%p\n",
           (unsigned long)hr, output ? *output : nullptr);

    if (SUCCEEDED(hr) && output && *output) {
        auto* resource = static_cast<ID3D12Resource*>(*output);

        D3D12_HEAP_PROPERTIES props{};
        D3D12_HEAP_FLAGS heapFlags{};

        HRESULT test = resource->GetHeapProperties(&props, &heapFlags);

        printf("[RESOURCE] GetHeapProperties hr=0x%08lX type=%u\n",
               (unsigned long)test, (unsigned)props.Type);

        auto** resourceVtable =
        *reinterpret_cast<void***>(resource);

        printf("[RESOURCE] Object=%p\n", resource);
        printf("[RESOURCE] VTable=%p\n", resourceVtable);
        printf("[RESOURCE] slot[9]  = %p\n", resourceVtable[9]);
        printf("[RESOURCE] slot[11] = %p\n", resourceVtable[11]);
        printf("[RESOURCE] slot[14] = %p\n", resourceVtable[14]);
        printf("[RESOURCE] PC GetDesc:\n");
        auto** originalResourceVtable =
        *reinterpret_cast<void***>(resource);

        printf("[RESOURCE] Original slot9=%p\n",
               originalResourceVtable[9]);
        auto d = resource->GetDesc();
        printf("[RESOURCE] %llu x %u, format=%u layout=0x%X\n",
               (unsigned long long)d.Width,
               (unsigned)d.Height,
               (unsigned)d.Format, static_cast<unsigned>(d.Layout));
        for (int i = 8; i <= 14; ++i)
            printf("[RESOURCE] vtable[%d] = %p\n",
                   i, resourceVtable[i]);
    }
    if (SUCCEEDED(hr) && output && *output) {
        lastRealResource = *output;
        lastXboxResource = *output;
        // The old resource proxy only diagnosed the suspected slot 9 call.
        // That call is now proven to target a descriptor heap. Keep native
        // resource identity for subsequent SRV/UAV calls into VKD3D.
        printf("[RESOURCE] Returned native ID3D12Resource=%p\n", *output);
    }

    return hr;
}

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);

    if (argc < 2 ||
        (strcmp(argv[1], "--pc") &&
         strcmp(argv[1], "--xbox"))) {
        puts("Usage: ffx-context.exe --pc|--xbox [3|4] [--heap-abi]");
        return 2;
    }

    if (argc > 4 || (argc > 2 && strcmp(argv[2], "3") && strcmp(argv[2], "4")) ||
        (argc > 3 && strcmp(argv[3], "--heap-abi"))) {
        puts("[FAIL] Expected provider 3 or 4 and optional --heap-abi");
        return 2;
    }

    bool xbox = !strcmp(argv[1], "--xbox");
    bool fsr3 = argc > 2 && !strcmp(argv[2], "3");
    adaptHeapABI = argc > 3;
    if (adaptHeapABI && !xbox) {
        puts("[FAIL] --heap-abi requires --xbox");
        return 2;
    }
    void* exceptionTrace = AddVectoredExceptionHandler(1, TraceAbiFault);
    if (!exceptionTrace) {
        printf("[FAIL] AddVectoredExceptionHandler: %lu\n", GetLastError());
        return 9;
    }

    printf("[MODE] %s backend, provider %s\n",
           xbox ? "Xbox" : "DX12",
           fsr3 ? "2.3.4" : "3.1.6");

    HMODULE d3d = LoadLibraryW(L"d3d12.dll");
    if (!d3d) {
        printf("[FAIL] d3d12.dll: %lu\n", GetLastError());
        return 3;
    }

    auto createDevice = reinterpret_cast<CreateDeviceFn>(
        GetProcAddress(d3d, "D3D12CreateDevice"));

    if (!createDevice) {
        puts("[FAIL] D3D12CreateDevice export");
        return 4;
    }

    ID3D12Device* device = nullptr;

    HRESULT hr = createDevice(
        nullptr, D3D_FEATURE_LEVEL_12_0,
        __uuidof(ID3D12Device),
        reinterpret_cast<void**>(&device));

    printf("[D3D12] hr=0x%08lX device=%p\n",
           static_cast<unsigned long>(
               static_cast<uint32_t>(hr)), device);

    if (FAILED(hr) || !device)
        return 5;

    void** vtable = *reinterpret_cast<void***>(device);
    printf("[ABI] Xbox requested slot 76 = %p\n",
           vtable[76]);

    printf("[ABI] Device: %p\n", device);
    printf("[ABI] VTable: %p\n", vtable);

    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(vtable, &mbi, sizeof(mbi))) {
        auto begin = reinterpret_cast<uintptr_t>(vtable);
        auto end = reinterpret_cast<uintptr_t>(mbi.BaseAddress)
        + mbi.RegionSize;

        if (mbi.State == MEM_COMMIT &&
            !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
            begin <= end &&
            end - begin >= 77 * sizeof(void*)) {

            for (int i = 72; i <= 76; ++i) {
                printf("[ABI] vtable[%d] = %p\n",
                       i, vtable[i]);
            }
            } else {
                puts("[ABI] vtable[76] outside readable region");
            }
    }

    HMODULE dll = LoadLibraryW(
        L"amd_fidelityfx_loader_xbox.dll");

    if (!dll) {
        printf("[FAIL] FFX loader: %lu\n", GetLastError());
        device->Release();
        return 6;
    }

    void** originalVtable = *reinterpret_cast<void***>(device);

    // Both the Xbox IID/output-field provenance and the compiled PC call
    // establish CreateDescriptorHeap at byte offset 0x70 (slot 14).
    originalCreateDescriptorHeap =
        reinterpret_cast<CreateDescriptorHeapFn>(originalVtable[14]);
    // Typed PC view-call probes confirm offsets 0x90/0x98. Xbox +0x3a26
    // passes resource, desc and the 8-byte CPU handle with the same ABI.
    originalCreateSRV = reinterpret_cast<CreateSRVFn>(originalVtable[18]);
    originalCreateUAV = reinterpret_cast<CreateUAVFn>(originalVtable[19]);
    // Xbox +0x4f61 uses IID_ID3D12RootSignature with the standard PC signature.
    originalCreateRootSignature = reinterpret_cast<CreateRootSignatureFn>(originalVtable[16]);

    void** xboxVtable = static_cast<void**>(
        VirtualAlloc(nullptr, 77 * sizeof(void*),
                     MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));

    if (!xboxVtable) return 9;

    memcpy(xboxVtable, originalVtable, 77 * sizeof(void*));

    xboxVtable[76] =
    reinterpret_cast<void*>(&TraceXboxResource);
    if (xbox) {
        xboxVtable[14] = reinterpret_cast<void*>(&TraceXboxDescriptorHeap);
        xboxVtable[18] = reinterpret_cast<void*>(&TraceXboxSRV);
        xboxVtable[19] = reinterpret_cast<void*>(&TraceXboxUAV);
        xboxVtable[16] = reinterpret_cast<void*>(&TraceXboxRootSignature);
    }

    *reinterpret_cast<void***>(device) = xboxVtable;

    puts("[ABI] Xbox device slot 76 hooked");
    if (xbox) printf("[ABI] Descriptor heap trace enabled, CPU return adapter=%d\n", adaptHeapABI);
    if (xbox) puts("[ABI] SRV/UAV trace enabled; resources retain native identity");
    if (xbox) puts("[ABI] Root signature trace enabled");

    auto create = reinterpret_cast<PfnFfxCreateContext>(
        GetProcAddress(dll, "ffxCreateContext"));

    auto destroy = reinterpret_cast<PfnFfxDestroyContext>(
        GetProcAddress(dll, "ffxDestroyContext"));

    if (!create || !destroy) {
        puts("[FAIL] FFX exports");
        *reinterpret_cast<void***>(device) = originalVtable;
        device->Release();
        FreeLibrary(dll);
        VirtualFree(xboxVtable, 0, MEM_RELEASE);
        FreeLibrary(d3d);
        RemoveVectoredExceptionHandler(exceptionTrace);
        return 7;
    }

    ffxCreateContextDescUpscale upscale{};
    upscale.header.type =
        FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    upscale.maxRenderSize = {1280, 720};
    upscale.maxUpscaleSize = {1920, 1080};
    upscale.flags = FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;
    if (const char* e = getenv("XSX_FFX_FLAGS")) { upscale.flags = strtoul(e, nullptr, 0); printf("[MODE] flags=0x%X\n", upscale.flags); }
    if (const char* e = getenv("XSX_FFX_SIZES")) { unsigned a,b,c,d; if (sscanf(e,"%ux%u:%ux%u",&a,&b,&c,&d)==4) { upscale.maxRenderSize={a,b}; upscale.maxUpscaleSize={c,d}; printf("[MODE] sizes %ux%u -> %ux%u\n",a,b,c,d);} }

    ffxCreateBackendXboxDesc xboxBackend{};
    xboxBackend.header.type =
        FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_XBOX;
    xboxBackend.device = device;

    ffxCreateBackendDX12Desc pcBackend{};
    pcBackend.header.type =
        FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    pcBackend.device = device;

    ffxCreateContextDescUpscaleVersion version{};
    version.header.type =
        FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
    version.version = FFX_UPSCALER_VERSION;
    if (const char* e = getenv("XSX_FFX_API_VERSION")) {   // формат major.minor.patch
        unsigned a = 0, b = 0, c = 0;
        if (sscanf(e, "%u.%u.%u", &a, &b, &c) == 3) {
            version.version = FFX_UPSCALER_MAKE_VERSION(a, b, c);
            printf("[MODE] app API version overridden: %u.%u.%u (0x%08X)\n", a, b, c, version.version);
        }
    }

    ffxOverrideVersion provider{};
    provider.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
    provider.versionId = fsr3
        ? 0xF5A5CA1E00803004ULL
        : 0xF5A5CA1E00C01006ULL;
    // Игровая сборка FSR4 (game_dlls) экспортирует другой id (ffx-query: 0xF5A5CA1E01001001, "4.1.1").
    if (const char* e = getenv("XSX_FFX_PROVIDER_ID")) {
        provider.versionId = strtoull(e, nullptr, 0);
        printf("[MODE] provider id overridden: 0x%016llX\n", (unsigned long long)provider.versionId);
    }

    // Chains: upscale -> backend -> API version -> provider.
    upscale.header.pNext = xbox
        ? &xboxBackend.header
        : &pcBackend.header;

    if (xbox)
        xboxBackend.header.pNext = &version.header;
    else
        pcBackend.header.pNext = &version.header;

    version.header.pNext = getenv("XSX_FFX_NO_PROVIDER") ? nullptr : &provider.header;

    ffxContext context{};

    puts("[TEST] Entering Xbox ffxCreateContext...");

    ffxReturnCode_t rc = create(
        &context, &upscale.header, nullptr);

    printf("[FFX] CreateContext rc=%d (0x%08X)\n",
           static_cast<int>(rc),
           static_cast<unsigned>(rc));
    // Native provider can propagate the SDK FfxErrorCode through this API.
    // ffx_error.h defines 0x8000000d as FFX_ERROR_BACKEND_API_ERROR.
    if (static_cast<uint32_t>(rc) == 0x8000000dU)
        puts("[FFX] FFX_ERROR_BACKEND_API_ERROR; inspect [XBOX DEBUG] and [ROOT]");

    bool destroyed = false;
    if (rc == 0) {
        const auto drc = destroy(&context, nullptr);
        printf("[FFX] DestroyContext rc=%d\n",
               static_cast<int>(drc));
        destroyed = drc == 0;
        if (destroyed) puts("[PASS] FFX context created and destroyed");
    }

    // Restore the device before its final native Release; keep the clone alive
    // through FFX unload, which can still hold device references.
    *reinterpret_cast<void***>(device) = originalVtable;
    device->Release();
    XsxDebugTrace::Restore();
    FreeLibrary(dll);
    VirtualFree(xboxVtable, 0, MEM_RELEASE);
    FreeLibrary(d3d);
    RemoveVectoredExceptionHandler(exceptionTrace);

    const int exitCode = rc == 0 && destroyed ? 0 : 8;
    printf("[TEST] process exit=%d\n", exitCode);
    return exitCode;
}
