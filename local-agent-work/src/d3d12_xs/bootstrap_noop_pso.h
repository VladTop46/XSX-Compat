#pragma once

// Synthetic graphics + empty compute bootstrap, opt-in via XSX_DEV_NOOP_PSO=1.
// Own cache references remain alive like the existing compute placeholder.
struct BootstrapPipelines {
    ID3D12Device* device;
    ID3D12RootSignature* root;
    ID3D12PipelineState* graphics;
};
static BootstrapPipelines GetBootstrapPipelines(ID3D12Device* dev)
{
    static SRWLOCK lock = SRWLOCK_INIT;
    ExclusiveSrwLock held(lock);
    static std::vector<BootstrapPipelines> cache;
    for (const auto& item : cache) if (item.device == dev) return item;
    struct Temporary {
        HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
        ID3DBlob *vs = nullptr, *signature = nullptr, *error = nullptr;
        ID3D12RootSignature* root = nullptr;
        ~Temporary() {
            if (vs) vs->Release(); if (signature) signature->Release();
            if (error) error->Release(); if (root) root->Release();
            if (compiler) FreeLibrary(compiler);
        }
    } tmp;
    auto compile = tmp.compiler ? (decltype(&D3DCompile))GetProcAddress(tmp.compiler, "D3DCompile") : nullptr;
    // Every vertex is identical and outside the clip volume: no fragments.
    const char source[] = "float4 main():SV_Position { return float4(2,2,0,1); }";
    if (!compile || FAILED(compile(source, sizeof source-1, "bootstrap-noop", nullptr, nullptr,
        "main", "vs_5_0", 0, 0, &tmp.vs, &tmp.error))) return {};
    if (tmp.error) { tmp.error->Release(); tmp.error = nullptr; }
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC rd{}; rd.Version = D3D_ROOT_SIGNATURE_VERSION_1_0;
    if (FAILED(xsSerializeVersionedRootSignature(&rd, &tmp.signature, &tmp.error)) ||
        FAILED(dev->CreateRootSignature(0, tmp.signature->GetBufferPointer(), tmp.signature->GetBufferSize(),
            __uuidof(ID3D12RootSignature), (void**)&tmp.root))) return {};
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = tmp.root; pd.VS = {tmp.vs->GetBufferPointer(), tmp.vs->GetBufferSize()};
    pd.SampleMask = ~UINT(0); pd.SampleDesc.Count = 1;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    ID3D12PipelineState* pso = nullptr;
    const HRESULT hr = dev->CreateGraphicsPipelineState(&pd, __uuidof(ID3D12PipelineState), (void**)&pso);
    if (FAILED(hr)) { XS_LOG("BOOTSTRAP graphics PSO failed hr=0x%08lX", (unsigned long)hr); return {}; }
    cache.push_back({dev, tmp.root, pso}); tmp.root = nullptr;
    XS_LOG("BOOTSTRAP valid native graphics PSO=%p empty_root=%p no_game_shader=1", (void*)pso, (void*)cache.back().root);
    return cache.back();
}
static HRESULT BootstrapGraphicsOutput(Wrap* w, REFIID iid, void** out)
{
    const auto pipelines = GetBootstrapPipelines((ID3D12Device*)w->real);
    if (!pipelines.graphics) return E_FAIL;
    const HRESULT hr = WrapOutput(pipelines.graphics->QueryInterface(iid, out), out, g_vtPso, 5);
    if (SUCCEEDED(hr)) {
        const unsigned count = ++g_bootstrap.pso;
        if (count <= 8 || !(count % 1000)) XS_LOG("BOOTSTRAP native57 -> valid no-op graphics PSO count=%u", count);
    }
    return hr;
}
static bool BindBootstrapGraphics(Wrap* w)
{
    PROF_SCOPE(PROF_DRAW_NOOP);
    auto* list = (ID3D12GraphicsCommandList*)w->real;
    ID3D12Device* dev = nullptr;
    if (FAILED(list->GetDevice(__uuidof(ID3D12Device), (void**)&dev))) return false;
    const auto pipelines = GetBootstrapPipelines(dev); dev->Release();
    if (!pipelines.graphics) { XS_LOG("BOOTSTRAP Draw skipped: no valid native PSO"); return false; }
    list->SetPipelineState(pipelines.graphics); list->SetGraphicsRootSignature(pipelines.root);
    // No-op VS consumes no IA or descriptors. Normalize topology ONLY in this
    // diagnostic mode; Xbox topology0x11 is not translated by this assignment.
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    return true;
}
static bool BindBootstrapCompute(Wrap* w)
{
    auto* list = (ID3D12GraphicsCommandList*)w->real;
    ID3D12Device* dev = nullptr;
    if (FAILED(list->GetDevice(__uuidof(ID3D12Device), (void**)&dev))) return false;
    auto* compute = GetStubComputePso(dev);
    const auto pipelines = GetBootstrapPipelines(dev); dev->Release();
    if (!compute || !pipelines.root) { XS_LOG("BOOTSTRAP Dispatch skipped: no valid native PSO"); return false; }
    list->SetPipelineState(compute); list->SetComputeRootSignature(pipelines.root);
    return true;
}
