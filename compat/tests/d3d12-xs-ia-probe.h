// Actual GPU regression for the two different command-list 47/48 contracts.
// Synthetic shaders only: this does not translate or execute a game shader.
#include <d3dcompiler.h>
#include <cmath>
template<class T> struct IaProbeCom {
    T* p = nullptr;
    ~IaProbeCom() { if (p) p->Release(); }
    T* operator->() const { return p; }
};
static bool RunInputAssemblerProbe(ID3D12Device* dev, SerFn serialize, bool xbox)
{
#define IA_HR(expr) do { HRESULT result = (expr); if (FAILED(result)) { \
    printf("[IA] %s %s hr=0x%08lX\n", xbox ? "Xbox" : "PC", #expr, (unsigned long)result); return false; } } while (0)
    struct Compiler {
        HMODULE module = LoadLibraryW(L"d3dcompiler_47.dll");
        ~Compiler() { if (module) FreeLibrary(module); }
    } compiler;
    auto compile = compiler.module ? (decltype(&D3DCompile))GetProcAddress(compiler.module, "D3DCompile") : nullptr;
    if (!compile) return false;
    static const char source[] =
        "struct V { float4 p:SV_Position; float4 c:COLOR; };"
        "V vertex(float2 p:POSITION, float4 c:COLOR) { V o; o.p=float4(p,0.25,1); o.c=c; return o; }"
        "ByteAddressBuffer tint:register(t0); cbuffer Scale:register(b0) { float4 scale; };"
        "float4 pixel(V i):SV_Target { return i.c*asfloat(tint.Load4(0))*scale; }";
    IaProbeCom<ID3DBlob> vs, ps, signature, error;
    IA_HR(compile(source, sizeof(source)-1, "ia-probe", nullptr, nullptr, "vertex", "vs_5_0", 0, 0, &vs.p, &error.p));
    if (error.p) { error.p->Release(); error.p = nullptr; }
    IA_HR(compile(source, sizeof(source)-1, "ia-probe", nullptr, nullptr, "pixel", "ps_5_0", 0, 0, &ps.p, &error.p));
    if (error.p) { error.p->Release(); error.p = nullptr; }
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC rd{}; rd.Version = D3D_ROOT_SIGNATURE_VERSION_1_0;
    D3D12_DESCRIPTOR_RANGE srv_range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable = {1, &srv_range};
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants = {0, 0, 4};
    rd.Desc_1_0.NumParameters = 2; rd.Desc_1_0.pParameters = params;
    rd.Desc_1_0.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    IA_HR(serialize(&rd, &signature.p, &error.p));
    IaProbeCom<ID3D12RootSignature> root;
    IA_HR(dev->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_ID3D12RootSignature, (void**)&root.p));
    const D3D12_INPUT_ELEMENT_DESC elements[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 2, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 2, 8, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}
    };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = root.p;
    pd.VS = {vs->GetBufferPointer(), vs->GetBufferSize()}; pd.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    pd.InputLayout = {elements, 2}; pd.SampleMask = ~UINT(0);
    pd.BlendState.RenderTarget[0] = {FALSE, FALSE, D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD,
        D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD, D3D12_LOGIC_OP_NOOP, D3D12_COLOR_WRITE_ENABLE_ALL};
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.DepthStencilState.DepthEnable = TRUE; pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1; pd.RTVFormats[0] = DXGI_FORMAT_R32G32B32A32_FLOAT;
    pd.DSVFormat = DXGI_FORMAT_D32_FLOAT; pd.SampleDesc.Count = 1;
    IaProbeCom<ID3D12PipelineState> pso;
    IA_HR(dev->CreateGraphicsPipelineState(&pd, IID_ID3D12PipelineState, (void**)&pso.p));
    IaProbeCom<ID3D12Resource> texture, depth, upload, readback;
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td{}; td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; td.Width = td.Height = 8;
    td.DepthOrArraySize = td.MipLevels = 1; td.SampleDesc.Count = 1;
    td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    IA_HR(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET,
        nullptr, IID_ID3D12Resource, (void**)&texture.p));
    td.Format = DXGI_FORMAT_D32_FLOAT; td.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    IA_HR(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_DEPTH_WRITE,
        nullptr, IID_ID3D12Resource, (void**)&depth.p));
    constexpr UINT pitch = 256, image_size = pitch * 8, size = image_size * 2 + 256;
    D3D12_RESOURCE_DESC bd{}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = size;
    bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    IA_HR(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, IID_ID3D12Resource, (void**)&upload.p));
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    IA_HR(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr, IID_ID3D12Resource, (void**)&readback.p));
    struct Vertex { float x, y, color[4]; };
    const Vertex vertices[] = {{9,9,{1,0,0,1}}, {-1,-1,{.125f,.25f,.5f,1}},
        {3,-1,{.125f,.25f,.5f,1}}, {-1,3,{.125f,.25f,.5f,1}}};
    const uint16_t indices[] = {0xffff, 0, 1, 2};
    void* mapped = nullptr; D3D12_RANGE none{0,0};
    IA_HR(upload->Map(0, &none, &mapped));
    memset(mapped, 0xcd, size); memcpy(mapped, vertices, sizeof vertices); memcpy((char*)mapped + 256, indices, sizeof indices);
    const FLOAT tint[] = {2,.5f,1,1}, scale[] = {.5f,2,1,1};
    memcpy((char*)mapped + 512, tint, sizeof tint);
    upload->Unmap(0, nullptr);
    IaProbeCom<ID3D12DescriptorHeap> rtv_heap, dsv_heap;
    struct { D3D12_DESCRIPTOR_HEAP_DESC desc; UINT64 base, size; } hd{};
    hd.desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.desc.NumDescriptors = 1;
    IA_HR(dev->CreateDescriptorHeap(&hd.desc, IID_ID3D12DescriptorHeap, (void**)&rtv_heap.p));
    hd.desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    IA_HR(dev->CreateDescriptorHeap(&hd.desc, IID_ID3D12DescriptorHeap, (void**)&dsv_heap.p));
    auto heap_start = [](ID3D12DescriptorHeap* heap) {
        return D3D12_CPU_DESCRIPTOR_HANDLE{((UINT64 (WINAPI*)(void*))(*(void***)heap)[9])(heap)};
    };
    const auto rtv = heap_start(rtv_heap.p), dsv = heap_start(dsv_heap.p);
    dev->CreateRenderTargetView(texture.p, nullptr, rtv); dev->CreateDepthStencilView(depth.p, nullptr, dsv);
    IaProbeCom<ID3D12DescriptorHeap> shader_heap;
    hd.desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    IA_HR(dev->CreateDescriptorHeap(&hd.desc, IID_ID3D12DescriptorHeap, (void**)&shader_heap.p));
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R32_TYPELESS; srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Buffer.FirstElement = 512 / 4; srv.Buffer.NumElements = 4; srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    dev->CreateShaderResourceView(upload.p, &srv, heap_start(shader_heap.p));
    const D3D12_GPU_DESCRIPTOR_HANDLE gpu_start{((UINT64 (WINAPI*)(void*))(*(void***)shader_heap.p)[10])(shader_heap.p)};
    IaProbeCom<ID3D12CommandAllocator> allocator;
    IaProbeCom<ID3D12GraphicsCommandList> clear, draw;
    IaProbeCom<ID3D12CommandQueue> queue;
    IaProbeCom<ID3D12Fence> fence;
    IA_HR(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_ID3D12CommandAllocator, (void**)&allocator.p));
    IA_HR(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.p, nullptr, IID_ID3D12GraphicsCommandList, (void**)&clear.p));
    D3D12_COMMAND_QUEUE_DESC qd{};
    IA_HR(dev->CreateCommandQueue(&qd, IID_ID3D12CommandQueue, (void**)&queue.p));
    IA_HR(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_ID3D12Fence, (void**)&fence.p));
    const FLOAT clear_color[] = {1,0,0,1};
    clear->CopyBufferRegion(readback.p, 0, upload.p, 0, size);
    clear->ClearRenderTargetView(rtv, clear_color, 0, nullptr);
    clear->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, .75f, 0, 0, nullptr);
    auto copy_image = [&](ID3D12GraphicsCommandList* list, UINT64 offset) {
        D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = {texture.p, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE};
        list->ResourceBarrier(1, &barrier);
        D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
        src.pResource = texture.p; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.pResource = readback.p; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = {offset, {DXGI_FORMAT_R32G32B32A32_FLOAT, 8, 8, 1, pitch}};
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter); list->ResourceBarrier(1, &barrier);
    };
    copy_image(clear.p, 0); IA_HR(clear->Close());
    if (xbox) {
        UINT xd[6]{};
        IA_HR(((HRESULT (WINAPI*)(void*, const UINT*, ID3D12CommandAllocator*, ID3D12PipelineState*, REFIID, void**))
            (*(void***)dev)[73])(dev, xd, allocator.p, pso.p, IID_ID3D12GraphicsCommandList, (void**)&draw.p));
    } else IA_HR(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.p, pso.p, IID_ID3D12GraphicsCommandList, (void**)&draw.p));
    D3D12_VIEWPORT vp{0,0,8,8,0,1}; D3D12_RECT scissor{0,0,8,8};
    draw->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
    draw->RSSetViewports(1, &vp); draw->RSSetScissorRects(1, &scissor);
    ID3D12DescriptorHeap* heaps[]{shader_heap.p}; draw->SetDescriptorHeaps(1, heaps);
    D3D12_VERTEX_BUFFER_VIEW vb{upload->GetGPUVirtualAddress(), sizeof vertices, sizeof(Vertex)};
    D3D12_INDEX_BUFFER_VIEW ib{upload->GetGPUVirtualAddress() + 256, sizeof indices, DXGI_FORMAT_R16_UINT};
    if (xbox) {
        void** vt = *(void***)draw.p;
        ((void (WINAPI*)(void*, ID3D12RootSignature*, UINT, UINT))vt[34])(draw.p, root.p, 0, 0);
        ((void (WINAPI*)(void*, UINT, D3D12_GPU_DESCRIPTOR_HANDLE))vt[36])(draw.p, 0, gpu_start);
        ((void (WINAPI*)(void*, UINT, UINT, const void*, UINT))vt[40])(draw.p, 1, 4, scale, 0);
        ((void (WINAPI*)(void*, const D3D12_INDEX_BUFFER_VIEW*))vt[47])(draw.p, &ib);
        ((void (WINAPI*)(void*, UINT, UINT, const D3D12_VERTEX_BUFFER_VIEW*))vt[48])(draw.p, 2, 1, &vb);
    } else {
        draw->SetGraphicsRootSignature(root.p); draw->IASetIndexBuffer(&ib); draw->IASetVertexBuffers(2, 1, &vb);
        draw->SetGraphicsRootDescriptorTable(0, gpu_start); draw->SetGraphicsRoot32BitConstants(1, 4, scale, 0);
    }
    draw->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    draw->DrawIndexedInstanced(3, 1, 1, 1, 0);
    copy_image(draw.p, image_size); IA_HR(draw->Close());
    ID3D12CommandList* lists[]{clear.p, draw.p}; queue->ExecuteCommandLists(2, lists);
    IA_HR(queue->Signal(fence.p, 1));
    const auto deadline = GetTickCount64() + 10000;
    while (fence->GetCompletedValue() < 1 && GetTickCount64() < deadline) Sleep(1);
    if (fence->GetCompletedValue() < 1) return false;
    D3D12_RANGE range{0, size}; IA_HR(readback->Map(0, &range, &mapped));
    unsigned failures = 0;
    const float expected[2][4] = {{1,0,0,1}, {.125f,.25f,.5f,1}};
    for (unsigned image = 0; image < 2; ++image) for (unsigned y = 0; y < 8; ++y) {
        const float* row = (const float*)((const char*)mapped + image * image_size + y * pitch);
        for (unsigned x = 0; x < 8; ++x) for (unsigned c = 0; c < 4; ++c)
            if (!std::isfinite(row[x*4+c]) || std::fabs(row[x*4+c] - expected[image][c]) > 1e-5f) ++failures;
    }
    for (unsigned i = 2 * image_size; i < size; ++i) if (((const unsigned char*)mapped)[i] != 0xcd) ++failures;
    readback->Unmap(0, &none);
    printf("[IA] %s readback clear=64 draw=64 guard=256 descriptor_tint=1 root_constants=1 failures=%u\n", xbox ? "Xbox" : "PC", failures);
    return failures == 0;
