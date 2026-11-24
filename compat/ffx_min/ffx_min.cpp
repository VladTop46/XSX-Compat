// ffx_min — минимальная замена amd_fidelityfx_loader_xbox.dll для первого кадра.
// Реализует только то, что игра использует (статически: create 0x10000+0x1000b, query 0x10004/0x10005/0x10008,
// dispatch 0x10001, destroy). Upscale = билинейный compute (без temporal accumulation, без sharpening, без reactive/
// transparency/depth/MV): это НЕ FSR и не имитирует его качество. Конвенция джиттера — из FSR2 (src = pixel+0.5-jitter).
#define INITGUID
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <ffx_api.h>
#include <ffx_upscale.h>
#include <xbox/ffx_api_xbox.h>

#define LOG(...) do { fprintf(stderr, "[FFX_MIN] " __VA_ARGS__); fputc('\n', stderr); fflush(stderr); } while (0)
enum { RC_OK = 0, RC_ERROR = 1, RC_UNKNOWN_DESCTYPE = 2, RC_RUNTIME = 3, RC_PARAMETER = 6 };
static const int kRing = 64;   // слотов по 2 дескриптора (SRV+UAV); защита от перезаписи при нескольких кадрах в полёте

static const char* kShader = R"(
cbuffer C : register(b0) { uint2 rs; uint2 us; float2 jit; float2 pad; };
Texture2D<float4> src : register(t0);
RWTexture2D<float4> dst : register(u0);
SamplerState smp : register(s0);
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= us)) return;
    float2 hr = (float2(id.xy) + 0.5) / float2(us);
    float2 uv = (hr * float2(rs) + jit) / float2(rs);   // un-jitter: src_pos = hr_pos*scale + jitter
    dst[id.xy] = src.SampleLevel(smp, uv, 0);
}
)";

struct Ctx {
    ID3D12Device* dev = nullptr;
    ID3D12RootSignature* rs = nullptr;
    ID3D12PipelineState* pso = nullptr;
    ID3D12DescriptorHeap* heap = nullptr;
    UINT inc = 0, slot = 0;
    uint32_t flags = 0;
    FfxApiDimensions2D maxRender{}, maxUp{};
};

static DXGI_FORMAT ViewFormat(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:     return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:  return DXGI_FORMAT_R10G10B10A2_UNORM;
    default: return f;
    }
}

static bool EnsurePipeline(Ctx* c)
{
    if (c->pso) return true;
    static HMODULE comp = LoadLibraryW(L"d3dcompiler_47.dll");
    using CompileFn = decltype(&D3DCompile);
    auto compile = comp ? (CompileFn)GetProcAddress(comp, "D3DCompile") : nullptr;
    if (!compile) { LOG("d3dcompiler_47.dll unavailable"); return false; }
    ID3DBlob *cs = nullptr, *err = nullptr;
    HRESULT hr = compile(kShader, strlen(kShader), "ffx_min", nullptr, nullptr, "main", "cs_5_0", 0, 0, &cs, &err);
    if (FAILED(hr)) { LOG("shader compile failed 0x%08lX: %s", (unsigned long)hr, err ? (char*)err->GetBufferPointer() : ""); return false; }

    D3D12_DESCRIPTOR_RANGE rg[2] = {
        { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 },
        { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 1 } };
    D3D12_ROOT_PARAMETER rp[2] = {};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; rp[0].Constants = { 0, 0, 8 };
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; rp[1].DescriptorTable = { 2, rg };
    D3D12_STATIC_SAMPLER_DESC ss = {};
    ss.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    ss.AddressU = ss.AddressV = ss.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    ss.MaxLOD = D3D12_FLOAT32_MAX; ss.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    ss.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rd = { 2, rp, 1, &ss, D3D12_ROOT_SIGNATURE_FLAG_NONE };

    static HMODULE d3d = LoadLibraryW(L"d3d12.dll");
    using SerFn = HRESULT (WINAPI*)(const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**, ID3DBlob**);
    auto ser = d3d ? (SerFn)GetProcAddress(d3d, "D3D12SerializeRootSignature") : nullptr;
    ID3DBlob* sig = nullptr;
    if (!ser || FAILED(hr = ser(&rd, D3D_ROOT_SIGNATURE_VERSION_1_0, &sig, &err))) { LOG("root sig serialize failed"); return false; }
    if (FAILED(hr = c->dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_ID3D12RootSignature, (void**)&c->rs)))
        { LOG("CreateRootSignature 0x%08lX", (unsigned long)hr); return false; }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = c->rs; pd.CS = { cs->GetBufferPointer(), cs->GetBufferSize() };
    if (FAILED(hr = c->dev->CreateComputePipelineState(&pd, IID_ID3D12PipelineState, (void**)&c->pso)))
        { LOG("CreateComputePipelineState 0x%08lX", (unsigned long)hr); return false; }
    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2 * kRing, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
    if (FAILED(hr = c->dev->CreateDescriptorHeap(&hd, IID_ID3D12DescriptorHeap, (void**)&c->heap)))
        { LOG("CreateDescriptorHeap 0x%08lX", (unsigned long)hr); return false; }
    c->inc = c->dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    LOG("pipeline ready");
    return true;
}

