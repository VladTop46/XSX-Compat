#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <cstdio>
#include <cstdint>
#include <fstream>
#include <vector>
#include <iterator>

#define CHECK(expr) do {                                  \
    HRESULT hr_ = (expr);                                 \
    printf("[%s] %s: 0x%08lX\n",                          \
           SUCCEEDED(hr_) ? "PASS" : "FAIL",              \
           #expr, (unsigned long)hr_);                    \
    fflush(stdout);                                       \
    if (FAILED(hr_)) ExitProcess(1);                      \
} while (0)

constexpr UINT GROUPS = 4096;
constexpr UINT THREADS = 256;
constexpr UINT COUNT = GROUPS * THREADS;
constexpr UINT64 BUFFER_SIZE = COUNT * sizeof(uint32_t);

static uint32_t expected(uint32_t x)
{
    x = x * 1664525u + 1013904223u;
    x ^= x >> 16;
    x *= 2246822519u;
    x ^= x >> 13;
    return x;
}

static ID3D12Resource* makeBuffer(
    ID3D12Device* device,
    D3D12_HEAP_TYPE type,
    D3D12_RESOURCE_STATES state,
    D3D12_RESOURCE_FLAGS flags)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = type;
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
    desc.Flags = flags;

    ID3D12Resource* resource = nullptr;

    CHECK(device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE,
        &desc, state, nullptr,
        IID_PPV_ARGS(&resource)));

    return resource;
}

