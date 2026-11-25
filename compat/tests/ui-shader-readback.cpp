// Executes all four early UI VS+PS pairs on controlled native D3D12 fixtures.
// No game/runtime DLLs, no Xbox wrapper, no shader substitution. This checks
// lowering and binding contracts, not equivalence to uncaptured Xbox pixels.
#define INITGUID
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include "../d3d12_xs/gfx_blend.h"
#include "ui-shader-data.h"

template<class T> struct Ref {
    T* p=nullptr;
    Ref()=default;
    Ref(const Ref&)=delete;
    Ref& operator=(const Ref&)=delete;
    ~Ref(){if(p)p->Release();}
    T* operator->()const{return p;}
};
static void require(bool ok,const char* what)
{
    if(!ok){std::printf("[FAIL] UI %s\n",what);throw std::runtime_error(what);}
}
static void hr(HRESULT value,const char* what)
{
    if(FAILED(value)){
        std::printf("[FAIL] UI %s hr=0x%08lx\n",what,(unsigned long)value);
        throw std::runtime_error(what);
    }
}
#define HR(expr) hr((expr),#expr)
struct Event { HANDLE p=CreateEventW(nullptr,FALSE,FALSE,nullptr); ~Event(){if(p)CloseHandle(p);} };
constexpr UINT W=8,H=8,Pitch=256,ReadOffset=512,ReadSize=3072;
constexpr UINT UploadSize=65536,Buffer1=4096,Constants=8192,Vertices=12288,Indices=16384,TexUpload=32768,GuardUpload=49152;
static const float background[4]={.0625f,.125f,.25f,.5f};
static const float color[4]={.31f,.57f,.83f,.67f};

static D3D12_RESOURCE_DESC bufferDesc(UINT64 size)
{
    D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=size;
    d.Height=d.DepthOrArraySize=d.MipLevels=1;d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return d;
}
static D3D12_RESOURCE_DESC imageDesc(bool output)
{
    D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width=W;d.Height=H;d.DepthOrArraySize=d.MipLevels=1;d.SampleDesc.Count=1;
    d.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;
    d.Flags=output?D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET:D3D12_RESOURCE_FLAG_NONE;
    return d;
}
static void transition(ID3D12GraphicsCommandList* l,ID3D12Resource* r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b)
{
    D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,a,b};l->ResourceBarrier(1,&barrier);
}
static void resource(ID3D12Device* dev,D3D12_HEAP_TYPE type,const D3D12_RESOURCE_DESC& desc,
    D3D12_RESOURCE_STATES state,Ref<ID3D12Resource>& out)
{
    D3D12_HEAP_PROPERTIES hp{};hp.Type=type;
    HR(dev->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&desc,state,nullptr,IID_PPV_ARGS(&out.p)));
}
struct Context {
    Ref<ID3D12Device> device;
    Ref<ID3D12CommandQueue> queue;
    Ref<ID3D12RootSignature> root;
    Ref<ID3D12Fence> fence;
    Event event;
    UINT64 completion=0;
    Context(){
        require(event.p!=nullptr,"create completion event");
        HR(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&device.p)));
        D3D12_COMMAND_QUEUE_DESC q{};HR(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue.p)));
        HR(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence.p)));
        D3D12_DESCRIPTOR_RANGE ranges[2]={{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,2,0,1,0},
            {D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,0,2,0}};
        D3D12_ROOT_PARAMETER params[3]{};
        for(unsigned i=0;i<2;++i){params[i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            params[i].DescriptorTable={1,&ranges[i]};params[i].ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;}
        params[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;params[2].Descriptor={0,2};
        D3D12_STATIC_SAMPLER_DESC sampler{};sampler.Filter=D3D12_FILTER_MIN_MAG_MIP_POINT;
        sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.ComparisonFunc=D3D12_COMPARISON_FUNC_NEVER;sampler.MaxLOD=D3D12_FLOAT32_MAX;
        sampler.MaxAnisotropy=1;sampler.RegisterSpace=1;sampler.ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC desc{3,params,1,&sampler,D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
        Ref<ID3DBlob> blob,error;HR(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1_0,&blob.p,&error.p));
        HR(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root.p)));
    }
    void execute(ID3D12GraphicsCommandList* list){
        HR(list->Close());ID3D12CommandList* lists[]={list};queue->ExecuteCommandLists(1,lists);
        HR(queue->Signal(fence.p,++completion));HR(fence->SetEventOnCompletion(completion,event.p));
        require(WaitForSingleObject(event.p,10000)==WAIT_OBJECT_0,"GPU completion timeout");
        const UINT64 done=fence->GetCompletedValue();
        require(done!=UINT64_MAX && done>=completion,"GPU completion/device status");
    }
};

