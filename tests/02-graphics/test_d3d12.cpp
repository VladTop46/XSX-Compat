#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <cstdio>
#include <cstdint>
#include <cstdlib>

#define CHECK(expr) do {                                      \
    HRESULT hr_ = (expr);                                     \
    printf("[%s] %s: 0x%08lX\n",                              \
           SUCCEEDED(hr_) ? "PASS" : "FAIL",                  \
           #expr, (unsigned long)hr_);                        \
    fflush(stdout);                                           \
    if (FAILED(hr_)) ExitProcess(1);                          \
} while (0)

constexpr UINT64 BUFFER_SIZE = 4 * 1024 * 1024;

static uint8_t pattern(UINT64 i)
{
    return static_cast<uint8_t>(
        (i * 131) ^ (i >> 8) ^ (i >> 16));
}

static ID3D12Resource* makeBuffer(
    ID3D12Device* device,
    D3D12_HEAP_TYPE heapType,
    D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = heapType;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = BUFFER_SIZE;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ID3D12Resource* resource = nullptr;

    CHECK(device->CreateCommittedResource(
        &heap,
        D3D12_HEAP_FLAG_NONE,
        &desc,
        state,
        nullptr,
        IID_PPV_ARGS(&resource)));

    return resource;
}

int main()
{
    IDXGIFactory4* factory = nullptr;

    CHECK(CreateDXGIFactory1(
        IID_PPV_ARGS(&factory)));

    IDXGIAdapter1* adapter = nullptr;

    for (UINT i = 0; ; ++i) {
        IDXGIAdapter1* candidate = nullptr;

        if (factory->EnumAdapters1(i, &candidate)
            == DXGI_ERROR_NOT_FOUND)
            break;

        if (!candidate) continue;

        DXGI_ADAPTER_DESC1 desc{};
        candidate->GetDesc1(&desc);

        printf("[ADAPTER %u] %ls\n",
               i, desc.Description);

        printf("[VRAM] %llu MiB\n",
               (unsigned long long)
               (desc.DedicatedVideoMemory / 1048576));

        printf("[PCI ID] %04X:%04X\n",
               desc.VendorId, desc.DeviceId);

        if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
            && !adapter) {
            adapter = candidate;
        } else {
            candidate->Release();
        }
    }

    if (!adapter) {
        printf("[FAIL] No hardware adapter\n");
        return 2;
    }

    ID3D12Device* device = nullptr;

    CHECK(D3D12CreateDevice(
        adapter,
        D3D_FEATURE_LEVEL_11_0,
        IID_PPV_ARGS(&device)));

    printf("[GPU NODES] %u\n",
           device->GetNodeCount());

    D3D_FEATURE_LEVEL levels[] = {
        static_cast<D3D_FEATURE_LEVEL>(0xC200),
        D3D_FEATURE_LEVEL_12_1,
        D3D_FEATURE_LEVEL_12_0,
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0
    };

    D3D12_FEATURE_DATA_FEATURE_LEVELS features{};
    features.NumFeatureLevels = sizeof(levels) / sizeof(levels[0]);
    features.pFeatureLevelsRequested = levels;

    CHECK(device->CheckFeatureSupport(
        D3D12_FEATURE_FEATURE_LEVELS,
        &features, sizeof(features)));

    printf("[FEATURE LEVEL] 0x%X\n",
           features.MaxSupportedFeatureLevel);

    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

    ID3D12CommandQueue* queue = nullptr;

    CHECK(device->CreateCommandQueue(
        &queueDesc, IID_PPV_ARGS(&queue)));

    ID3D12CommandAllocator* allocator = nullptr;

    CHECK(device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS(&allocator)));

    ID3D12GraphicsCommandList* list = nullptr;

    CHECK(device->CreateCommandList(
        0,
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        allocator,
        nullptr,
        IID_PPV_ARGS(&list)));

    printf("[ALLOCATE] UPLOAD\n");

    auto upload = makeBuffer(
        device, D3D12_HEAP_TYPE_UPLOAD,
        D3D12_RESOURCE_STATE_GENERIC_READ);

    printf("[ALLOCATE] DEFAULT\n");

    auto gpu = makeBuffer(
        device, D3D12_HEAP_TYPE_DEFAULT,
        D3D12_RESOURCE_STATE_COPY_DEST);

    printf("[ALLOCATE] READBACK\n");

    auto readback = makeBuffer(
        device, D3D12_HEAP_TYPE_READBACK,
        D3D12_RESOURCE_STATE_COPY_DEST);

    printf("[GPU VA] 0x%llX\n",
           (unsigned long long)
           gpu->GetGPUVirtualAddress());

    void* mapped = nullptr;
    D3D12_RANGE noRead{0, 0};

    CHECK(upload->Map(0, &noRead, &mapped));

    auto bytes = static_cast<uint8_t*>(mapped);

    for (UINT64 i = 0; i < BUFFER_SIZE; ++i)
        bytes[i] = pattern(i);

    upload->Unmap(0, nullptr);

    list->CopyBufferRegion(gpu, 0, upload, 0, BUFFER_SIZE);

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = gpu;
    barrier.Transition.Subresource =
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore =
        D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter =
        D3D12_RESOURCE_STATE_COPY_SOURCE;

    list->ResourceBarrier(1, &barrier);

    list->CopyBufferRegion(readback, 0, gpu, 0, BUFFER_SIZE);

    CHECK(list->Close());

    ID3D12Fence* fence = nullptr;

    CHECK(device->CreateFence(
        0, D3D12_FENCE_FLAG_NONE,
        IID_PPV_ARGS(&fence)));

    HANDLE event = CreateEventW(
        nullptr, FALSE, FALSE, nullptr);

    if (!event) return 3;

    ID3D12CommandList* lists[] = {list};

    printf("[EXECUTE] command list\n");
    fflush(stdout);

    queue->ExecuteCommandLists(1, lists);

    CHECK(queue->Signal(fence, 1));
    CHECK(fence->SetEventOnCompletion(1, event));

    if (WaitForSingleObject(event, 10000)
        != WAIT_OBJECT_0) {
        printf("[FAIL] GPU fence timeout\n");
        return 4;
    }

    CHECK(device->GetDeviceRemovedReason());

    printf("[FENCE] completed=%llu\n",
           (unsigned long long)
           fence->GetCompletedValue());

    D3D12_RANGE readRange{0, BUFFER_SIZE};

    CHECK(readback->Map(0, &readRange, &mapped));

    bytes = static_cast<uint8_t*>(mapped);
    UINT64 errors = 0;

    for (UINT64 i = 0; i < BUFFER_SIZE; ++i) {
        if (bytes[i] != pattern(i))
            ++errors;
    }

    readback->Unmap(0, &noRead);

    printf("[VERIFIED BYTES] %llu\n",
           (unsigned long long)BUFFER_SIZE);

    printf("[MISMATCHES] %llu\n",
           (unsigned long long)errors);

    CloseHandle(event);

    fence->Release();
    readback->Release();
    gpu->Release();
    upload->Release();
    list->Release();
    allocator->Release();
    queue->Release();
    device->Release();
    adapter->Release();
    factory->Release();

    printf("[RESULT] %s\n",
           errors == 0 ? "PASS" : "FAIL");

    return errors == 0 ? 0 : 5;
}
