// GPU probe for the native constant-store CS lowering. No fallback shader.
#define INITGUID
#include <windows.h>
#include <d3d12.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include "native-cs-data.h"

#define HR(expr) do { HRESULT h = (expr); if (FAILED(h)) { \
    std::fprintf(stderr, "[FAIL] %s: 0x%08lx\n", #expr, (unsigned long)h); return 2; } } while (0)

int main()
{
    std::puts("[SOURCE] translated game CS_CullMeshletsInitArgs, one raw UAV store");
    std::puts("[HASH] " NATIVE_PROGRAM_HASH);
    ID3D12Device* dev = nullptr;
    HR(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev)));
    D3D12_ROOT_PARAMETER parameter{};
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    parameter.Descriptor.ShaderRegister = UAV_REGISTER;
    parameter.Descriptor.RegisterSpace = UAV_SPACE;
    D3D12_ROOT_SIGNATURE_DESC rsd{1, &parameter, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ID3DBlob *serialized = nullptr, *errors = nullptr;
    HR(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1_0, &serialized, &errors));
    ID3D12RootSignature* root = nullptr;
    HR(dev->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&root)));
    serialized->Release(); if (errors) errors->Release();
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = root; pd.CS = {kNativeCS, sizeof(kNativeCS)};
    ID3D12PipelineState* pso = nullptr;
    HR(dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso)));
    std::puts("[PSO] CreateComputePipelineState S_OK");
    ID3D12CommandQueue* queue = nullptr;
    D3D12_COMMAND_QUEUE_DESC qd{};
    HR(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)));
    ID3D12CommandAllocator* allocator = nullptr;
    HR(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
    ID3D12GraphicsCommandList* list = nullptr;
    HR(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, pso, IID_PPV_ARGS(&list)));

    constexpr size_t size = 64;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; desc.Width = size; desc.Height = 1;
    desc.DepthOrArraySize = 1; desc.MipLevels = 1; desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    ID3D12Resource *upload = nullptr, *buffer = nullptr, *readback = nullptr;
    HR(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)));
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    HR(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)));
    hp.Type = D3D12_HEAP_TYPE_DEFAULT; desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    HR(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&buffer)));
    void* mapped = nullptr;
    D3D12_RANGE empty{0, 0}; HR(upload->Map(0, &empty, &mapped));
    std::memset(mapped, 0xa5, size); upload->Unmap(0, nullptr);
    list->CopyBufferRegion(buffer, 0, upload, 0, size);
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {buffer, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
    list->ResourceBarrier(1, &barrier);
    list->SetComputeRootSignature(root);
    list->SetComputeRootUnorderedAccessView(0, buffer->GetGPUVirtualAddress());
    list->Dispatch(1, 1, 1);
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    list->ResourceBarrier(1, &barrier);
    list->CopyBufferRegion(readback, 0, buffer, 0, size);
    HR(list->Close());
    ID3D12CommandList* lists[] = {list}; queue->ExecuteCommandLists(1, lists);
    ID3D12Fence* fence = nullptr;
    HR(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event) return 3;
    HR(queue->Signal(fence, 1)); HR(fence->SetEventOnCompletion(1, event));
    if (WaitForSingleObject(event, 10000) != WAIT_OBJECT_0) return 4;
    D3D12_RANGE range{0, size}; HR(readback->Map(0, &range, &mapped));
    const uint32_t* words = static_cast<const uint32_t*>(mapped);
    unsigned failures = 0;
    for (unsigned i = 0; i < size / 4; ++i) {
        uint32_t expected = i < STORE_DWORDS ? 0 : 0xa5a5a5a5;
        if (words[i] != expected) {
            std::printf("[MISMATCH] dword=%u expected=%08x actual=%08x\n", i, expected, words[i]); ++failures;
        }
    }
    readback->Unmap(0, &empty);
    std::printf("[%s] compute readback dwords=%zu written=%u guard=%zu failures=%u\n",
        failures ? "FAIL" : "PASS", size / 4, STORE_DWORDS, size / 4 - STORE_DWORDS, failures);
    CloseHandle(event); fence->Release(); readback->Release(); buffer->Release(); upload->Release();
    list->Release(); allocator->Release(); queue->Release(); pso->Release(); root->Release(); dev->Release();
    return failures ? 5 : 0;
}
