#define NOMINMAX
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <thread>
using VirtualAllocFn=void*(*)(void*,size_t,uint32_t,uint64_t,uint32_t);
using AllocateFn=BOOL(*)(uint32_t,size_t*,uintptr_t*);
using MapFn=void*(*)(void*,size_t,const uintptr_t*);
using ScatterFn=BOOL(*)(void**,size_t,const uintptr_t*);
using FreeFn=BOOL(*)(size_t,const uintptr_t*);
static unsigned checks=0;
static void check(bool ok,const char* why) {
    ++checks;if (!ok) { std::printf("[FAIL] XMEM NEIGHBOR %s error=%lu\n",why,GetLastError());throw std::runtime_error(why); }
}
template<class T> static T proc(HMODULE module,const char* name) {
    FARPROC symbol=GetProcAddress(module,name);T result=nullptr;static_assert(sizeof result==sizeof symbol);
    std::memcpy(&result,&symbol,sizeof result);check(result!=nullptr,name);return result;
}
struct Writer {
    std::atomic<bool> stop{false};HANDLE started=nullptr;std::thread worker;
    Writer(volatile LONG* left,volatile LONG* right) {
        started=CreateEventW(nullptr,TRUE,FALSE,nullptr);check(started!=nullptr,"writer start event");
        worker=std::thread([this,left,right] {
            InterlockedIncrement(left);InterlockedIncrement(right);SetEvent(started);
            while (!stop.load(std::memory_order_acquire)) { InterlockedIncrement(left);InterlockedIncrement(right); }
        });
    }
    void finish() { stop.store(true,std::memory_order_release);if (worker.joinable()) worker.join(); }
    ~Writer() { finish();if (started) CloseHandle(started); }
};
int main() try {
    setvbuf(stdout,nullptr,_IONBF,0);SetEnvironmentVariableA("XSX_XMEM_ALIAS","1");
    char nv[4]={};const bool native=GetEnvironmentVariableA("XSX_NATIVE_GFX",nv,sizeof nv)>0 && nv[0]=='1';
    HMODULE module=LoadLibraryA("xmem.dll");check(module!=nullptr,"load candidate xmem.dll");
    const auto allocate=proc<VirtualAllocFn>(module,"XMemVirtualAlloc");
    const auto idsAlloc=proc<AllocateFn>(module,"XMemAllocatePhysicalPages");
    const auto map=proc<MapFn>(module,"XMemMapPhysicalPages");
    const auto scatter=proc<ScatterFn>(module,"XMemMapPhysicalPagesScatter");
    const auto idsFree=proc<FreeFn>(module,"XMemFreePhysicalPages");
    constexpr size_t P=0x10000,N=8;
    auto* A=(unsigned char*)allocate(nullptr,N*P,0x20402000,0x21,PAGE_READWRITE);
    auto* B=(unsigned char*)allocate(nullptr,N*P,0x20402000,0x21,PAGE_READWRITE);
    check(A && B,"two physical reserves");
    uintptr_t ids[N]={};size_t count=N;check(idsAlloc(0x380,&count,ids) && count==N,"allocate eight physical ids");
    check(map(A,N,ids)==A && map(B,N,ids)==B,"map both alias ranges");
    for (size_t i=0;i<N;++i) {
        MEMORY_BASIC_INFORMATION info={};
        // XSX_NATIVE_GFX=1: pages are amdgpu BO overlays invisible to Wine's VirtualQuery;
        // check real accessibility of the whole page instead of the section-view shape.
        if (native) check(!IsBadWritePtr(A+i*P,P),"native BO page fully accessible");
        else check(VirtualQuery(A+i*P,&info,sizeof info)==sizeof info && info.State==MEM_COMMIT &&
            info.Type==MEM_MAPPED && info.AllocationBase==A+i*P && info.RegionSize==P,
            "one native section view per64KiB Xbox page");
        A[i*P+0x100]=(unsigned char)(0x50+i);
        check(B[i*P+0x100]==0x50+i,"real section alias identity");
    }
    constexpr size_t O=0x3000;
    auto* left=(volatile LONG*)(A+O);auto* right=(volatile LONG*)(A+7*P+O);
    auto* leftAlias=(volatile LONG*)(B+O);auto* rightAlias=(volatile LONG*)(B+7*P+O);
    *left=*right=0;
    const uint32_t guard=0x9e731ac5;
    for (auto* p:{A+O,A+7*P+O}) { std::memcpy(p-4,&guard,4);std::memcpy(p+4,&guard,4); }
    {
        Writer writer(left,right);
        check(WaitForSingleObject(writer.started,5000)==WAIT_OBJECT_0,"neighbor writer running");
        for (unsigned iteration=0;iteration<2000;++iteration) {
            const unsigned target=2+iteration%4;void* page=A+target*P;uintptr_t unmap=0;
            check(scatter(&page,1,&unmap)!=FALSE,"unmap target only");
            const uintptr_t alias=ids[(target+1)%N];
            check(map(page,1,&alias)==page,"remap target to another physical id");
            check(*((unsigned char*)page+0x100)==0x50+(target+1)%N &&
                *((unsigned char*)page+0x100)==B[((target+1)%N)*P+0x100],"remapped page aliases distinct source pattern");
            if (!(iteration%100)) {
                MEMORY_BASIC_INFORMATION l={},r={};
                if (native) check(!IsBadReadPtr((void*)left,4) && !IsBadReadPtr((void*)right,4),"retained neighbors remain accessible");
                else check(VirtualQuery((void*)left,&l,sizeof l)==sizeof l && VirtualQuery((void*)right,&r,sizeof r)==sizeof r &&
                    l.State==MEM_COMMIT && r.State==MEM_COMMIT,"retained neighbors remain committed");
            }
        }
        writer.finish();
    }
    const LONG l=InterlockedCompareExchange(left,0,0),r=InterlockedCompareExchange(right,0,0);
    check(l>0 && r>0,"neighbor writers made progress");
    check(InterlockedCompareExchange(leftAlias,0,0)==l && InterlockedCompareExchange(rightAlias,0,0)==r,"writer bytes visible through independent aliases");
    for (auto* p:{A+O,A+7*P+O}) {
        uint32_t before=0,after=0;std::memcpy(&before,p-4,4);std::memcpy(&after,p+4,4);
        check(before==guard && after==guard,"adjacent DWORD guards intact");
    }
    check(map(A,N,nullptr)==A && map(B,N,nullptr)==B,"unmap both reserves");
    check(idsFree(N,ids)!=FALSE,"release physical ids");
    std::printf("[PASS] XMEM NEIGHBOR checks=%u remaps=2000 writer_left=%ld writer_right=%ld alias_identity=1 guards=1 native_page_bytes=65536\n",checks,l,r);
    return 0;
} catch (const std::exception& error) { std::printf("[FAIL] XMEM NEIGHBOR checks=%u stage=%s\n",checks,error.what());return 1; }
