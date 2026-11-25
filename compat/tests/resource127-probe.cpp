// Committed127: full Xbox stack ABI, owned title memory, native GPU backing.
// No original title files or network requests are needed by the probe.
#define INITGUID
#include <windows.h>
#include <d3d12.h>
#include <cstdio>
#include <cstring>
#include <map>

static int checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::printf("[FAIL] line=%d %s\n", __LINE__, #x); } } while (0)
struct Allocation { size_t size; UINT64 attributes; bool pooled; };
static std::map<void*, Allocation> allocations;
static unsigned allocated = 0, freed = 0, calls = 0, freeErrors = 0;
static bool failAllocation = false;
static size_t lastSize = 0;
static UINT64 lastAttributes = 0;
using MemoryAlloc = void* (WINAPI*)(size_t, UINT64);
using MemoryFree = void (WINAPI*)(void*, UINT64);
template<class Fn> static Fn Symbol(HMODULE module, const char* name)
{
    Fn fn = nullptr;
    FARPROC raw = module ? GetProcAddress(module, name) : nullptr;
    static_assert(sizeof fn == sizeof raw); std::memcpy(&fn, &raw, sizeof fn);
    return fn;
}
static MemoryAlloc xmemAlloc = nullptr;
static MemoryFree xmemFree = nullptr;
static bool poolEnabled = false;
static void* poolBase = nullptr;
static size_t poolUsed = 0;
static UINT64 poolAttributes = 0;
static constexpr size_t poolSize = 64 * 1024 * 1024;
static void* WINAPI Allocate(size_t size, UINT64 attributes)
{
    ++calls; lastSize = size; lastAttributes = attributes;
    if (failAllocation) return nullptr;
    void* ptr = nullptr;
    if (poolEnabled) {
        if (!poolBase) { poolAttributes = attributes; poolBase = xmemAlloc(poolSize, attributes); }
        const size_t rounded = (size + 65535) & ~size_t{65535};
        if (!poolBase || rounded > poolSize - poolUsed) return nullptr;
        ptr = (char*)poolBase + poolUsed;
        poolUsed += rounded;
    } else ptr = xmemAlloc(size, attributes);
    if (!ptr) return nullptr;
    allocations[ptr] = {size, attributes, poolEnabled}; ++allocated;
    return ptr;
}
static void WINAPI Free(void* ptr, UINT64 attributes)
{
    auto it = allocations.find(ptr);
    if (it == allocations.end()) { ++freeErrors; return; }
    if (attributes != it->second.attributes) ++freeErrors;
    if (!it->second.pooled) xmemFree(ptr, attributes);
    allocations.erase(it); ++freed;
}
#ifndef XSX_NO_TITLE_HOOKS
extern "C" {
__declspec(dllexport) void* __XMemAllocationHooks__[3] = {(void*)&Allocate, (void*)&Free, nullptr};
}
#endif
using Create = HRESULT (WINAPI*)(IUnknown*, const void*, REFIID, void**);
using Commit = HRESULT (WINAPI*)(void*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
    const D3D12_RESOURCE_DESC1*, D3D12_BARRIER_LAYOUT, const D3D12_CLEAR_VALUE*,
    ID3D12ProtectedResourceSession*, UINT, const DXGI_FORMAT*, REFIID, void**);
using XboxInfo = D3D12_RESOURCE_ALLOCATION_INFO* (WINAPI*)(D3D12_RESOURCE_ALLOCATION_INFO*,
    void*, UINT, UINT, const D3D12_RESOURCE_DESC*);
using XboxInfo1 = D3D12_RESOURCE_ALLOCATION_INFO* (WINAPI*)(D3D12_RESOURCE_ALLOCATION_INFO*,
    void*, UINT, UINT, const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_ALLOCATION_INFO1*);
static void XmemChecks()
{
    HMODULE memory = LoadLibraryA("xmem.dll"); CHECK(memory);
    if (!memory) return;
    xmemAlloc = Symbol<MemoryAlloc>(memory, "XMemAlloc");
    xmemFree = Symbol<MemoryFree>(memory, "XMemFree");
    CHECK(xmemAlloc && xmemFree);
    if (!xmemAlloc || !xmemFree) return;
    struct Case { UINT64 attributes; size_t alignment, size; };
    const Case cases[]{
        {0, 256, 0}, {0xa8004000, 256, 4097},
        {0x30808000, 65536, poolSize}, // Actual game's display-pool request.
        {0x35808000, 2 * 1024 * 1024, 65537},
        {0xf0814000, 65536, 4097}, // Memory-type bits must not enter alignment.
    };
    for (const auto& test : cases) {
        void* ptr = xmemAlloc(test.size, test.attributes); CHECK(ptr);
        CHECK(ptr && !((uintptr_t)ptr & (test.alignment - 1)));
        if (ptr) {
            const size_t size = test.size ? test.size : 1;
            auto* data = (volatile unsigned char*)ptr;
            data[0] = 0x35; data[size - 1] = 0xa7;
            CHECK(data[size - 1] == 0xa7 && (size == 1 || data[0] == 0x35));
            xmemFree(ptr, test.attributes);
        }
    }
    std::printf("[XMEM] alignment cases=5 actual_display_pool_attributes=0x30808000\n");
}
static unsigned GpuCheck(ID3D12Device* device, ID3D12Resource* texture)
{
    auto desc = texture->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 total = 0;
    device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &total);
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; buffer.Width = total + 256;
    buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_READBACK;
    ID3D12Resource* readback = nullptr;
    CHECK(SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback))) && readback);
    if (!readback) return 1;
    unsigned char* bytes = nullptr;
    CHECK(SUCCEEDED(readback->Map(0, nullptr, (void**)&bytes)) && bytes);
    if (!bytes) { readback->Release(); return 1; }
    std::memset(bytes, 0xa7, (size_t)buffer.Width); readback->Unmap(0, nullptr);
    struct { D3D12_DESCRIPTOR_HEAP_DESC desc; UINT64 guest = 0, size = 0; } hd{};
    hd.desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.desc.NumDescriptors = 1;
    ID3D12DescriptorHeap* views = nullptr;
    CHECK(SUCCEEDED(device->CreateDescriptorHeap(&hd.desc, IID_PPV_ARGS(&views))) && views);
    ID3D12CommandAllocator* allocator = nullptr;
    CHECK(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) && allocator);
    ID3D12CommandQueue* queue = nullptr;
    D3D12_COMMAND_QUEUE_DESC qd{};
    CHECK(SUCCEEDED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))) && queue);
    ID3D12GraphicsCommandList* list = nullptr;
    if (allocator) CHECK(SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        allocator, nullptr, IID_PPV_ARGS(&list))) && list);
    ID3D12Fence* fence = nullptr;
    CHECK(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))) && fence);
    HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    CHECK(done);
    unsigned errors = 1;
    if (views && allocator && queue && list && fence && done) {
        // The Xbox heap adapter returns its 64-bit handle in RAX.
        auto cpuStart = (UINT64 (WINAPI*)(void*))(*(void***)views)[9];
        D3D12_CPU_DESCRIPTOR_HANDLE handle{(SIZE_T)cpuStart(views)};
        device->CreateRenderTargetView(texture, nullptr, handle);
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = texture;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        list->ResourceBarrier(1, &barrier);
        const float color[4]{1, 0, 1, 1};
        list->ClearRenderTargetView(handle, color, 0, nullptr);
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        list->ResourceBarrier(1, &barrier);
        D3D12_TEXTURE_COPY_LOCATION source{}, target{};
        source.pResource = texture; source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        target.pResource = readback; target.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        target.PlacedFootprint = footprint;
        list->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
        CHECK(SUCCEEDED(list->Close()));
        ID3D12CommandList* commands[]{list}; queue->ExecuteCommandLists(1, commands);
        CHECK(SUCCEEDED(fence->SetEventOnCompletion(1, done)));
        CHECK(SUCCEEDED(queue->Signal(fence, 1)));
        const DWORD wait = WaitForSingleObject(done, 5000);
        CHECK(wait == WAIT_OBJECT_0);
        if (wait == WAIT_OBJECT_0) {
            bytes = nullptr;
            CHECK(SUCCEEDED(readback->Map(0, nullptr, (void**)&bytes)) && bytes);
            if (bytes) {
                errors = 0;
                for (size_t i = 0; i < buffer.Width; ++i) {
                    const size_t row = i / footprint.Footprint.RowPitch, column = i % footprint.Footprint.RowPitch;
                    unsigned char expected = 0xa7;
                    if (row < desc.Height && column < desc.Width * 4) expected = column % 4 == 1 ? 0 : 255;
                    if (bytes[i] != expected) ++errors;
                }
                readback->Unmap(0, nullptr);
            }
        }
    }
    CHECK(errors == 0);
    std::printf("[GPU] resource127 pixels=%llu buffer_bytes=%llu padding_and_tail_guard=1 errors=%u\n",
        (unsigned long long)(desc.Width * desc.Height), (unsigned long long)buffer.Width, errors);
    if (done) CloseHandle(done);
    if (fence) fence->Release();
    if (list) list->Release();
    if (queue) queue->Release();
    if (allocator) allocator->Release();
    if (views) views->Release();
    readback->Release();
    return errors;
}
int main()
{
    XmemChecks();
    if (!xmemAlloc || !xmemFree) return 1;
    // Exercise the installed return thunk as well as all seven stack arguments.
    SetEnvironmentVariableA("XSX_TRACE_RETURNS", "1");
    HMODULE module = LoadLibraryA("d3d12_xs.dll");
    FARPROC symbol = module ? GetProcAddress(module, "D3D12XboxCreateDevice") : nullptr;
    Create create = nullptr; static_assert(sizeof create == sizeof symbol);
    std::memcpy(&create, &symbol, sizeof create); CHECK(create);
    if (!create) return 1;
    ID3D12Device* device = nullptr;
    CHECK(SUCCEEDED(create(nullptr, nullptr, IID_PPV_ARGS(&device))) && device);
    if (!device) return 1;
    auto commit = (Commit)(*(void***)device)[127];
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC1 desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = desc.Height = 8; desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.Flags = (D3D12_RESOURCE_FLAGS)(0xc00000 | D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    D3D12_CLEAR_VALUE clear{}; clear.Format = desc.Format; clear.Color[0] = clear.Color[2] = clear.Color[3] = 1;
    constexpr UINT64 canary = 0x12345678aabbccddull;
    struct { UINT64 before; ID3D12Resource* value; UINT64 after; } output{canary, nullptr, canary};
    const HRESULT hr = commit(device, &heap, D3D12_HEAP_FLAG_ALLOW_DISPLAY, &desc,
        D3D12_BARRIER_LAYOUT_COMMON, &clear, nullptr, 0, nullptr,
        __uuidof(ID3D12Resource), (void**)&output.value);
    std::printf("[RESOURCE127] create hr=0x%08lX resource=%p\n", (unsigned long)hr, (void*)output.value);
    CHECK(hr == S_OK && output.value);
    CHECK(output.before == canary && output.after == canary);
    if (output.value) {
        const auto actual = output.value->GetDesc();
        CHECK(actual.Width == 8 && actual.Height == 8 && actual.Format == desc.Format);
        CHECK(actual.Flags == D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        const UINT64 address = output.value->GetGPUVirtualAddress();
        CHECK(address && !(address & 0xffff));
        void* cpu = nullptr;
        CHECK(SUCCEEDED(output.value->Map(0, nullptr, &cpu)) && (UINT64)(uintptr_t)cpu == address);
        if (cpu) {
            // Shadow stores must remain stable; GPU clear is checked separately.
            std::memset(cpu, 0x35, 32); output.value->Unmap(0, nullptr);
            void* again = nullptr;
            CHECK(SUCCEEDED(output.value->Map(0, nullptr, &again)) && again == cpu);
            CHECK(again && ((unsigned char*)again)[0] == 0x35);
            output.value->Unmap(0, nullptr);
        }
        CHECK(output.value->AddRef() >= 2);
        CHECK(output.value->Release() >= 1);
        ID3D12Resource* identity = nullptr;
        CHECK(SUCCEEDED(output.value->QueryInterface(IID_PPV_ARGS(&identity))) && identity == output.value);
        if (identity) identity->Release();
#ifndef XSX_NO_TITLE_HOOKS
        CHECK(allocated == 1 && freed == 0 && allocations.size() == 1);
        CHECK(lastSize == 0x10000 && ((lastAttributes >> 24) & 31) == 16);
        CHECK(((lastAttributes >> 16) & 255) == 128);
#endif
        GpuCheck(device, output.value);
        CHECK(output.value->Release() == 0); output.value = nullptr;
#ifndef XSX_NO_TITLE_HOOKS
        CHECK(allocated == 1 && freed == 1 && allocations.empty() && freeErrors == 0);
#else
        MEMORY_BASIC_INFORMATION info{};
        CHECK(VirtualQuery((void*)(uintptr_t)address, &info, sizeof info) && info.State == MEM_FREE);
#endif
    }
    // A >2MiB allocation must request actual resource alignment, not page size.
    desc.Width = desc.Height = 1024;
    CHECK(commit(device, &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_BARRIER_LAYOUT_COMMON,
        &clear, nullptr, 0, nullptr, __uuidof(ID3D12Resource), (void**)&output.value) == S_OK && output.value);
    if (output.value) {
#ifndef XSX_NO_TITLE_HOOKS
        D3D12_RESOURCE_DESC plain{}; std::memcpy(&plain, &desc, sizeof plain);
        D3D12_RESOURCE_ALLOCATION_INFO guest{}, via108{};
        ((XboxInfo)(*(void***)device)[25])(&guest, device, 0, 1, &plain);
        D3D12_RESOURCE_ALLOCATION_INFO1 perResource{};
        ((XboxInfo1)(*(void***)device)[108])(&via108, device, 0, 1, &plain, &perResource);
        const auto host = device->GetResourceAllocationInfo(0, 1, &plain);
        CHECK(lastSize == guest.SizeInBytes && guest.SizeInBytes == 4 * 1024 * 1024);
        CHECK(((lastAttributes >> 24) & 31) == 16 && guest.Alignment == 65536);
        CHECK(via108.SizeInBytes == guest.SizeInBytes && perResource.SizeInBytes == guest.SizeInBytes &&
            via108.Alignment == guest.Alignment && perResource.Alignment == guest.Alignment && !perResource.Offset);
        CHECK(host.SizeInBytes != ~UINT64(0));
        std::printf("[RESOURCE127] large guest_bytes=%#llx host_bytes=%#llx alignment=%#llx\n",
            (unsigned long long)guest.SizeInBytes, (unsigned long long)host.SizeInBytes, (unsigned long long)guest.Alignment);
        CHECK(((lastAttributes >> 14) & 3) == 2);
        CHECK(allocations.size() == 1);
#endif
        CHECK(output.value->Release() == 0); output.value = nullptr;
    }
    const unsigned before = calls;
    CHECK(commit(device, &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_BARRIER_LAYOUT_COMMON,
        &clear, nullptr, 0, nullptr, __uuidof(ID3D12Resource), nullptr) == S_FALSE);
    CHECK(calls == before);
    output.value = (ID3D12Resource*)(uintptr_t)canary;
    CHECK(commit(device, &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_BARRIER_LAYOUT_COMMON,
        &clear, nullptr, 0, nullptr, __uuidof(ID3D12Fence), (void**)&output.value) == E_NOINTERFACE);
    CHECK(!output.value && output.before == canary && output.after == canary);
    CHECK(commit(device, nullptr, D3D12_HEAP_FLAG_NONE, &desc, D3D12_BARRIER_LAYOUT_COMMON,
        &clear, nullptr, 0, nullptr, __uuidof(ID3D12Resource), (void**)&output.value) == E_INVALIDARG);
    CHECK(!output.value);
    const DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
    CHECK(commit(device, &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_BARRIER_LAYOUT_COMMON,
        &clear, nullptr, 1, &format, __uuidof(ID3D12Resource), (void**)&output.value) == E_NOTIMPL);
    CHECK(!output.value && calls == before);
    CHECK(commit(device, &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_BARRIER_LAYOUT_COMMON,
        &clear, (ID3D12ProtectedResourceSession*)(uintptr_t)canary, 0, nullptr,
        __uuidof(ID3D12Resource), (void**)&output.value) == E_NOTIMPL);
    CHECK(!output.value && calls == before);
#ifndef XSX_NO_TITLE_HOOKS
    failAllocation = true;
    CHECK(commit(device, &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_BARRIER_LAYOUT_COMMON,
        &clear, nullptr, 0, nullptr, __uuidof(ID3D12Resource), (void**)&output.value) == E_OUTOFMEMORY);
    CHECK(!output.value && calls == before + 1 && allocations.empty());
    failAllocation = false;
    // Reproduce the title's two4K backbuffers inside its fixed64MiB pool.
    poolEnabled = true;
    desc.Width = 3840; desc.Height = 2160;
    ID3D12Resource* backbuffers[2]{};
    for (auto& backbuffer : backbuffers) {
        CHECK(commit(device, &heap, D3D12_HEAP_FLAG_ALLOW_DISPLAY, &desc, D3D12_BARRIER_LAYOUT_COMMON,
            &clear, nullptr, 0, nullptr, __uuidof(ID3D12Resource), (void**)&backbuffer) == S_OK && backbuffer);
        const UINT64 address = backbuffer ? backbuffer->GetGPUVirtualAddress() : 0;
        CHECK(address && !(address & 65535));
        CHECK(lastSize == 0x1fe0000);
    }
    CHECK(poolUsed <= poolSize && poolSize - poolUsed <= 0x1fffff);
    std::printf("[RESOURCE127] pool_bytes=%#zx used=%#zx remaining=%#zx backbuffers=%u\n",
        poolSize, poolUsed, poolSize - poolUsed, unsigned(bool(backbuffers[0])) + unsigned(bool(backbuffers[1])));
    for (auto* backbuffer : backbuffers) if (backbuffer) CHECK(backbuffer->Release() == 0);
    if (poolBase) xmemFree(poolBase, poolAttributes);
    poolBase = nullptr; poolEnabled = false;
#endif
    CHECK(allocated == freed && freeErrors == 0 && allocations.empty());
    std::printf("[RESOURCE127] allocations=%u frees=%u free_errors=%u live=%zu\n", allocated, freed, freeErrors, allocations.size());
    device->Release();
    std::printf("[%s] resource127 checks=%d failures=%d scope=native-GPU-and-CPU-shadow\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