// Independent mathematical half-precision truncation for the finite fixture
// range. No implementation of the HLSL helper or packed-bit decoder is reused.
static float halfRtz(float value)
{
    require(std::isfinite(value) && std::fabs(value)<65504,"finite half reference range");
    if(value==0)return value;
    int exponent=0;const double magnitude=std::fabs((double)value);std::frexp(magnitude,&exponent);
    const double unit=std::ldexp(1.0,std::max(exponent-11,-24));
    return std::copysign((float)(std::floor(magnitude/unit)*unit),value);
}
static void texel(unsigned x,unsigned y,float out[4])
{
    out[0]=(x+1)/16.f;out[1]=(y+1)/16.f;out[2]=((x^y)+1)/16.f;out[3]=.5f;
}
static void reference(unsigned pair,unsigned variant,unsigned x,unsigned y,float out[4])
{
    const bool inside=!variant || (x>=2 && x<6 && y>=2 && y<6);
    if(!inside){std::memcpy(out,background,sizeof background);return;}
    float source[4]{};
    if(pair==0){
        const unsigned tx=variant?2*(x-2)+1:x,ty=variant?2*(y-2)+1:y;
        texel(tx,ty,source);for(unsigned c=0;c<4;++c)source[c]=halfRtz(source[c]*color[c]);
    }else if(pair==1){
        const float tint[4]={.75f,.5f,1.25f,.625f};
        for(unsigned c=0;c<4;++c)source[c]=halfRtz(color[c]*tint[c]);
    }else{
        const float add[4]={.125f,-.125f,.0625f,.03125f},mul[4]={.5f,1.5f,.25f,1.f};
        for(unsigned c=0;c<4;++c){
            float v=std::clamp(std::fma(color[c],mul[c],add[c]),0.f,1.f);
            if(c==3)v*=.5f;
            source[c]=halfRtz(v);
        }
    }
    if(!variant){std::memcpy(out,source,sizeof source);return;}
    // Explicit independent equations for the three observed control words.
    // Not derived by calling NativeBlend or DecodeBlendControl.
    const float alpha=source[3];
    for(unsigned c=0;c<4;++c){
        const float srcFactor=(pair==0 || (pair>=2 && c==3))?1.f:alpha;
        out[c]=source[c]*srcFactor+background[c]*(1.f-alpha);
    }
}