static float Halton(int i, int b) { float f = 1, r = 0; while (i > 0) { f /= b; r += f * (i % b); i /= b; } return r; }

extern "C" {

FFX_API_ENTRY ffxReturnCode_t ffxCreateContext(ffxContext* context, ffxCreateContextDescHeader* desc, const ffxAllocationCallbacks*)
{
    if (!context || !desc) return RC_PARAMETER;
    auto* c = new Ctx();
    bool haveUpscale = false;
    for (ffxApiHeader* h = desc; h; h = h->pNext) {
        if (h->type == FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE) {
            auto* u = reinterpret_cast<ffxCreateContextDescUpscale*>(h);
            c->flags = u->flags; c->maxRender = u->maxRenderSize; c->maxUp = u->maxUpscaleSize; haveUpscale = true;
        } else if (h->type == FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_XBOX) {
            c->dev = reinterpret_cast<ffxCreateBackendXboxDesc*>(h)->device;
        }
    }
    LOG("CreateContext upscale=%d flags=0x%X render=%ux%u up=%ux%u device=%p", haveUpscale, c->flags,
        c->maxRender.width, c->maxRender.height, c->maxUp.width, c->maxUp.height, (void*)c->dev);
    if (!haveUpscale) { delete c; return RC_UNKNOWN_DESCTYPE; }
    *context = c;
    return RC_OK;
}

FFX_API_ENTRY ffxReturnCode_t ffxDestroyContext(ffxContext* context, const ffxAllocationCallbacks*)
{
    if (!context) return RC_PARAMETER;
    auto* c = static_cast<Ctx*>(*context);
    if (c) { if (c->heap) c->heap->Release(); if (c->pso) c->pso->Release(); if (c->rs) c->rs->Release(); delete c; }
    *context = nullptr; LOG("DestroyContext");
    return RC_OK;
}

FFX_API_ENTRY ffxReturnCode_t ffxConfigure(ffxContext*, const ffxConfigureDescHeader* desc)
{
    LOG("Configure type=0x%llx (ignored)", desc ? (unsigned long long)desc->type : 0ULL);
    return RC_OK;
}

FFX_API_ENTRY ffxReturnCode_t ffxQuery(ffxContext*, ffxQueryDescHeader* desc)
{
    if (!desc) return RC_PARAMETER;
    switch (desc->type) {
    case FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTERPHASECOUNT: {
        auto* q = reinterpret_cast<ffxQueryDescUpscaleGetJitterPhaseCount*>(desc);
        if (!q->pOutPhaseCount || !q->renderWidth) return RC_PARAMETER;
        float r = float(q->displayWidth) / float(q->renderWidth);
        *q->pOutPhaseCount = (int32_t)ceilf(8.0f * r * r);          // формула FSR2
        return RC_OK; }
    case FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTEROFFSET: {
        auto* q = reinterpret_cast<ffxQueryDescUpscaleGetJitterOffset*>(desc);
        if (!q->pOutX || !q->pOutY || q->phaseCount <= 0) return RC_PARAMETER;
        int i = (q->index % q->phaseCount) + 1;
        *q->pOutX = Halton(i, 2) - 0.5f; *q->pOutY = Halton(i, 3) - 0.5f;   // Halton(2,3), как FSR2
        return RC_OK; }
    case FFX_API_QUERY_DESC_TYPE_UPSCALE_GPU_MEMORY_USAGE: {
        auto* q = reinterpret_cast<ffxQueryDescUpscaleGetGPUMemoryUsage*>(desc);
        if (q->gpuMemoryUsageUpscaler) *q->gpuMemoryUsageUpscaler = {0, 0};
        return RC_OK; }
    case FFX_API_QUERY_DESC_TYPE_UPSCALE_GETUPSCALERATIOFROMQUALITYMODE: {
        auto* q = reinterpret_cast<ffxQueryDescUpscaleGetUpscaleRatioFromQualityMode*>(desc);
        static const float k[] = { 1.0f, 1.5f, 1.7f, 2.0f, 3.0f };
        if (!q->pOutUpscaleRatio || q->qualityMode > 4) return RC_PARAMETER;
        *q->pOutUpscaleRatio = k[q->qualityMode]; return RC_OK; }
    case FFX_API_QUERY_DESC_TYPE_UPSCALE_GETRENDERRESOLUTIONFROMQUALITYMODE: {
        auto* q = reinterpret_cast<ffxQueryDescUpscaleGetRenderResolutionFromQualityMode*>(desc);
        static const float k[] = { 1.0f, 1.5f, 1.7f, 2.0f, 3.0f };
        if (!q->pOutRenderWidth || !q->pOutRenderHeight || q->qualityMode > 4) return RC_PARAMETER;
        *q->pOutRenderWidth = (uint32_t)(q->displayWidth / k[q->qualityMode]);
        *q->pOutRenderHeight = (uint32_t)(q->displayHeight / k[q->qualityMode]); return RC_OK; }
    default:
        LOG("Query: unsupported type 0x%llx", (unsigned long long)desc->type);
        return RC_UNKNOWN_DESCTYPE;
    }
}

FFX_API_ENTRY ffxReturnCode_t ffxDispatch(ffxContext* context, const ffxDispatchDescHeader* desc)
{
    if (!context || !*context || !desc) return RC_PARAMETER;
    if (desc->type != FFX_API_DISPATCH_DESC_TYPE_UPSCALE) {
        LOG("Dispatch: unsupported type 0x%llx", (unsigned long long)desc->type);
        return RC_UNKNOWN_DESCTYPE;
    }
    auto* c = static_cast<Ctx*>(*context);
    auto* d = reinterpret_cast<const ffxDispatchDescUpscale*>(desc);
    auto* list = static_cast<ID3D12GraphicsCommandList*>(d->commandList);
    auto* src = static_cast<ID3D12Resource*>(d->color.resource);
    auto* dst = static_cast<ID3D12Resource*>(d->output.resource);
    if (!list || !src || !dst) return RC_PARAMETER;
    if (!c->dev) list->GetDevice(IID_ID3D12Device, (void**)&c->dev);   // device берём у списка, если не передан backend desc
    if (!c->dev || !EnsurePipeline(c)) return RC_RUNTIME;

    D3D12_RESOURCE_DESC sd = src->GetDesc(), od = dst->GetDesc();
    UINT slot = (c->slot++) % kRing;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = c->heap->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += SIZE_T(slot) * 2 * c->inc;
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
    sv.Format = ViewFormat(sd.Format); sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; sv.Texture2D.MipLevels = 1;
    c->dev->CreateShaderResourceView(src, &sv, cpu);
    cpu.ptr += c->inc;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uv = {};
    uv.Format = ViewFormat(od.Format); uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    c->dev->CreateUnorderedAccessView(dst, nullptr, &uv, cpu);

    uint32_t rw = d->renderSize.width ? d->renderSize.width : (uint32_t)sd.Width;
    uint32_t rh = d->renderSize.height ? d->renderSize.height : sd.Height;
    uint32_t uw = d->upscaleSize.width ? d->upscaleSize.width : (uint32_t)od.Width;
    uint32_t uh = d->upscaleSize.height ? d->upscaleSize.height : od.Height;
    struct { uint32_t rs[2], us[2]; float jit[2], pad[2]; } k = { {rw, rh}, {uw, uh}, {d->jitterOffset.x, d->jitterOffset.y}, {0, 0} };

    ID3D12DescriptorHeap* heaps[] = { c->heap };
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(c->rs);
    list->SetPipelineState(c->pso);
    list->SetComputeRoot32BitConstants(0, 8, &k, 0);
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = c->heap->GetGPUDescriptorHandleForHeapStart();
    gpu.ptr += UINT64(slot) * 2 * c->inc;
    list->SetComputeRootDescriptorTable(1, gpu);
    list->Dispatch((uw + 7) / 8, (uh + 7) / 8, 1);
    return RC_OK;
}

} // extern "C"
