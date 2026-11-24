// Bootstrap-тест d3d12_xs.dll: ТОЛЬКО через экспорты d3d12_xs (как это делает игра).
// Проверяет: экспорты, root signature serialize->deserialize roundtrip (1.1 и 1.0),
// D3D12XboxCreateDevice -> реальный PC device -> CreateRootSignature/Queue/Fence/Buffer,
// негативные случаи, и фиксирует границу Xbox/PC device ABI (см. README отчёта).
#define INITGUID
#include <windows.h>
#include <d3d12.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#include <algorithm>

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...) do { if (cond) { g_pass++; printf("[PASS] " __VA_ARGS__); } \
    else { g_fail++; printf("[FAIL] " __VA_ARGS__); } putchar('\n'); } while (0)

using SerFn   = HRESULT (WINAPI*)(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC*, ID3DBlob**, ID3DBlob**);
using DeserFn = HRESULT (WINAPI*)(const void*, SIZE_T, REFIID, void**);
using CreateDevFn = HRESULT (WINAPI*)(IUnknown*, const void*, REFIID, void**);
using VmFn    = HRESULT (WINAPI*)(const void*);
#include "d3d12-xs-ia-probe.h"

struct WrapperStress {
    ID3D12Device* device;
    HANDLE start;
    uintptr_t* pointers;
    volatile LONG errors;
};
static DWORD WINAPI StressWrapperThread(void* opaque)
{
    auto& run = *(WrapperStress*)opaque;
    using Native86 = HRESULT (WINAPI*)(void*, const void*, REFIID, void**);
    const auto create = (Native86)(*reinterpret_cast<void***>(run.device))[86];
    WaitForSingleObject(run.start, INFINITE);
    for (unsigned i = 0; i < 4096; ++i) {
        // Exercise the EXISTING empty-PSO fallback solely as a COM/arena
        // lifetime regression. No native shader execution is claimed here.
        ID3D12PipelineState* pso = nullptr;
        const HRESULT hr = create(run.device, nullptr, IID_ID3D12PipelineState, (void**)&pso);
        if (FAILED(hr) || !pso) { InterlockedIncrement(&run.errors); continue; }
        run.pointers[i] = (uintptr_t)pso;
        IUnknown* alias = nullptr;
        if (FAILED(pso->QueryInterface(__uuidof(IUnknown), (void**)&alias)) || alias != pso)
            InterlockedIncrement(&run.errors);
        if (alias) alias->Release();
        if (!pso->Release()) InterlockedIncrement(&run.errors); // Cache must retain its own reference.
    }
    return 0;
}

static D3D12_VERSIONED_ROOT_SIGNATURE_DESC MakeDesc11(std::vector<D3D12_ROOT_PARAMETER1>& params,
    std::vector<D3D12_DESCRIPTOR_RANGE1>& ranges, D3D12_STATIC_SAMPLER_DESC& smp)
{
    ranges.resize(2);
    ranges[0] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 4, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE, 0 };
    ranges[1] = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_NONE, 4 };
    params.resize(3);
    params[0] = {}; params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor = { 0, 0, D3D12_ROOT_DESCRIPTOR_FLAG_NONE };
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1] = {}; params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants = { 1, 0, 8 };
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[2] = {}; params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable = { 2, ranges.data() };
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    smp = {};
    smp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    smp.AddressU = smp.AddressV = smp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    smp.MaxLOD = D3D12_FLOAT32_MAX; smp.ShaderRegister = 0;
    smp.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    smp.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    smp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC d = {};
    d.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    d.Desc_1_1 = { 3, params.data(), 1, &smp, D3D12_ROOT_SIGNATURE_FLAG_NONE };
    return d;
}

