#pragma once
// Private native PC presentation pass. It samples a host texture; it neither
// translates title shaders nor synchronizes a title's CPU shadow with the GPU.
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <cstring>
#include <climits>

namespace xsx::presentation {
template<class T> class Ref {
    T* value_ = nullptr;
public:
    Ref() = default;
    Ref(const Ref&) = delete;
    Ref& operator=(const Ref&) = delete;
    ~Ref() { Reset(); }
    T* Get() const { return value_; }
    T* operator->() const { return value_; }
    void Reset() { T* old = value_; value_ = nullptr; if (old) old->Release(); }
    T** Put() { Reset(); return &value_; }
    void Set(T* value) { if (value) value->AddRef(); Reset(); value_ = value; }
};
template<class Fn> inline Fn Symbol(HMODULE module, const char* name)
{
    Fn result = nullptr;
    FARPROC raw = module ? GetProcAddress(module, name) : nullptr;
    static_assert(sizeof result == sizeof raw);
    std::memcpy(&result, &raw, sizeof result);
    return result;
}
inline void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
    D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    if (before == after) return;
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before; barrier.Transition.StateAfter = after;
    list->ResourceBarrier(1, &barrier);
}
class SurfaceBlit {
    Ref<ID3D12Device> device_;
    Ref<ID3D12RootSignature> root_;
    Ref<ID3D12PipelineState> pipeline_;
    Ref<ID3D12DescriptorHeap> views_;
    Ref<ID3D12Resource> source_;
public:
    // Deliberately narrow color contract: no HDR tone mapping/sRGB conversion.
    static bool Supports(const D3D12_RESOURCE_DESC& desc)
    {
        return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
            desc.DepthOrArraySize == 1 && desc.MipLevels == 1 && desc.SampleDesc.Count == 1 &&
            !(desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) &&
            (desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM || desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM);
    }
    HRESULT Initialize(ID3D12Device* device)
    {
        if (!device) return E_POINTER;
        if (device_.Get()) return device_.Get() == device ? S_OK : E_INVALIDARG;
        struct Module { HMODULE value; ~Module() { if (value) FreeLibrary(value); } };
        // Modules outlive their blob objects, including every error path.
        Module compiler{LoadLibraryW(L"d3dcompiler_47.dll")};
        Module d3d{LoadLibraryW(L"d3d12.dll")};
        auto compile = Symbol<decltype(&D3DCompile)>(compiler.value, "D3DCompile");
        using Serialize = HRESULT (WINAPI*)(const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**, ID3DBlob**);
        auto serialize = Symbol<Serialize>(d3d.value, "D3D12SerializeRootSignature");
        if (!compile || !serialize) return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
        Ref<ID3DBlob> vertex, pixel, signature, error;
        static constexpr char shader[] =
            "struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
            "V vs(uint id : SV_VertexID) { V v; v.uv=float2((id<<1)&2,id&2);"
            "v.pos=float4(v.uv*float2(2,-2)+float2(-1,1),0,1); return v; }\n"
            "Texture2D<float4> image : register(t0); SamplerState linearClamp : register(s0);\n"
            "float4 ps(V v) : SV_Target0 { return image.SampleLevel(linearClamp,v.uv,0); }\n";
        auto shaderCompile = [&](const char* entry, const char* target, Ref<ID3DBlob>& output) {
            const HRESULT hr = compile(shader, sizeof shader - 1, "XSX presentation", nullptr, nullptr,
                entry, target, D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                output.Put(), error.Put());
            if (FAILED(hr) && error.Get()) std::fprintf(stderr, "[BLIT] shader %s: %.*s\n", entry,
                (int)error->GetBufferSize(), (const char*)error->GetBufferPointer());
            return hr;
        };
        HRESULT hr = shaderCompile("vs", "vs_5_0", vertex);
        if (FAILED(hr)) return hr;
        hr = shaderCompile("ps", "ps_5_0", pixel);
        if (FAILED(hr)) return hr;
        D3D12_DESCRIPTOR_RANGE range{};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; range.NumDescriptors = 1;
        D3D12_ROOT_PARAMETER parameter{};
        parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameter.DescriptorTable.NumDescriptorRanges = 1; parameter.DescriptorTable.pDescriptorRanges = &range;
        parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_STATIC_SAMPLER_DESC sampler{};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS; sampler.MaxAnisotropy = 1;
        sampler.MaxLOD = D3D12_FLOAT32_MAX; sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC rootDesc{};
        rootDesc.NumParameters = 1; rootDesc.pParameters = &parameter;
        rootDesc.NumStaticSamplers = 1; rootDesc.pStaticSamplers = &sampler;
        hr = serialize(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, signature.Put(), error.Put());
        if (FAILED(hr)) return hr;
        hr = device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(),
            __uuidof(ID3D12RootSignature), (void**)root_.Put());
        if (FAILED(hr)) return hr;
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
        pso.pRootSignature = root_.Get();
        pso.VS = {vertex->GetBufferPointer(), vertex->GetBufferSize()};
        pso.PS = {pixel->GetBufferPointer(), pixel->GetBufferSize()};
        pso.SampleMask = UINT_MAX;
        pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pso.RasterizerState.DepthClipEnable = TRUE;
        for (auto& blend : pso.BlendState.RenderTarget) {
            blend.SrcBlend = blend.SrcBlendAlpha = D3D12_BLEND_ONE;
            blend.DestBlend = blend.DestBlendAlpha = D3D12_BLEND_ZERO;
            blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
            blend.LogicOp = D3D12_LOGIC_OP_NOOP;
            blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        }
        pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        pso.DepthStencilState.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
        pso.DepthStencilState.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
        pso.DepthStencilState.FrontFace.StencilFailOp = pso.DepthStencilState.FrontFace.StencilDepthFailOp =
            pso.DepthStencilState.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
        pso.DepthStencilState.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        pso.DepthStencilState.BackFace = pso.DepthStencilState.FrontFace;
        pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso.NumRenderTargets = 1; pso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        pso.SampleDesc.Count = 1;
        hr = device->CreateGraphicsPipelineState(&pso, __uuidof(ID3D12PipelineState), (void**)pipeline_.Put());
        if (FAILED(hr)) return hr;
        D3D12_DESCRIPTOR_HEAP_DESC heap{};
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; heap.NumDescriptors = 1;
        heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        hr = device->CreateDescriptorHeap(&heap, __uuidof(ID3D12DescriptorHeap), (void**)views_.Put());
        if (FAILED(hr)) return hr;
        device_.Set(device);
        return S_OK;
    }
    // Caller owns target transitions and must wait for the previous submission
    // before recycling this pass's descriptor or releasing its retained source.
    // Source state is explicit; Record restores it after sampling.
    HRESULT Record(ID3D12GraphicsCommandList* list, ID3D12Resource* source,
        D3D12_RESOURCE_STATES sourceState, UINT width, UINT height, D3D12_CPU_DESCRIPTOR_HANDLE target)
    {
        if (!list || !source) return E_POINTER;
        if (!device_.Get()) return E_UNEXPECTED;
        if (!width || !height || width > 16384 || height > 16384 || !target.ptr) return E_INVALIDARG;
        const auto desc = source->GetDesc();
        if (!Supports(desc)) return E_NOTIMPL;
        source_.Set(source);
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = desc.Format; view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Texture2D.MipLevels = 1;
        device_->CreateShaderResourceView(source, &view, views_->GetCPUDescriptorHandleForHeapStart());
        Transition(list, source, sourceState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        list->SetGraphicsRootSignature(root_.Get()); list->SetPipelineState(pipeline_.Get());
        ID3D12DescriptorHeap* heaps[]{views_.Get()}; list->SetDescriptorHeaps(1, heaps);
        list->SetGraphicsRootDescriptorTable(0, views_->GetGPUDescriptorHandleForHeapStart());
        const D3D12_VIEWPORT viewport{0, 0, (float)width, (float)height, 0, 1};
        const D3D12_RECT scissor{0, 0, (LONG)width, (LONG)height};
        list->RSSetViewports(1, &viewport); list->RSSetScissorRects(1, &scissor);
        list->OMSetRenderTargets(1, &target, FALSE, nullptr);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->DrawInstanced(3, 1, 0, 0);
        Transition(list, source, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, sourceState);
        return S_OK;
    }
    void ReleaseSource() { source_.Reset(); } // Only after GPU completion.
};
} // namespace xsx::presentation
