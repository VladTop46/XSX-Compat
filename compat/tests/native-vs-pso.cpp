// GPU probe for the translated native VS. Pixel shader is synthetic UV output.
// Uses native PC D3D12 objects; does not load the game or replace any vtable.
#define INITGUID
#include <windows.h>
#include <d3d12.h>
#include <cstdio>
#include <cmath>
#include <cstdint>
#include "native-vs-data.h"

#define HR(expr) do { HRESULT hr_ = (expr); if (FAILED(hr_)) { \
    std::fprintf(stderr, "[FAIL] %s: 0x%08lx\n", #expr, (unsigned long)hr_); return 2; } } while (0)

int main()
{
    std::puts("[SOURCE] translated native fullscreen VS; synthetic UV pixel shader");
    std::puts("[HASH] " NATIVE_PROGRAM_HASH);
    ID3D12Device* dev = nullptr;
    HR(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev)));
    ID3D12CommandQueue* queue = nullptr;
    D3D12_COMMAND_QUEUE_DESC qd{};
    HR(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)));
    ID3D12CommandAllocator* allocator = nullptr;
    HR(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
    ID3D12GraphicsCommandList* list = nullptr;
    HR(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, IID_PPV_ARGS(&list)));

    D3D12_ROOT_SIGNATURE_DESC rd{};
    rd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ID3DBlob* serialized = nullptr;
    ID3DBlob* errors = nullptr;
    HR(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1_0, &serialized, &errors));
    ID3D12RootSignature* root = nullptr;
    HR(dev->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&root)));
    serialized->Release();
    if (errors) errors->Release();

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = root;
    pd.VS = {kNativeVS, sizeof(kNativeVS)};
    pd.PS = {kProbePS, sizeof(kProbePS)};
    pd.BlendState.RenderTarget[0] = {FALSE, FALSE, D3D12_BLEND_ONE, D3D12_BLEND_ZERO,
        D3D12_BLEND_OP_ADD, D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD,
        D3D12_LOGIC_OP_NOOP, D3D12_COLOR_WRITE_ENABLE_ALL};
    pd.SampleMask = UINT_MAX;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = DXGI_FORMAT_R32G32B32A32_FLOAT;
    pd.SampleDesc.Count = 1;
    ID3D12PipelineState* pso = nullptr;
    HR(dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso)));
    std::puts("[PSO] CreateGraphicsPipelineState S_OK");

    constexpr UINT width = 8, height = 8;
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC textureDesc{};
    textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    textureDesc.Width = width; textureDesc.Height = height;
    textureDesc.DepthOrArraySize = 1; textureDesc.MipLevels = 1;
    textureDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ID3D12Resource* texture = nullptr;
    HR(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &textureDesc,
        D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&texture)));

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = 1;
    ID3D12DescriptorHeap* heap = nullptr;
    HR(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)));
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = heap->GetCPUDescriptorHandleForHeapStart();
    dev->CreateRenderTargetView(texture, nullptr, rtv);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rows = 0;
    UINT64 rowBytes = 0, total = 0;
    dev->GetCopyableFootprints(&textureDesc, 0, 1, 0, &footprint, &rows, &rowBytes, &total);
    D3D12_RESOURCE_DESC bufferDesc{};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width = total; bufferDesc.Height = 1;
    bufferDesc.DepthOrArraySize = 1; bufferDesc.MipLevels = 1;
    bufferDesc.SampleDesc.Count = 1;
    bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    ID3D12Resource* readback = nullptr;
    HR(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bufferDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)));

    const float clear[] = {-8, -8, -8, -8};
    D3D12_VIEWPORT viewport{0, 0, float(width), float(height), 0, 1};
    D3D12_RECT scissor{0, 0, LONG(width), LONG(height)};
    list->ClearRenderTargetView(rtv, clear, 0, nullptr);
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    list->RSSetViewports(1, &viewport); list->RSSetScissorRects(1, &scissor);
    list->SetGraphicsRootSignature(root); list->SetPipelineState(pso);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->DrawInstanced(3, 1, 0, 0);
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {texture, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                         D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE};
    list->ResourceBarrier(1, &barrier);
    D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
    dst.pResource = readback; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = footprint;
    src.pResource = texture; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    HR(list->Close());
    ID3D12CommandList* lists[] = {list}; queue->ExecuteCommandLists(1, lists);
    ID3D12Fence* fence = nullptr;
    HR(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event) return 3;
    HR(queue->Signal(fence, 1)); HR(fence->SetEventOnCompletion(1, event));
    if (WaitForSingleObject(event, 10000) != WAIT_OBJECT_0) {
        std::puts("[FAIL] GPU completion timeout"); return 4;
    }
    void* mapped = nullptr;
    D3D12_RANGE range{0, SIZE_T(total)};
    HR(readback->Map(0, &range, &mapped));
    double maximum = 0;
    UINT failures = 0;
    for (UINT y = 0; y < height; ++y) {
        const float* row = reinterpret_cast<const float*>(static_cast<const uint8_t*>(mapped) + footprint.Offset + y * footprint.Footprint.RowPitch);
        for (UINT x = 0; x < width; ++x) {
            const float expected[] = {(x + .5f) / width, (y + .5f) / height, 0, 1};
            for (UINT c = 0; c < 4; ++c) {
                double error = std::fabs(double(row[4 * x + c]) - expected[c]);
                if (!std::isfinite(row[4 * x + c]) || error > 1e-5) ++failures;
                if (error > maximum) maximum = error;
            }
        }
    }
    D3D12_RANGE empty{0, 0}; readback->Unmap(0, &empty);
    std::printf("[%s] readback pixels=%u components=%u failures=%u max_error=%.9g\n",
        failures ? "FAIL" : "PASS", width * height, width * height * 4, failures, maximum);
    CloseHandle(event);
    fence->Release(); readback->Release(); heap->Release(); texture->Release();
    pso->Release(); root->Release(); list->Release(); allocator->Release(); queue->Release(); dev->Release();
    return failures ? 5 : 0;
}