int main()
{
    HMODULE xs = LoadLibraryW(L"d3d12_xs.dll");
    CHECK(xs, "LoadLibrary d3d12_xs.dll (err=%lu)", GetLastError());
    if (!xs) return 2;
    auto ser  = (SerFn)GetProcAddress(xs, "D3D12SerializeVersionedRootSignature");
    auto des  = (DeserFn)GetProcAddress(xs, "D3D12CreateRootSignatureDeserializer");
    auto cdev = (CreateDevFn)GetProcAddress(xs, "D3D12XboxCreateDevice");
    auto vm   = (VmFn)GetProcAddress(xs, "D3D12ConfigureVirtualMemoryX");
    CHECK(ser && des && cdev && vm, "4 exports resolved (ser=%p des=%p dev=%p vm=%p)", (void*)ser, (void*)des, (void*)cdev, (void*)vm);
    if (!(ser && des && cdev && vm)) return 2;

    // --- VM: порядок вызова до device, как предполагается для игры ---
    // Форма вызова игры: 32-байтовая обнулённая структура, dword[1] = N+2 (callsite RVA 0x1e1cfe0).
    UINT vmcfg[8] = {}; vmcfg[1] = 3;
    HRESULT hrVm = vm(vmcfg);
    CHECK(SUCCEEDED(hrVm), "ConfigureVirtualMemoryX(game-shaped cfg) = 0x%08lX (игра: fail => int3)", (unsigned long)hrVm);

    // --- Root signature roundtrip 1.1 ---
    std::vector<D3D12_ROOT_PARAMETER1> p; std::vector<D3D12_DESCRIPTOR_RANGE1> r; D3D12_STATIC_SAMPLER_DESC smp;
    auto d11 = MakeDesc11(p, r, smp);
    ID3DBlob *blob = nullptr, *err = nullptr;
    HRESULT hr = ser(&d11, &blob, &err);
    CHECK(SUCCEEDED(hr) && blob && blob->GetBufferSize() > 0, "Serialize 1.1 hr=0x%08lX size=%zu", (unsigned long)hr, blob ? (size_t)blob->GetBufferSize() : 0);
    if (!blob) return 1;
    CHECK(!memcmp(blob->GetBufferPointer(), "DXBC", 4), "blob is DXBC container");

    ID3D12RootSignatureDeserializer* dz = nullptr;
    hr = des(blob->GetBufferPointer(), blob->GetBufferSize(), IID_ID3D12RootSignatureDeserializer, (void**)&dz);
    CHECK(SUCCEEDED(hr) && dz, "Deserialize (1.0 iface) hr=0x%08lX", (unsigned long)hr);
    if (dz) {
        D3D12_ROOT_SIGNATURE_DESC dd; dd = *dz->GetRootSignatureDesc();   // MinGW aggregate-ABI safe copy
        CHECK(dd.NumParameters == 3 && dd.NumStaticSamplers == 1, "params=%u samplers=%u", dd.NumParameters, dd.NumStaticSamplers);
        CHECK(dd.pParameters[0].ParameterType == D3D12_ROOT_PARAMETER_TYPE_CBV, "param0 CBV");
        CHECK(dd.pParameters[1].ParameterType == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS && dd.pParameters[1].Constants.Num32BitValues == 8, "param1 8 root constants");
        CHECK(dd.pParameters[2].ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE &&
              dd.pParameters[2].DescriptorTable.NumDescriptorRanges == 2 &&
              dd.pParameters[2].DescriptorTable.pDescriptorRanges[0].NumDescriptors == 4 &&
              dd.pParameters[2].DescriptorTable.pDescriptorRanges[1].RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_UAV, "param2 table SRV(4)+UAV");
        // idempotence: 1.0 -> serialize -> deserialize даёт то же описание
        D3D12_VERSIONED_ROOT_SIGNATURE_DESC v10 = {}; v10.Version = D3D_ROOT_SIGNATURE_VERSION_1_0; v10.Desc_1_0 = dd;
        ID3DBlob* b2 = nullptr; ID3DBlob* e2 = nullptr;
        hr = ser(&v10, &b2, &e2);
        CHECK(SUCCEEDED(hr) && b2, "Serialize 1.0 of deserialized desc hr=0x%08lX", (unsigned long)hr);
        if (b2) {
            ID3D12RootSignatureDeserializer* dz2 = nullptr;
            hr = des(b2->GetBufferPointer(), b2->GetBufferSize(), IID_ID3D12RootSignatureDeserializer, (void**)&dz2);
            bool same = false;
            if (SUCCEEDED(hr) && dz2) { D3D12_ROOT_SIGNATURE_DESC d2; d2 = *dz2->GetRootSignatureDesc();
                same = d2.NumParameters == dd.NumParameters && d2.NumStaticSamplers == dd.NumStaticSamplers; dz2->Release(); }
            CHECK(same, "1.0 roundtrip stable");
            b2->Release();
        }
        dz->Release();
    }
    // versioned IID через тот же export (Xbox FFX использует только 1.0 IID C54A6B66)
    ID3D12VersionedRootSignatureDeserializer* vdz = nullptr;
    hr = des(blob->GetBufferPointer(), blob->GetBufferSize(), IID_ID3D12VersionedRootSignatureDeserializer, (void**)&vdz);
    printf("[INFO] Deserialize via versioned IID hr=0x%08lX\n", (unsigned long)hr);
    if (vdz) vdz->Release();

    // --- Форма вызова игры #1 (RVA 0x247fc80): пустая 1.1 LOCAL root signature, flags=0x80 (DXR) ---
    { D3D12_VERSIONED_ROOT_SIGNATURE_DESC ld = {}; ld.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
      ld.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE;
      ID3DBlob* lb = nullptr; ID3DBlob* le = nullptr; hr = ser(&ld, &lb, &le);
      CHECK(SUCCEEDED(hr) && lb && lb->GetBufferSize() > 0, "Serialize empty LOCAL root sig (game DXR) hr=0x%08lX size=%zu", (unsigned long)hr, lb ? (size_t)lb->GetBufferSize() : 0);
      if (lb) lb->Release(); if (le) le->Release(); }

    // --- Негативные случаи ---
    ID3D12RootSignatureDeserializer* bad = nullptr;
    char junk[64] = "not a root signature";
    hr = des(junk, sizeof junk, IID_ID3D12RootSignatureDeserializer, (void**)&bad);
    CHECK(FAILED(hr) && !bad, "garbage rejected hr=0x%08lX", (unsigned long)hr);
    hr = cdev(nullptr, nullptr, IID_ID3D12Device, nullptr);
    CHECK(hr == E_INVALIDARG, "CreateDevice(out=NULL) = E_INVALIDARG (0x%08lX)", (unsigned long)hr);

    // --- Настоящий create-device ---
    // Форма вызова игры (callsite RVA 0x24785b7): 0x50 байт, +0=0x20004, +8=0x100000, +0x28=1, +0x3c=0x20;
    // riid = 5405c344-d457-444e-b4dd-2366e45aee39 (ID3D12Device11, по IID в vkd3d-proton), adapter=NULL.
    UINT params[20] = {}; params[0] = 0x20004; params[2] = 0x100000; params[10] = 1; params[15] = 0x20;
    static const GUID IID_Dev11 = {0x5405c344,0xd457,0x444e,{0xb4,0xdd,0x23,0x66,0xe4,0x5a,0xee,0x39}};
    ID3D12Device* dev = nullptr;
    hr = cdev(nullptr, params, IID_Dev11, (void**)&dev);
    CHECK(SUCCEEDED(hr) && dev, "XboxCreateDevice(IID Device11, game-shaped params) hr=0x%08lX", (unsigned long)hr);
    if (dev) { dev->Release(); dev = nullptr; }
    hr = cdev(nullptr, params, IID_ID3D12Device, (void**)&dev);
    CHECK(SUCCEEDED(hr) && dev, "D3D12XboxCreateDevice hr=0x%08lX dev=%p", (unsigned long)hr, (void*)dev);
    if (!dev) { printf("\nSUMMARY pass=%d fail=%d\n", g_pass, g_fail); return 1; }
    const ULONG with_extra_ref = dev->AddRef(); dev->Release();
    IUnknown* device_alias = nullptr;
    hr = dev->QueryInterface(__uuidof(IUnknown), (void**)&device_alias);
    const ULONG after_alias_release = device_alias ? device_alias->Release() : 0;
    CHECK(SUCCEEDED(hr) && device_alias == dev && after_alias_release == with_extra_ref - 1,
          "device QueryInterface retains one owned reference and wrapper identity");

    ID3D12RootSignature* rs = nullptr;
    hr = dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_ID3D12RootSignature, (void**)&rs);
    CHECK(SUCCEEDED(hr) && rs, "CreateRootSignature from d3d12_xs-serialized blob hr=0x%08lX", (unsigned long)hr);

    D3D12_COMMAND_QUEUE_DESC qd = {}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* q = nullptr; ID3D12CommandAllocator* a = nullptr; ID3D12GraphicsCommandList* l = nullptr; ID3D12Fence* f = nullptr;
    HRESULT h1 = dev->CreateCommandQueue(&qd, IID_ID3D12CommandQueue, (void**)&q);
    HRESULT h2 = dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_ID3D12CommandAllocator, (void**)&a);
    HRESULT h3 = a ? dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, a, nullptr, IID_ID3D12GraphicsCommandList, (void**)&l) : E_FAIL;
    HRESULT h4 = dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_ID3D12Fence, (void**)&f);
    CHECK(SUCCEEDED(h1) && SUCCEEDED(h2) && SUCCEEDED(h3) && SUCCEEDED(h4), "queue/allocator/list/fence (%08lX %08lX %08lX %08lX)",
          (unsigned long)h1, (unsigned long)h2, (unsigned long)h3, (unsigned long)h4);
    if (q && l && f) {
        l->Close();
        ID3D12CommandList* lists[] = { l };
        q->ExecuteCommandLists(1, lists);
        q->Signal(f, 1);
        HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        f->SetEventOnCompletion(1, ev);
        DWORD w = WaitForSingleObject(ev, 5000);
        CHECK(w == WAIT_OBJECT_0 && f->GetCompletedValue() >= 1, "empty list executed, fence signalled");
        void** qvt = *reinterpret_cast<void***>(q);
        UINT64 pc_frequency = 0, xbox_frequency = 0;
        const HRESULT pf = q->GetTimestampFrequency(&pc_frequency);
        const HRESULT xf = ((HRESULT (WINAPI*)(void*, UINT64*))qvt[18])(q, &xbox_frequency);
        CHECK(SUCCEEDED(pf) && SUCCEEDED(xf) && pc_frequency && pc_frequency == xbox_frequency,
              "Xbox queue slot18 timestamp frequency equals PC (%llu)", (unsigned long long)xbox_frequency);
        const HRESULT executed = ((HRESULT (WINAPI*)(void*, UINT, ID3D12CommandList* const*, UINT64, UINT64))qvt[35])(q, 1, lists, 0, 0);
        q->Signal(f, 2);
        ResetEvent(ev); f->SetEventOnCompletion(2, ev);
        w = WaitForSingleObject(ev, 5000);
        CHECK(SUCCEEDED(executed) && w == WAIT_OBJECT_0 && f->GetCompletedValue() >= 2,
              "Xbox queue slot35 unwraps list and executes, fence=2");
        // Xbox fence12 exposes a pollable completion address. Mirror only the
        // real native completion value, with actual PC queue Signal/Wait calls.
        ID3D12Fence* monitor = nullptr;
        const HRESULT created_monitor = dev->CreateFence(17, D3D12_FENCE_FLAG_NONE, IID_ID3D12Fence, (void**)&monitor);
        volatile UINT64* address = nullptr;
        if (monitor) {
            void** fence_vt = *reinterpret_cast<void***>(monitor);
            address = ((volatile UINT64* (WINAPI*)(void*))fence_vt[12])(monitor);
        }
        CHECK(SUCCEEDED(created_monitor) && address && *address == 17,
              "Xbox fence12 returns initialized actual completion address");
        if (monitor && address) {
            IUnknown* alias = nullptr;
            const HRESULT queried = monitor->QueryInterface(__uuidof(IUnknown), (void**)&alias);
            const ULONG kept = alias ? alias->Release() : 0;
            CHECK(SUCCEEDED(queried) && alias == monitor && kept == 1,
                  "fence QI identity and owned reference retained");
            const HRESULT signalled_cpu = monitor->Signal(18);
            CHECK(SUCCEEDED(signalled_cpu) && *address == 18 && monitor->GetCompletedValue() == 18,
                  "CPU fence Signal synchronizes completion mirror");
            ID3D12CommandQueue* second = nullptr;
            const HRESULT created_second = dev->CreateCommandQueue(&qd, IID_ID3D12CommandQueue, (void**)&second);
            CHECK(SUCCEEDED(created_second) && second, "second queue created for fence Signal/Wait unwrap");
            bool gpu_mirror = false;
            if (second) {
                const HRESULT signal_first = q->Signal(monitor, 19);
                const HRESULT wait_second = second->Wait(monitor, 19);
                const HRESULT signal_second = second->Signal(monitor, 20);
                ResetEvent(ev);
                const HRESULT set_event = monitor->SetEventOnCompletion(20, ev);
                const DWORD completed = WaitForSingleObject(ev, 5000);
                // Do not call GetCompletedValue until AFTER observing the raw
                // address: that accessor could otherwise hide a broken worker.
                const ULONGLONG deadline = GetTickCount64() + 5000;
                while (*address != 20 && GetTickCount64() < deadline) Sleep(1);
                gpu_mirror = SUCCEEDED(signal_first) && SUCCEEDED(wait_second) && SUCCEEDED(signal_second) &&
                    SUCCEEDED(set_event) && completed == WAIT_OBJECT_0 && *address == 20 && monitor->GetCompletedValue() == 20;
                second->Release();
            }
            CHECK(gpu_mirror, "GPU Signal/Wait completes, raw fence address reaches 20 without CPU accessor");
            void* fence_vtable = *(void**)monitor;
            const HRESULT xbox_signal = ((HRESULT (WINAPI*)(void*, ID3D12Fence*, UINT64))qvt[16])(q, monitor, 21);
            const ULONGLONG xbox_deadline = GetTickCount64() + 5000;
            while (*address != 21 && GetTickCount64() < xbox_deadline) Sleep(1);
            CHECK(SUCCEEDED(xbox_signal) && *(void**)monitor == fence_vtable && *address == 21 && monitor->GetCompletedValue() == 21,
                  "Xbox queue16 Signal completes on GPU, fence vtable retained (NOT timestamp output)");
            const ULONG remaining = monitor->Release(); monitor = nullptr;
            CHECK(remaining == 0, "final fence Release unregisters polling mirror");
        }
        if (monitor) monitor->Release();
        // Keep the PC query API functional while separating incompatible Xbox52.
        D3D12_QUERY_HEAP_DESC query_desc{};
        query_desc.Type = D3D12_QUERY_HEAP_TYPE_OCCLUSION; query_desc.Count = 1;
        ID3D12QueryHeap* query = nullptr;
        const HRESULT created_query = dev->CreateQueryHeap(&query_desc, IID_ID3D12QueryHeap, (void**)&query);
        CHECK(SUCCEEDED(created_query) && query, "PC query heap created");
        D3D12_HEAP_PROPERTIES readback_heap{}; readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC readback_desc{};
        readback_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; readback_desc.Width = 64;
        readback_desc.Height = readback_desc.DepthOrArraySize = readback_desc.MipLevels = 1;
        readback_desc.SampleDesc.Count = 1; readback_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ID3D12Resource* query_result = nullptr;
        const HRESULT created_result = dev->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE,
            &readback_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_ID3D12Resource, (void**)&query_result);
        D3D12_HEAP_PROPERTIES seed_heap{}; seed_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        ID3D12Resource* seed = nullptr;
        const HRESULT created_seed = dev->CreateCommittedResource(&seed_heap, D3D12_HEAP_FLAG_NONE,
            &readback_desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_ID3D12Resource, (void**)&seed);
        CHECK(SUCCEEDED(created_result) && query_result && SUCCEEDED(created_seed) && seed,
              "PC query readback and sentinel buffers created");
        if (query && query_result && seed) {
            UINT64* seed_data = nullptr; D3D12_RANGE no_read{0, 0};
            const HRESULT seed_map = seed->Map(0, &no_read, (void**)&seed_data);
            CHECK(SUCCEEDED(seed_map) && seed_data, "query sentinel initialized");
            if (seed_data) { for (unsigned i = 0; i < 8; ++i) seed_data[i] = ~UINT64(0); seed->Unmap(0, nullptr); }
            a->Reset(); l->Reset(a, nullptr);
            l->SetGraphicsRootSignature(rs);
            l->SetGraphicsRoot32BitConstant(1, 0x12345678, 0);
            l->CopyBufferRegion(query_result, 0, seed, 0, 64);
            l->BeginQuery(query, D3D12_QUERY_TYPE_OCCLUSION, 0);
            l->EndQuery(query, D3D12_QUERY_TYPE_OCCLUSION, 0);
            l->ResolveQueryData(query, D3D12_QUERY_TYPE_OCCLUSION, 0, 1, query_result, 0);
            l->Close(); q->ExecuteCommandLists(1, lists); q->Signal(f, 3);
            ResetEvent(ev); f->SetEventOnCompletion(3, ev);
            w = WaitForSingleObject(ev, 5000);
            UINT64* result = nullptr; D3D12_RANGE range{0, 64};
            const HRESULT mapped = w == WAIT_OBJECT_0 ? query_result->Map(0, &range, (void**)&result) : E_FAIL;
            bool guarded = result != nullptr;
            for (unsigned i = 1; guarded && i < 8; ++i) guarded = result[i] == ~UINT64(0);
            CHECK(SUCCEEDED(mapped) && result && *result == 0 && guarded,
                  "PC Begin/End/Resolve GPU query overwrites sentinel, 7 guards intact");
            if (result) query_result->Unmap(0, nullptr);
            // The known Xbox call shape must never reach PC BeginQuery.
            a->Reset();
            UINT xbox_desc[6]{}; ID3D12GraphicsCommandList* xbox_list = nullptr;
            void** device_vt = *reinterpret_cast<void***>(dev);
            const HRESULT created_xbox = ((HRESULT (WINAPI*)(void*, const UINT*, ID3D12CommandAllocator*,
                ID3D12PipelineState*, REFIID, void**))device_vt[73])(dev, xbox_desc, a, nullptr,
                IID_ID3D12GraphicsCommandList, (void**)&xbox_list);
            CHECK(SUCCEEDED(created_xbox) && xbox_list, "Xbox descriptor path creates tagged list");
            if (xbox_list) {
                void** list_vt = *reinterpret_cast<void***>(xbox_list);
                auto* inline_state = (unsigned char*)xbox_list;
                const UINT64 cursor = *(const UINT64*)(inline_state + 0x18);
                const UINT64 limit = *(const UINT64*)(inline_state + 0x28);
                CHECK(cursor == 0 && limit == 0, "Xbox inline draw cursor/limit are empty; game selects virtual Draw");
                ID3D12Fence* neighbor = nullptr;
                const HRESULT created_neighbor = dev->CreateFence(123, D3D12_FENCE_FLAG_NONE, IID_ID3D12Fence, (void**)&neighbor);
                const bool separated = neighbor && (uintptr_t)neighbor >= (uintptr_t)xbox_list + 0x1000;
                // Reproduce the game's ordinary inline list-state writes;
                // they must not overwrite another arena object's vtable.
                if (separated) {
                    inline_state[0x140] = 1; inline_state[0x142] = 1;
                    *(UINT*)(inline_state + 0x544) = 0x11223344;
                    *(UINT*)(inline_state + 0x564) = 0x55667788;
                }
                CHECK(SUCCEEDED(created_neighbor) && separated && neighbor->GetCompletedValue() == 123,
                      "Xbox list state through +0x564 is isolated from following COM object");
                if (neighbor) neighbor->Release();
                UINT64 descriptor[4]{};
                ((void (WINAPI*)(void*, ID3D12RootSignature*, UINT, UINT))list_vt[34])(
                    xbox_list, rs, 0, 0);
                ((void (WINAPI*)(void*, void*, uintptr_t, uintptr_t, uintptr_t))list_vt[52])(
                    xbox_list, descriptor, (uintptr_t)(descriptor + 1), 0, 0);
                const HRESULT closed = xbox_list->Close();
                ID3D12CommandList* xbox_lists[]{xbox_list};
                ((HRESULT (WINAPI*)(void*, UINT, ID3D12CommandList* const*, UINT64, UINT64))qvt[35])(q, 1, xbox_lists, 0, 0);
                q->Signal(f, 4); ResetEvent(ev); f->SetEventOnCompletion(4, ev);
                w = WaitForSingleObject(ev, 5000);
                CHECK(SUCCEEDED(closed) && w == WAIT_OBJECT_0 && f->GetCompletedValue() >= 4,
                      "Xbox34 root signature binding and unsupported Xbox52 never reach wrong PC APIs");
                xbox_list->Release();
            }
        }
        if (query) query->Release();
        if (query_result) query_result->Release();
        if (seed) seed->Release();
        CloseHandle(ev);
    }
    CHECK(RunInputAssemblerProbe(dev, ser, false), "PC clear, IA, descriptor table and root constants produce expected pixels");
    CHECK(RunInputAssemblerProbe(dev, ser, true), "Xbox list36/40/47/48 table/constants/index/vertex bindings produce expected pixels");
    // upload buffer map/write/read
    D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC rd = {}; rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width = 65536; rd.Height = 1;
    rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* buf = nullptr;
    hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_ID3D12Resource, (void**)&buf);
    CHECK(SUCCEEDED(hr) && buf, "PC CreateCommittedResource (PC vtable slot 27) hr=0x%08lX", (unsigned long)hr);
    if (buf) { void* m = nullptr; D3D12_RANGE none = {0,0};
        bool ok = SUCCEEDED(buf->Map(0, &none, &m)) && m;
        if (ok) { *(UINT*)m = 0xC0FFEE; ok = *(UINT*)m == 0xC0FFEE; buf->Unmap(0, nullptr); }
        CHECK(ok, "buffer map/write/readback"); buf->Release(); }


    // --- Xbox-ABI вызовы через vtable обёртки (то, как их делает игра) ---
    {
        void** xvt = *reinterpret_cast<void***>(dev);
        struct HP { UINT Type, Cpu, Pool, Cn, Vn; };
        typedef HP* (WINAPI* PcFn)(void* self, HP* ret, UINT node, UINT type);       // PC/VKD3D-порядок
        typedef HP* (WINAPI* XbFn)(HP* ret, void* self, UINT node, UINT type);       // Xbox-порядок (доказан в игре): RCX=буфер, RDX=this
        HP a = {}, b = {}; ((PcFn)xvt[26])(dev, &a, 1, 1); ((XbFn)xvt[26])(&b, dev, 1, 1);
        CHECK(a.Type != 0 && !memcmp(&a, &b, sizeof a), "slot26 GetCustomHeapProperties: PC-порядок == Xbox-порядок (Type=%u Cpu=%u Pool=%u)", a.Type, a.Cpu, a.Pool);
        void* vtBefore = *(void**)dev; HP c = {}; ((XbFn)xvt[26])(&c, dev, 1, 2); CHECK(*(void**)dev == vtBefore, "Xbox-порядок НЕ затирает vtable обёртки");
        uint64_t luid = ((uint64_t (WINAPI*)(void*))xvt[43])(dev); printf("[INFO] slot43 GetAdapterLuid (RAX) = %#llx\n", (unsigned long long)luid);
        UINT inc = ((UINT (WINAPI*)(void*, UINT))xvt[15])(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV); CHECK(inc > 0, "slot15 GetDescriptorHandleIncrementSize forward = %u", inc);
        D3D12_RESOURCE_DESC xrd = {}; xrd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; xrd.Width = 4096; xrd.Height = 1; xrd.DepthOrArraySize = 1; xrd.MipLevels = 1; xrd.SampleDesc.Count = 1; xrd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; xrd.Flags = (D3D12_RESOURCE_FLAGS)0xC00000;
        D3D12_HEAP_PROPERTIES xhp = {}; xhp.Type = D3D12_HEAP_TYPE_UPLOAD; ID3D12Resource* xr = nullptr;
        HRESULT hx = ((HRESULT (WINAPI*)(void*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**))xvt[76])(dev, &xhp, D3D12_HEAP_FLAG_NONE, &xrd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_ID3D12Resource, (void**)&xr);
        CHECK(SUCCEEDED(hx) && xr, "slot76 CreateCommittedResource (Xbox flags 0xC00000 -> маска) hr=0x%08lX", (unsigned long)hx); if (xr) xr->Release();
        uintptr_t un = ((uintptr_t (WINAPI*)(void*, uintptr_t, uintptr_t, uintptr_t))xvt[100])(dev, 1, 2, 3); CHECK(un == 0, "незамапленный слот 100 -> заглушка, возврат 0 (см. лог)");
        // Native PSO methods must never let an uninitialized stack value enter
        // the game's COM cache. Check both register and stack output arguments.
        using Native57 = HRESULT (WINAPI*)(void*, const void*, REFIID, void**);
        using Updates55 = HRESULT (WINAPI*)(void*, ID3D12PipelineState*, UINT, const void*, REFIID, void**);
        auto native57 = (Native57)xvt[57]; auto updates55 = (Updates55)xvt[55];
        struct { uintptr_t before; void* output; uintptr_t after; } result{0x12345678, (void*)0x1234, 0x87654321};
        uint64_t opaque[32]{}; // Opaque data; never PC bytecode or a fake PSO.
        const auto guards_ok = [&] { return result.before == 0x12345678 && result.after == 0x87654321; };
        hr = native57(dev, opaque, IID_ID3D12PipelineState, &result.output);
        CHECK(hr == E_NOTIMPL && !result.output && guards_ok(), "Xbox57 unsupported native PSO clears stale output, guards intact");
        result.output = (void*)0x1234;
        hr = native57(dev, opaque, IID_ID3D12Resource, &result.output);
        CHECK(hr == E_NOINTERFACE && !result.output && guards_ok(), "Xbox57 rejects wrong IID and clears output");
        result.output = (void*)0x1234;
        hr = native57(dev, nullptr, IID_ID3D12PipelineState, &result.output);
        CHECK(hr == E_INVALIDARG && !result.output && guards_ok() &&
              native57(dev, opaque, IID_ID3D12PipelineState, nullptr) == E_POINTER, "Xbox57 rejects null description/output");
        result.output = (void*)0x1234;
        hr = updates55(dev, nullptr, 1, opaque, IID_ID3D12PipelineState, &result.output);
        CHECK(hr == E_INVALIDARG && !result.output && guards_ok(), "Xbox55 stack output is cleared for invalid base PSO");
        CHECK(updates55(dev, nullptr, 0, nullptr, IID_ID3D12PipelineState, nullptr) == E_POINTER,
              "Xbox55 rejects null stack output");
        // Exact rejected game description, compared to a legal PC depth target.
        D3D12_RESOURCE_DESC depth{};
        depth.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        depth.Width = 2560; depth.Height = 1440;
        depth.DepthOrArraySize = depth.MipLevels = depth.SampleDesc.Count = 1;
        depth.Format = DXGI_FORMAT_R32G8X24_TYPELESS;
        depth.Flags = (D3D12_RESOURCE_FLAGS)0x80300006;
        D3D12_RESOURCE_DESC pc_depth = depth;
        pc_depth.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        const auto expected = dev->GetResourceAllocationInfo(1, 1, &pc_depth);
        D3D12_RESOURCE_ALLOCATION_INFO allocation{};
        using XboxInfo25 = D3D12_RESOURCE_ALLOCATION_INFO* (WINAPI*)(D3D12_RESOURCE_ALLOCATION_INFO*, void*, UINT, UINT, const D3D12_RESOURCE_DESC*);
        ((XboxInfo25)xvt[25])(&allocation, dev, 1, 1, &depth);
        CHECK(allocation.SizeInBytes && allocation.SizeInBytes != ~UINT64(0) &&
              allocation.SizeInBytes == expected.SizeInBytes && allocation.Alignment == expected.Alignment,
              "Xbox depth allocation equals legal PC depth target (size=%#llx)", (unsigned long long)allocation.SizeInBytes);
        D3D12_RESOURCE_ALLOCATION_INFO1 allocation1{};
        using XboxInfo108 = D3D12_RESOURCE_ALLOCATION_INFO* (WINAPI*)(D3D12_RESOURCE_ALLOCATION_INFO*, void*, UINT, UINT, const D3D12_RESOURCE_DESC*, void*);
        D3D12_RESOURCE_ALLOCATION_INFO via108{};
        ((XboxInfo108)xvt[108])(&via108, dev, 1, 1, &depth, &allocation1);
        CHECK(via108.SizeInBytes == expected.SizeInBytes && via108.Alignment == expected.Alignment &&
              allocation1.SizeInBytes == expected.SizeInBytes && allocation1.Offset == 0,
              "Xbox108 depth allocation and per-resource info agree with Xbox25");
        D3D12_RESOURCE_DESC invalid_pc = pc_depth;
        invalid_pc.Flags = (D3D12_RESOURCE_FLAGS)6;
        const auto rejected = dev->GetResourceAllocationInfo(1, 1, &invalid_pc);
        CHECK(rejected.SizeInBytes == ~UINT64(0), "ordinary PC depth+UAV remains invalid");
        D3D12_HEAP_PROPERTIES gpu_heap{}; gpu_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        ID3D12Resource* depth_resource = nullptr;
        hr = ((HRESULT (WINAPI*)(void*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC*,
            D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**))xvt[76])(
            dev, &gpu_heap, D3D12_HEAP_FLAG_NONE, &depth, D3D12_RESOURCE_STATE_DEPTH_WRITE,
            nullptr, IID_ID3D12Resource, (void**)&depth_resource);
        CHECK(SUCCEEDED(hr) && depth_resource, "Xbox depth-only backing created (UAV unsupported) hr=%#lx", (unsigned long)hr);
        bool correct_depth = false;
        if (depth_resource) {
            const auto created = depth_resource->GetDesc();
            correct_depth = created.Flags == pc_depth.Flags && created.Format == depth.Format &&
                created.Width == depth.Width && created.Height == depth.Height;
            depth_resource->Release();
        }
        CHECK(correct_depth, "depth resource uses exactly the description used for allocation");
        D3D12_RESOURCE_DESC ffx = pc_depth;
        ffx.Width = 1280; ffx.Height = 720; ffx.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        ffx.Flags = (D3D12_RESOURCE_FLAGS)0xc00005;
        const auto ffx_alloc = dev->GetResourceAllocationInfo(1, 1, &ffx);
        ffx.Flags = (D3D12_RESOURCE_FLAGS)5;
        const auto ffx_pc = dev->GetResourceAllocationInfo(1, 1, &ffx);
        CHECK(ffx_alloc.SizeInBytes != ~UINT64(0) && ffx_alloc.SizeInBytes == ffx_pc.SizeInBytes &&
              ffx_alloc.Alignment == ffx_pc.Alignment, "FFX render-target+UAV allocation behavior retained");
    }
    // --- Граница ABI: что именно возвращает XboxCreateDevice ---
    void** vt = *reinterpret_cast<void***>(dev);
    printf("[ABI] device=%p vtable=%p  slot15=%p slot16=%p slot27=%p slot67=%p slot76=%p\n",
           (void*)dev, (void*)vt, vt[15], vt[16], vt[27], vt[67], vt[76]);
    printf("[ABI] Wrapper: PC bootstrap и известные Xbox-переходники. Xbox-код вызывает CreateCommittedResource по слоту 76 (PC: 27) и\n"
           "[ABI] native PSO по слоту 67; слоты 15/16/18 совпадают с PC. См. compat/reports/d3d12-xs-bootstrap.\n");

    // 32768 distinct 64-byte wrappers cross two 1-MiB commit boundaries. Eight
    // simultaneous creators reproduce the game's formerly unsynchronized
    // arena frontier; each also balances QI/Release on the cached stub PSO.
    {
        HANDLE start = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        HANDLE threads[8]{}; WrapperStress runs[8]{};
        std::vector<uintptr_t> addresses(8 * 4096);
        unsigned created = 0;
        if (start) for (unsigned i = 0; i < 8; ++i) {
            runs[i].device = dev; runs[i].start = start; runs[i].pointers = addresses.data() + i * 4096;
            threads[i] = CreateThread(nullptr, 0, StressWrapperThread, &runs[i], 0, nullptr);
            if (!threads[i]) break;
            ++created;
        }
        if (start) SetEvent(start);
        const DWORD waited = created ? WaitForMultipleObjects(created, threads, TRUE, 30000) : WAIT_FAILED;
        CHECK(created == 8 && waited == WAIT_OBJECT_0, "8 parallel wrapper workers completed");
        if (created && waited != WAIT_OBJECT_0) return 2;
        LONG errors = 0;
        for (unsigned i = 0; i < created; ++i) { errors += runs[i].errors; CloseHandle(threads[i]); }
        if (start) CloseHandle(start);
        CHECK(created == 8 && !errors, "32768 cached-stub PSO create/QI/release cycles, cache reference retained (errors=%ld)", errors);
        std::sort(addresses.begin(), addresses.end());
        CHECK(addresses.front() && std::adjacent_find(addresses.begin(), addresses.end()) == addresses.end(),
              "32768 unique wrappers, concurrent commits beyond 2 MiB");
    }

    CHECK(RunBootstrapNoopProbe(dev), "opt-in no-op PSOs are real native graphics/compute objects; Draw/Dispatch submit and GPU guard stays intact");
    if (l) l->Release(); if (a) a->Release(); if (q) q->Release(); if (f) f->Release();
    if (rs) rs->Release(); dev->Release(); blob->Release();
    printf("\nSUMMARY pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