struct Vertex {float position[3],color[4],uv[2];};
struct ScaleVertex {float color[4],secondary[4],position[2];};
static bool runCase(Context& context,unsigned pair,unsigned variant)
{
    auto* dev=context.device.p;
    Ref<ID3D12PipelineState> pipeline;
    D3D12_INPUT_ELEMENT_DESC elements[3]{};
    if(pair<2){
        elements[0]={"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0};
        elements[1]={"COLOR",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,12,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0};
        elements[2]={"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,0,28,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0};
    }else{
        elements[0]={"COLOR",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,0,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0};
        elements[1]={"COLOR",1,DXGI_FORMAT_R32G32B32A32_FLOAT,0,16,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0};
        elements[2]={"POSITION",0,DXGI_FORMAT_R32G32_FLOAT,0,32,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0};
    }
    static const uint32_t control[]={0xe5010501,0xe5040504,0xe5010504,0xe5010504};
    const uint32_t word=variant?control[pair]:control[pair]&~(1u<<30);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC p{};p.pRootSignature=context.root.p;
    p.VS=shaderPairs[pair].vs;p.PS=shaderPairs[pair].ps;p.InputLayout={elements,3};
    require(xsx::gfx::NativeBlend(word,p.BlendState.RenderTarget[0]),"map explicit blend control");
    p.SampleMask=~UINT(0);p.SampleDesc.Count=1;p.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    p.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;p.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;
    p.RasterizerState.DepthClipEnable=TRUE;p.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_ALWAYS;
    p.NumRenderTargets=1;p.RTVFormats[0]=DXGI_FORMAT_R32G32B32A32_FLOAT;
    HR(dev->CreateGraphicsPipelineState(&p,IID_PPV_ARGS(&pipeline.p)));
    std::printf("[PSO] UI pair=%u variant=%u S_OK VS=%s PS=%s blend=%#x\n",pair+1,variant,shaderPairs[pair].vsHash,shaderPairs[pair].psHash,word);
    Ref<ID3D12Resource> upload,target,texture,readback;
    resource(dev,D3D12_HEAP_TYPE_UPLOAD,bufferDesc(UploadSize),D3D12_RESOURCE_STATE_GENERIC_READ,upload);
    resource(dev,D3D12_HEAP_TYPE_DEFAULT,imageDesc(true),D3D12_RESOURCE_STATE_RENDER_TARGET,target);
    resource(dev,D3D12_HEAP_TYPE_DEFAULT,imageDesc(false),D3D12_RESOURCE_STATE_COPY_DEST,texture);
    resource(dev,D3D12_HEAP_TYPE_READBACK,bufferDesc(ReadSize),D3D12_RESOURCE_STATE_COPY_DEST,readback);
    void* mapped=nullptr;D3D12_RANGE none{0,0};HR(upload->Map(0,&none,&mapped));
    auto* bytes=(unsigned char*)mapped;std::memset(bytes,0xcd,UploadSize);
    float a[1024]{},b[1024]{};const float scale=variant?.5f:1.f;
    if(pair==0){
        a[0]=a[5]=2;a[10]=1;a[12]=.5f;a[13]=-.25f;
        const float second=scale*.5f;
        b[88]=b[93]=second;b[98]=1;b[100]=-.5f*second;b[101]=.25f*second;b[103]=1;
    }else if(pair==1){
        const float tint[4]={.75f,.5f,1.25f,.625f};std::memcpy(a+8,tint,sizeof tint);
    }else{
        const unsigned base=(pair==3 && variant)?16:0; // Batch1 selects records4..7.
        const float add[4]={.125f,-.125f,.0625f,.03125f},mul[4]={.5f,1.5f,.25f,1.f};
        std::memcpy(a+base,add,sizeof add);std::memcpy(a+base+4,mul,sizeof mul);
        a[base+8]=scale;a[base+13]=scale;
    }
    std::memcpy(bytes,a,sizeof a);std::memcpy(bytes+Buffer1,b,sizeof b);
    const uint32_t vsharp[8]={16,256,0,0,16,256,0,0};std::memcpy(bytes+Constants,vsharp,sizeof vsharp);
    const float positions[4][2]={{-1,1},{1,1},{-1,-1},{1,-1}};
    const uint16_t order[6]={0,1,2,2,1,3};
    if(pair<2){
        Vertex vertices[6]{};
        for(unsigned i=0;i<6;++i){const auto& pos=positions[order[i]];
            vertices[i].position[0]=pos[0]*(pair==1?scale:1.f);
            vertices[i].position[1]=pos[1]*(pair==1?scale:1.f);
            std::memcpy(vertices[i].color,color,sizeof color);
            vertices[i].uv[0]=(pos[0]+1)*.5f;vertices[i].uv[1]=(1-pos[1])*.5f;}
        std::memcpy(bytes+Vertices,vertices,sizeof vertices);
    }else{
        ScaleVertex vertices[4]{};
        for(unsigned i=0;i<4;++i){std::memcpy(vertices[i].color,color,sizeof color);
            vertices[i].secondary[2]=(pair==3 && variant)?1.f/255.f:0.f;
            vertices[i].secondary[3]=.5f;std::memcpy(vertices[i].position,positions[i],sizeof positions[i]);}
        std::memcpy(bytes+Vertices,vertices,sizeof vertices);std::memcpy(bytes+Indices,order,sizeof order);
    }
    for(unsigned y=0;y<H;++y)for(unsigned x=0;x<W;++x){float t[4];texel(x,y,t);std::memcpy(bytes+TexUpload+y*Pitch+x*16,t,sizeof t);}
    upload->Unmap(0,nullptr);
    Ref<ID3D12DescriptorHeap> rtvHeap,views;
    D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;hd.NumDescriptors=1;
    HR(dev->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&rtvHeap.p)));
    const auto rtv=rtvHeap->GetCPUDescriptorHandleForHeapStart();dev->CreateRenderTargetView(target.p,nullptr,rtv);
    hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;hd.NumDescriptors=3;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    HR(dev->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&views.p)));
    const UINT step=dev->GetDescriptorHandleIncrementSize(hd.Type);auto cpu=views->GetCPUDescriptorHandleForHeapStart();
    for(unsigned i=0;i<2;++i){D3D12_SHADER_RESOURCE_VIEW_DESC d{};d.Format=DXGI_FORMAT_R32_TYPELESS;
        d.ViewDimension=D3D12_SRV_DIMENSION_BUFFER;d.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        d.Buffer.FirstElement=i*Buffer1/4;d.Buffer.NumElements=1024;d.Buffer.Flags=D3D12_BUFFER_SRV_FLAG_RAW;
        dev->CreateShaderResourceView(upload.p,&d,cpu);cpu.ptr+=step;}
    dev->CreateShaderResourceView(texture.p,nullptr,cpu);
    Ref<ID3D12CommandAllocator> allocator;Ref<ID3D12GraphicsCommandList> list;
    HR(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator.p)));
    HR(dev->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.p,pipeline.p,IID_PPV_ARGS(&list.p)));
    list->CopyBufferRegion(readback.p,0,upload.p,GuardUpload,ReadSize);
    D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=upload.p;src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint={TexUpload,{DXGI_FORMAT_R32G32B32A32_FLOAT,W,H,1,Pitch}};
    dst.pResource=texture.p;dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);transition(list.p,texture.p,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    list->ClearRenderTargetView(rtv,background,0,nullptr);list->OMSetRenderTargets(1,&rtv,FALSE,nullptr);
    D3D12_VIEWPORT vp{0,0,(float)W,(float)H,0,1};D3D12_RECT scissor{0,0,W,H};
    list->RSSetViewports(1,&vp);list->RSSetScissorRects(1,&scissor);list->SetGraphicsRootSignature(context.root.p);
    ID3D12DescriptorHeap* heaps[]={views.p};list->SetDescriptorHeaps(1,heaps);
    auto gpu=views->GetGPUDescriptorHandleForHeapStart();list->SetGraphicsRootDescriptorTable(0,gpu);
    gpu.ptr+=2*step;list->SetGraphicsRootDescriptorTable(1,gpu);
    list->SetGraphicsRootConstantBufferView(2,upload->GetGPUVirtualAddress()+Constants);
    D3D12_VERTEX_BUFFER_VIEW vb{upload->GetGPUVirtualAddress()+Vertices,pair<2?UINT(sizeof(Vertex)*6):UINT(sizeof(ScaleVertex)*4),pair<2?UINT(sizeof(Vertex)):UINT(sizeof(ScaleVertex))};
    list->IASetVertexBuffers(0,1,&vb);list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    if(pair<2)list->DrawInstanced(6,1,0,0);
    else {D3D12_INDEX_BUFFER_VIEW ib{upload->GetGPUVirtualAddress()+Indices,sizeof order,DXGI_FORMAT_R16_UINT};
        list->IASetIndexBuffer(&ib);list->DrawIndexedInstanced(6,1,0,0,0);}
    transition(list.p,target.p,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE);
    src={};dst={};src.pResource=target.p;src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource=readback.p;dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint={ReadOffset,{DXGI_FORMAT_R32G32B32A32_FLOAT,W,H,1,Pitch}};
    list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);context.execute(list.p);
    D3D12_RANGE read{0,ReadSize};HR(readback->Map(0,&read,&mapped));
    const auto* output=(const unsigned char*)mapped;unsigned errors=0,guardErrors=0;float maxError=0;
    for(unsigned y=0;y<H;++y)for(unsigned x=0;x<W;++x){
        float actual[4],expected[4];std::memcpy(actual,output+ReadOffset+y*Pitch+x*16,sizeof actual);reference(pair,variant,x,y,expected);
        for(unsigned c=0;c<4;++c){const float error=std::fabs(actual[c]-expected[c]);
            if(!std::isfinite(actual[c]) || error>2e-5f){if(errors<3)std::printf("[PIXEL] pair=%u variant=%u x=%u y=%u c=%u expected=%.9g got=%.9g\n",pair+1,variant,x,y,c,expected[c],actual[c]);++errors;}
            if(std::isfinite(error))maxError=std::max(maxError,error);
        }
    }
    for(unsigned i=0;i<ReadSize;++i){
        const bool pixel=i>=ReadOffset && i<ReadOffset+H*Pitch && ((i-ReadOffset)%Pitch)<W*16;
        if(!pixel && output[i]!=0xcd)++guardErrors;
    }
    readback->Unmap(0,&none);
    std::printf("[UI] pair=%u variant=%u pixels=64 errors=%u max_error=%.9g guard_errors=%u indexed=%u batch=%u\n",pair+1,variant,errors,maxError,guardErrors,pair>=2,pair==3?variant:0);
    return !errors && !guardErrors;
}
int main()
{
    std::setvbuf(stdout,nullptr,_IONBF,0);
    try{
        Context context;unsigned failures=0;
        for(unsigned pair=0;pair<4;++pair)for(unsigned variant=0;variant<2;++variant)
            if(!runCase(context,pair,variant))++failures;
        std::printf("[%s] UI shaders programs=8 pairs=4 cases=8 pixels=512 failures=%u scope=translated-programs-controlled-fixtures\n",failures?"FAIL":"PASS",failures);
        return failures?1:0;
    }catch(const std::exception& e){std::printf("[FAIL] UI shader probe stage=%s\n",e.what());return 2;}
}
