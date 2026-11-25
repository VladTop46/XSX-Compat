// Exercises slot57 and the actual fork DLL, not a second native PSO builder.
// Private captured ISA/layout fixtures are generated locally by the build script.
#define INITGUID
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <algorithm>
#include <stdexcept>
#include "native-ui-fixtures.h"
static unsigned checks=0;
static void check(bool ok,const char* what) { ++checks;if (!ok) { std::printf("[FAIL] BRIDGE %s\n",what);throw std::runtime_error(what); } }
static void hr(HRESULT value,const char* what) { if (FAILED(value)) std::printf("[HRESULT] %s = %08lx\n",what,(unsigned long)value);check(SUCCEEDED(value),what); }
#define HR(x) hr((x),#x)
static void checkComMethods(void* object) {
    void** vtable=nullptr;void* methods[3]={};SIZE_T bytes=0;
    check(ReadProcessMemory(GetCurrentProcess(),object,&vtable,sizeof vtable,&bytes) && bytes==sizeof vtable,"COM vtable readable");
    check(ReadProcessMemory(GetCurrentProcess(),vtable,methods,sizeof methods,&bytes) && bytes==sizeof methods,"COM methods readable");
    for (auto method:methods) {
        MEMORY_BASIC_INFORMATION info={};
        const DWORD executable=PAGE_EXECUTE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY;
        check(method && VirtualQuery(method,&info,sizeof info)==sizeof info && info.State==MEM_COMMIT &&
            !(info.Protect&(PAGE_GUARD|PAGE_NOACCESS)) && (info.Protect&executable),"COM QI/AddRef/Release executable");
    }
}
template<class T> struct Ref {
    T* p=nullptr;~Ref() { if (p) p->Release(); } T* operator->() const { return p; }
};
using Create=HRESULT(WINAPI*)(IUnknown*,const void*,REFIID,void**);
using NativePso=HRESULT(WINAPI*)(void*,const void*,REFIID,void**);
using Commit=HRESULT(WINAPI*)(void*,const D3D12_HEAP_PROPERTIES*,D3D12_HEAP_FLAGS,const D3D12_RESOURCE_DESC*,D3D12_RESOURCE_STATES,const D3D12_CLEAR_VALUE*,REFIID,void**);
using CreateList=HRESULT(WINAPI*)(void*,const UINT*,ID3D12CommandAllocator*,ID3D12PipelineState*,REFIID,void**);
using SetRT=void(WINAPI*)(void*,UINT,const D3D12_CPU_DESCRIPTOR_HANDLE*,BOOL,const D3D12_CPU_DESCRIPTOR_HANDLE*);
using Clear=void(WINAPI*)(void*,D3D12_CPU_DESCRIPTOR_HANDLE,const float*,UINT,const D3D12_RECT*);
using SetVB=void(WINAPI*)(void*,UINT,UINT,const D3D12_VERTEX_BUFFER_VIEW*);
using SetIB=void(WINAPI*)(void*,const D3D12_INDEX_BUFFER_VIEW*);
using Table=void(WINAPI*)(void*,UINT,UINT64);
using CpuStart=UINT64(WINAPI*)(void*);
constexpr UINT W=8,H=8,Pitch=256,Offset=512,ReadSize=3072;
static const float background[4]={.0625f,.125f,.25f,.5f};
struct Vertex { float position[3];unsigned char color[4];float uv[2]; };
static_assert(sizeof(Vertex)==24);
static D3D12_RESOURCE_DESC buffer(UINT n) {
    D3D12_RESOURCE_DESC d={};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=n;
    d.Height=d.DepthOrArraySize=d.MipLevels=1;d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;return d;
}
static void transition(ID3D12GraphicsCommandList* list,ID3D12Resource* resource,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b={};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after};list->ResourceBarrier(1,&b);
}
static float halfRtz(float v) {
    if (!v) return v;
    int exponent=0;std::frexp(std::fabs((double)v),&exponent);
    const double unit=std::ldexp(1.,std::max(exponent-11,-24));return std::copysign((float)(std::floor(std::fabs(v)/unit)*unit),v);
}
int main() try {
    setvbuf(stdout,nullptr,_IONBF,0);
    SetEnvironmentVariableA("XSX_DEV_NATIVE_GFX","1");
    SetEnvironmentVariableA("XSX_DEV_NOOP_PSO",nullptr); // No fallback may satisfy slot57 checks.
    // Resolve the private cache relative to this executable.
    char configured[1024]={};
    GetModuleFileNameA(nullptr,configured,sizeof configured);
    char* slash=strrchr(configured,'\\');check(slash!=nullptr,"probe directory");
    std::strcpy(slash+1,"shader-cache");SetEnvironmentVariableA("XSX_DEV_SHADER_CACHE",configured);
    HMODULE module=LoadLibraryA("d3d12_xs.dll");check(module!=nullptr,"load candidate DLL");
    FARPROC symbol=GetProcAddress(module,"D3D12XboxCreateDevice");Create create=nullptr;
    std::memcpy(&create,&symbol,sizeof create);check(create!=nullptr,"create export");
    Ref<ID3D12Device> dev;HR(create(nullptr,nullptr,__uuidof(ID3D12Device),(void**)&dev.p));check(dev.p!=nullptr,"device output");
    auto dv=*(void***)dev.p;const auto commit=(Commit)dv[76];
    Ref<ID3D12PipelineState> pipelines[4];
    for (unsigned i=0;i<4;++i) {
        // Keep slot57 as the first child creation: a heap/allocator/resource
        // warmup would hide an uninitialized PSO wrapper table.
        std::printf("[BRIDGE STAGE] slot57 pair=%u first_child=%u fallback=0\n",i+1,i==0);
        // Construct raw captured offsets independently of the runtime parser:
        // q0 opaque, then address/length pairs. A high q0 catches treating it
        // as the state byte count, as the old synthetic fixture did.
        uint64_t description[13]={0x5af135aacc731139};
        for (unsigned p=0;p<6;++p) { description[1+2*p]=(uint64_t)fixtures[i][p].data;description[2+2*p]=fixtures[i][p].size; }
        struct { UINT64 before;void* out;UINT64 after; } guarded={0x5af135aacc731139,nullptr,0x5af135aacc731139};
        HR(((NativePso)dv[57])(dev.p,description,__uuidof(ID3D12PipelineState),&guarded.out));
        check(guarded.out && guarded.before==guarded.after && guarded.before==0x5af135aacc731139,"slot57 output and guards");
        checkComMethods(guarded.out);
        pipelines[i].p=(ID3D12PipelineState*)guarded.out;
        Ref<ID3D12PipelineState> same;HR(pipelines[i]->QueryInterface(IID_PPV_ARGS(&same.p)));check(same.p==pipelines[i].p,"PSO QI identity");
        check(pipelines[i]->AddRef()>=2 && pipelines[i]->Release()>=1,"PSO AddRef/Release");
        std::printf("[BRIDGE PSO] pair=%u output=nonnull identity=1\n",i+1);
    }
    for (unsigned test=0;test<5;++test) {
        uint64_t description[13]={0x5af135aacc731139};
        for (unsigned p=0;p<6;++p) { description[1+2*p]=(uint64_t)fixtures[1][p].data;description[2+2*p]=fixtures[1][p].size; }
        if (test==0) description[2]=0xd0; // No declared blend word.
        if (test==1) description[1]=1; // Invalid state address.
        if (test==2) description[4]=0x4f; // Layout count not present.
        if (test==3) description[6]=23; // Incomplete shader hash prefix.
        if (test==4) description[9]=1; // Invalid VS address.
        struct { UINT64 before;void* out;UINT64 after; } guarded={0x5af135aacc731139,(void*)1,0x5af135aacc731139};
        const HRESULT rejected=((NativePso)dv[57])(dev.p,description,__uuidof(ID3D12PipelineState),&guarded.out);
        check(rejected==E_NOTIMPL && !guarded.out && guarded.before==guarded.after &&
            guarded.before==0x5af135aacc731139,"invalid native span rejected with null guarded output");
    }
    SetEnvironmentVariableA("XSX_DEV_NOOP_PSO","1"); // Suppress native guest root writes; bridge supplies its own root.
    D3D12_HEAP_PROPERTIES hp={};hp.Type=D3D12_HEAP_TYPE_DEFAULT;
    auto image=buffer(W);image.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;image.Height=H;
    image.Format=DXGI_FORMAT_R8G8B8A8_UNORM;image.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;image.Layout=D3D12_TEXTURE_LAYOUT_UNKNOWN;
    Ref<ID3D12Resource> target,readback,upload;
    HR(commit(dev.p,&hp,D3D12_HEAP_FLAG_NONE,&image,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&target.p)));
    hp.Type=D3D12_HEAP_TYPE_READBACK;auto rd=buffer(ReadSize);
    HR(commit(dev.p,&hp,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback.p)));
    hp.Type=D3D12_HEAP_TYPE_UPLOAD;
    HR(commit(dev.p,&hp,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&upload.p)));
    void* mapped=nullptr;HR(upload->Map(0,nullptr,&mapped));memset(mapped,0xcd,ReadSize);upload->Unmap(0,nullptr);
    struct { D3D12_DESCRIPTOR_HEAP_DESC desc;UINT64 ext[2]; } hd={};hd.desc.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;hd.desc.NumDescriptors=1;
    Ref<ID3D12DescriptorHeap> heap;HR(dev->CreateDescriptorHeap(&hd.desc,IID_PPV_ARGS(&heap.p)));
    auto hv=*(void***)heap.p;D3D12_CPU_DESCRIPTOR_HANDLE rtv={((CpuStart)hv[9])(heap.p)};
    dev->CreateRenderTargetView(target.p,nullptr,rtv);
    Ref<ID3D12CommandQueue> queue;D3D12_COMMAND_QUEUE_DESC q={};HR(dev->CreateCommandQueue(&q,IID_PPV_ARGS(&queue.p)));
    Ref<ID3D12Fence> fence;HR(dev->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence.p)));
    HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);check(event!=nullptr,"completion event");UINT64 completion=0;
    Ref<ID3D12CommandAllocator> allocator;HR(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator.p)));
    Ref<ID3D12GraphicsCommandList> list;const UINT desc[16]={};
    HR(((CreateList)dv[73])(dev.p,desc,allocator.p,nullptr,IID_PPV_ARGS(&list.p)));
    auto lv=*(void***)list.p;
    alignas(16) float tint[16]={};alignas(32) uint32_t table[16]={};
    const uint64_t address=(uint64_t)tint;
    table[0]=(uint32_t)address;table[1]=(uint32_t)(address>>32)|(16u<<16);table[2]=4;
    const float tintA[4]={.75f,.5f,1.25f,.625f},tintB[4]={.25f,1.f,.5f,1.f};
    Vertex vertices[6]={};const float positions[4][2]={{-1,1},{1,1},{-1,-1},{1,-1}};const uint16_t order[6]={0,1,2,2,1,3};
    unsigned errors=0,guardErrors=0;
    auto populate=[&] {
        for (unsigned i=0;i<6;++i) { vertices[i]={};memcpy(vertices[i].position,positions[order[i]],8);
            const unsigned char color[4]={128,64,192,128};memcpy(vertices[i].color,color,4); }
    };
    for (unsigned test=0;test<6;++test) {
        const bool replay=test==1,indexed=test==2,valid=test<3;
        if (!replay) {
            if (test) { HR(allocator->Reset());HR(list->Reset(allocator.p,nullptr)); }
            populate();
            table[2]=test==3?0:4;
            list->CopyBufferRegion(readback.p,0,upload.p,0,ReadSize);
            transition(list.p,target.p,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_RENDER_TARGET);
            ((Clear)lv[52])(list.p,rtv,background,0,nullptr);
            D3D12_CPU_DESCRIPTOR_HANDLE bindTarget=test==4?D3D12_CPU_DESCRIPTOR_HANDLE{0x12345678}:rtv;
            ((SetRT)lv[50])(list.p,1,&bindTarget,FALSE,nullptr);
            list->SetPipelineState(pipelines[1].p);
            ((Table)lv[36])(list.p,0,(uint64_t)table);
            if (test!=5) {
                D3D12_VERTEX_BUFFER_VIEW vb={(uint64_t)vertices,sizeof vertices,sizeof vertices[0]};
                ((SetVB)lv[48])(list.p,0,1,&vb);memset(&vb,0xa7,sizeof vb);
            }
            if (indexed) {
                // The six vertices are already a triangle list; use sequential indices.
                static uint16_t indices[6]={0,1,2,3,4,5};
                D3D12_INDEX_BUFFER_VIEW ib={(uint64_t)indices,sizeof indices,DXGI_FORMAT_R16_UINT};
                ((SetIB)lv[47])(list.p,&ib);memset(&ib,0xa7,sizeof ib);
            }
            for (unsigned half=0;half<2;++half) {
                memcpy(tint+8,half?tintB:tintA,16);
                D3D12_VIEWPORT vp={(float)(half*4),0,4,8,0,1};D3D12_RECT scissor={(LONG)half*4,0,(LONG)(half+1)*4,8};
                list->RSSetViewports(1,&vp);list->RSSetScissorRects(1,&scissor);
                if (indexed) list->DrawIndexedInstanced(6,1,0,0,0);else list->DrawInstanced(6,1,0,0);
            }
            // Neither mutable guest bytes nor poisoned setter structs may be reread on submission.
            memset(tint,0,sizeof tint);memset(vertices,0,sizeof vertices);
            transition(list.p,target.p,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION source={},destination={};source.pResource=target.p;source.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            destination.pResource=readback.p;destination.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            destination.PlacedFootprint={Offset,{DXGI_FORMAT_R8G8B8A8_UNORM,W,H,1,Pitch}};
            list->CopyTextureRegion(&destination,0,0,0,&source,nullptr);
            transition(list.p,target.p,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);HR(list->Close());
        }
        ID3D12CommandList* lists[]={list.p};queue->ExecuteCommandLists(1,lists);
        HR(queue->Signal(fence.p,++completion));HR(fence->SetEventOnCompletion(completion,event));
        check(WaitForSingleObject(event,10000)==WAIT_OBJECT_0,"GPU completion timeout");
        check(fence->GetCompletedValue()!=UINT64_MAX,"device alive");
        HR(readback->Map(0,nullptr,&mapped));const auto* bytes=(const unsigned char*)mapped;
        unsigned caseErrors=0,caseGuards=0,maxError=0;
        for (UINT i=0;i<ReadSize;++i) {
            const bool pixel=i>=Offset && i<Offset+H*Pitch && (i-Offset)%Pitch<W*4;
            if (!pixel) { if (bytes[i]!=0xcd) ++caseGuards;continue; }
            const UINT x=((i-Offset)%Pitch)/4,c=(i-Offset)%4;
            float expected=background[c];
            if (valid) {
                const float packed[4]={128/255.f,64/255.f,192/255.f,128/255.f};
                const float* t=x<4?tintA:tintB;
                const float component=halfRtz(packed[c]*t[c]),alpha=halfRtz(packed[3]*t[3]);
                expected=component*alpha+background[c]*(1-alpha);
            }
            const unsigned want=(unsigned)std::lround(std::clamp(expected,0.f,1.f)*255.f);
            const unsigned error=(unsigned)std::abs((int)bytes[i]-(int)want);maxError=std::max(maxError,error);
            if (error>1) ++caseErrors;
        }
        readback->Unmap(0,nullptr);errors+=caseErrors;guardErrors+=caseGuards;
        std::printf("[BRIDGE GPU] case=%u replay=%u indexed=%u expected_draw=%u pixels=64 errors=%u max_error=%u guard_errors=%u\n",test,replay,indexed,valid,caseErrors,maxError,caseGuards);
        check(!caseErrors && !caseGuards,"pixel oracle and guards");
    }
    HR(allocator->Reset());HR(list->Reset(allocator.p,nullptr));HR(list->Close());
    CloseHandle(event);
    std::printf("[PASS] native UI bridge checks=%u pipelines=4 cases=6 pixels=384 errors=%u guard_errors=%u scope=slot57-packed-vertices-Vsharp-fence-lifetime\n",checks,errors,guardErrors);
    return 0;
} catch (const std::exception& error) { std::printf("[FAIL] native UI bridge checks=%u stage=%s\n",checks,error.what());return 1; }