#undef IA_HR
}

static bool RunBootstrapNoopProbe(ID3D12Device* dev)
{
    struct Mode {
        std::vector<char> old;
        Mode() {
            const DWORD size = GetEnvironmentVariableA("XSX_DEV_NOOP_PSO", nullptr, 0);
            if (size) { old.resize(size); GetEnvironmentVariableA("XSX_DEV_NOOP_PSO", old.data(), size); }
            SetEnvironmentVariableA("XSX_DEV_NOOP_PSO", "1");
        }
        ~Mode() { SetEnvironmentVariableA("XSX_DEV_NOOP_PSO", old.empty() ? nullptr : old.data()); }
    } mode;
#define NOOP_HR(expr) do { HRESULT result = (expr); if (FAILED(result)) { \
    printf("[BOOTSTRAP] %s hr=0x%08lX\n", #expr, (unsigned long)result); return false; } } while (0)
    using Native = HRESULT (WINAPI*)(void*, const void*, REFIID, void**);
    void** vt = *(void***)dev;
    UINT64 opaque[32]{};
    IaProbeCom<ID3D12PipelineState> graphics, compute, updated;
    NOOP_HR(((Native)vt[57])(dev, opaque, IID_ID3D12PipelineState, (void**)&graphics.p));
    NOOP_HR(((Native)vt[67])(dev, opaque, IID_ID3D12PipelineState, (void**)&compute.p));
    NOOP_HR(((HRESULT (WINAPI*)(void*, ID3D12PipelineState*, UINT, const void*, REFIID, void**))vt[55])(
        dev, graphics.p, 0, nullptr, IID_ID3D12PipelineState, (void**)&updated.p));
    if (!graphics.p || !compute.p || updated.p != graphics.p) return false;
    IaProbeCom<ID3D12CommandAllocator> allocator;
    IaProbeCom<ID3D12GraphicsCommandList> list;
    IaProbeCom<ID3D12CommandQueue> queue;
    IaProbeCom<ID3D12Fence> fence;
    NOOP_HR(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_ID3D12CommandAllocator, (void**)&allocator.p));
    UINT xd[6]{};
    NOOP_HR(((HRESULT (WINAPI*)(void*, const UINT*, ID3D12CommandAllocator*, ID3D12PipelineState*, REFIID, void**))vt[73])(
        dev, xd, allocator.p, graphics.p, IID_ID3D12GraphicsCommandList, (void**)&list.p));
    D3D12_COMMAND_QUEUE_DESC qd{};
    NOOP_HR(dev->CreateCommandQueue(&qd, IID_ID3D12CommandQueue, (void**)&queue.p));
    NOOP_HR(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_ID3D12Fence, (void**)&fence.p));
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bd{}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = 64;
    bd.Height = bd.DepthOrArraySize = bd.MipLevels = bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    IaProbeCom<ID3D12Resource> upload, gpu, readback;
    NOOP_HR(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, IID_ID3D12Resource, (void**)&upload.p));
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    NOOP_HR(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr, IID_ID3D12Resource, (void**)&gpu.p));
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    NOOP_HR(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr, IID_ID3D12Resource, (void**)&readback.p));
    void* data = nullptr; D3D12_RANGE none{0,0};
    NOOP_HR(upload->Map(0, &none, &data)); memset(data, 0xa7, 64); upload->Unmap(0, nullptr);
    list->CopyBufferRegion(gpu.p, 0, upload.p, 0, 64);
    list->SetPipelineState(nullptr); // Diagnostic mode supplies a real PSO.
    D3D12_VIEWPORT vp{0,0,8,8,0,1}; D3D12_RECT rect{0,0,8,8};
    list->RSSetViewports(1, &vp); list->RSSetScissorRects(1, &rect);
    list->DrawInstanced(3, 1, 0, 0); list->Dispatch(2, 1, 1);
    D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {gpu.p, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE};
    list->ResourceBarrier(1, &barrier); list->CopyBufferRegion(readback.p, 0, gpu.p, 0, 64);
    NOOP_HR(list->Close());
    ID3D12CommandList* lists[]{list.p};
    NOOP_HR(((HRESULT (WINAPI*)(void*, UINT, ID3D12CommandList* const*, UINT64, UINT64))(*(void***)queue.p)[35])(
        queue.p, 1, lists, 0, 0));
    NOOP_HR(queue->Signal(fence.p, 1));
    const auto deadline = GetTickCount64() + 10000;
    while (fence->GetCompletedValue() < 1 && GetTickCount64() < deadline) Sleep(1);
    if (fence->GetCompletedValue() < 1) return false;
    D3D12_RANGE range{0,64}; NOOP_HR(readback->Map(0, &range, &data));
    unsigned failures = 0;
    for (unsigned i = 0; i < 64; ++i) if (((unsigned char*)data)[i] != 0xa7) ++failures;
    readback->Unmap(0, &none);
    printf("[BOOTSTRAP] native PSO57/55/67 Draw/Dispatch/Barrier/Execute GPU guard64 failures=%u\n", failures);
    return !failures;
#undef NOOP_HR
}