int main()
{
    std::ifstream file("compute.dxil", std::ios::binary);

    if (!file) {
        printf("[FAIL] Cannot open compute.dxil\n");
        return 2;
    }

    std::vector<char> shader(
        (std::istreambuf_iterator<char>(file)),
        std::istreambuf_iterator<char>());

    printf("[SHADER] %zu bytes\n", shader.size());

    IDXGIFactory4* factory = nullptr;
    IDXGIAdapter1* adapter = nullptr;

    CHECK(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    CHECK(factory->EnumAdapters1(0, &adapter));

    DXGI_ADAPTER_DESC1 adapterDesc{};
    CHECK(adapter->GetDesc1(&adapterDesc));

    printf("[GPU] %ls\n", adapterDesc.Description);

    ID3D12Device* device = nullptr;

    CHECK(D3D12CreateDevice(
        adapter,
        static_cast<D3D_FEATURE_LEVEL>(0xC200),
        IID_PPV_ARGS(&device)));

    // Root signature: UAV u0.
    D3D12_ROOT_PARAMETER param{};
    param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    param.Descriptor.ShaderRegister = 0;
    param.Descriptor.RegisterSpace = 0;
    param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rootDesc{};
    rootDesc.NumParameters = 1;
    rootDesc.pParameters = &param;

    ID3DBlob* rootBlob = nullptr;
    ID3DBlob* errorBlob = nullptr;

    HRESULT hr = D3D12SerializeRootSignature(
        &rootDesc,
        D3D_ROOT_SIGNATURE_VERSION_1,
        &rootBlob,
        &errorBlob);

    if (FAILED(hr)) {
        if (errorBlob)
            printf("[ROOT ERROR] %.*s\n",
                (int)errorBlob->GetBufferSize(),
                (const char*)errorBlob->GetBufferPointer());

        return 3;
    }

    ID3D12RootSignature* root = nullptr;

    CHECK(device->CreateRootSignature(
        0,
        rootBlob->GetBufferPointer(),
        rootBlob->GetBufferSize(),
        IID_PPV_ARGS(&root)));

    rootBlob->Release();
    if (errorBlob) errorBlob->Release();

    // Compute pipeline.
    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = root;
    psoDesc.CS.pShaderBytecode = shader.data();
    psoDesc.CS.BytecodeLength = shader.size();

    ID3D12PipelineState* pso = nullptr;

    CHECK(device->CreateComputePipelineState(
        &psoDesc, IID_PPV_ARGS(&pso)));

    // GPU resources.
    auto output = makeBuffer(
        device,
        D3D12_HEAP_TYPE_DEFAULT,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

    auto readback = makeBuffer(
        device,
        D3D12_HEAP_TYPE_READBACK,
        D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_FLAG_NONE);

    printf("[UAV ADDRESS] 0x%llX\n",
        (unsigned long long)output->GetGPUVirtualAddress());

    // Dedicated compute command queue.
    D3D12_COMMAND_QUEUE_DESC qdesc{};
    qdesc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;

    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;

    CHECK(device->CreateCommandQueue(
        &qdesc, IID_PPV_ARGS(&queue)));

    CHECK(device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_COMPUTE,
        IID_PPV_ARGS(&allocator)));

    CHECK(device->CreateCommandList(
        0,
        D3D12_COMMAND_LIST_TYPE_COMPUTE,
        allocator, pso,
        IID_PPV_ARGS(&list)));

    list->SetComputeRootSignature(root);

    list->SetComputeRootUnorderedAccessView(
        0, output->GetGPUVirtualAddress());

    printf("[DISPATCH] %u groups x %u threads\n",
        GROUPS, THREADS);

    list->Dispatch(GROUPS, 1, 1);

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = output;
    barrier.Transition.Subresource =
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore =
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    barrier.Transition.StateAfter =
        D3D12_RESOURCE_STATE_COPY_SOURCE;

    list->ResourceBarrier(1, &barrier);

    list->CopyBufferRegion(
        readback, 0, output, 0, BUFFER_SIZE);

    CHECK(list->Close());

    ID3D12Fence* fence = nullptr;

    CHECK(device->CreateFence(
        0, D3D12_FENCE_FLAG_NONE,
        IID_PPV_ARGS(&fence)));

    HANDLE event = CreateEventW(
        nullptr, FALSE, FALSE, nullptr);

    if (!event) return 4;

    ID3D12CommandList* lists[] = {list};

    LARGE_INTEGER freq{}, start{}, end{};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);

    printf("[EXECUTE] Submitting compute workload\n");
    fflush(stdout);

    queue->ExecuteCommandLists(1, lists);

    CHECK(queue->Signal(fence, 1));
    CHECK(fence->SetEventOnCompletion(1, event));

    if (WaitForSingleObject(event, 15000)
        != WAIT_OBJECT_0) {
        printf("[FAIL] GPU timeout\n");
        return 5;
    }

    QueryPerformanceCounter(&end);

    CHECK(device->GetDeviceRemovedReason());

    printf("[SUBMISSION TIME] %.3f ms\n",
        1000.0 * (end.QuadPart - start.QuadPart) /
        freq.QuadPart);

    // Verify every shader invocation.
    void* mapped = nullptr;
    D3D12_RANGE range{0, BUFFER_SIZE};

    CHECK(readback->Map(0, &range, &mapped));

    const auto* values = static_cast<uint32_t*>(mapped);
    UINT errors = 0;

    for (UINT i = 0; i < COUNT; ++i) {
        uint32_t ref = expected(i);

        if (values[i] != ref) {
            if (errors < 10)
                printf("[MISMATCH] %u GPU=%08X CPU=%08X\n",
                    i, values[i], ref);

            ++errors;
        }
    }

    for (UINT i = 0; i < 8; ++i)
        printf("[SAMPLE %u] 0x%08X\n", i, values[i]);

    D3D12_RANGE noWrite{0, 0};
    readback->Unmap(0, &noWrite);

    printf("[VERIFIED VALUES] %u\n", COUNT);
    printf("[MISMATCHES] %u\n", errors);

    CloseHandle(event);

    fence->Release();
    list->Release();
    allocator->Release();
    queue->Release();
    readback->Release();
    output->Release();
    pso->Release();
    root->Release();
    device->Release();
    adapter->Release();
    factory->Release();

    printf("[RESULT] %s\n", errors ? "FAIL" : "PASS");

    return errors ? 6 : 0;
}
