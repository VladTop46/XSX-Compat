// Exercises the actual phys.h MapPages/UnmapRange with observed host-view
// boundaries. Observe after host unmap, before the caller can restore tails.
// This tests mapping transactions, not native Windows execution/concurrency.
#ifndef XSX_PHYS_HEADER
#define XSX_PHYS_HEADER "../runtime/xmem/phys.h"
#endif
#include XSX_PHYS_HEADER
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
namespace model {
struct Mapping { size_t size;uint64_t offset; };
static std::map<uintptr_t,Mapping> views;
static std::vector<uintptr_t> retained;
static unsigned gaps=0,checks=0;
static void check(bool ok,const char* what) { ++checks;if (!ok) { std::fprintf(stderr,"[FAIL] %s\n",what);std::exit(1); } }
static void log(const char*,...) {}
static PVOID va2(HANDLE,PVOID address,SIZE_T,ULONG,ULONG,void*,ULONG) { return address; }
static PVOID map(HANDLE,HANDLE,PVOID address,ULONG64 offset,SIZE_T size,ULONG,ULONG,void*,ULONG) {
    const auto base=(uintptr_t)address;
    for (const auto& entry:views) if (base<entry.first+entry.second.size && entry.first<base+size) return nullptr;
    views[base]={size,offset};return address;
}
static BOOL unmap(HANDLE,PVOID address,ULONG) {
    const auto base=(uintptr_t)address;auto it=views.find(base);if (it==views.end()) return 0;
    // UnmapViewOfFile2 removes the complete host view. Any retained neighbor
    // inside that view is inaccessible until a later MapViewOfFile3 call.
    for (auto page:retained) if (page>=base && page<base+it->second.size) ++gaps;
    views.erase(it);return 1;
}
static uintptr_t id(uintptr_t address) {
    auto it=views.upper_bound(address);if (it==views.begin()) return 0;--it;
    if (address>=it->first+it->second.size) return 0;
    return phys::kFirstId+(it->second.offset+(address-it->first))/phys::kPage*phys::kIdStep;
}
}
DWORD GetEnvironmentVariableA(const char*,char*,DWORD) { return 0; }
HMODULE LoadLibraryW(const wchar_t*) { return nullptr; }
void* GetProcAddress(HMODULE,const char*) { return nullptr; }
HANDLE CreateFileMappingW(HANDLE,void*,DWORD,DWORD,DWORD,const wchar_t*) { return nullptr; }
DWORD GetLastError() { return 0; }
BOOL VirtualFree(PVOID,SIZE_T,DWORD) { return 1; }
HANDLE GetCurrentProcess() { return (HANDLE)1; }
int main(int argc,char** argv) {
    using namespace model;const bool expectGap=argc==2 && !std::strcmp(argv[1],"--expect-retained-gap");
    constexpr uintptr_t A=0x100000000ull,B=0x200000000ull;const size_t P=phys::kPage;
    phys::g.ok=true;phys::g.va2=&va2;phys::g.mv3=&map;phys::g.uv2=&unmap;
    phys::g.regions[A]=64*P;phys::g.regions[B]=64*P;
    std::vector<std::pair<uintptr_t,uintptr_t>> pages;
    for (unsigned i=0;i<8;++i) pages.push_back({A+i*P,phys::kFirstId+i*phys::kIdStep});
    std::vector<size_t> fallback;check(phys::MapPages(pages,fallback,log)==8 && fallback.empty(),"initial eight pages");
    // Alias a page independently. A replacement must not alter this mapping.
    check(phys::MapPages({{B,phys::kFirstId+3*phys::kIdStep}},fallback,log)==1,"alias page3");
    for (unsigned i=0;i<8;++i) if (i!=3) retained.push_back(A+i*P);
    check(phys::MapPages({{A+3*P,phys::kFirstId+7*phys::kIdStep}},fallback,log)==1,"replace middle page");
    for (unsigned i=0;i<8;++i) check(id(A+i*P)==phys::kFirstId+(i==3?7:i)*phys::kIdStep,"final mapping identity");
    check(id(B)==phys::kFirstId+3*phys::kIdStep,"other alias unchanged");
    check(phys::UnmapPages(A+3*P,1,log),"unmap middle page");check(!id(A+3*P),"target becomes reserve");
    for (unsigned i=0;i<8;++i) if (i!=3) check(id(A+i*P)==phys::kFirstId+i*phys::kIdStep,"unmap retained identity");
    retained={A,A+P,A+6*P,A+7*P};
    pages.clear();for (unsigned i=2;i<6;++i) pages.push_back({A+i*P,phys::kFirstId+(i+16)*phys::kIdStep});
    check(phys::MapPages(pages,fallback,log)==4,"replace interior run");
    for (auto page:retained) check(id(page)==phys::kFirstId+(page-A)/P*phys::kIdStep,"interior retained identity");
    // Allocation granularity must stay64KiB, even for a large initial run.
    retained.clear();pages.clear();for (unsigned i=0;i<64;++i) pages.push_back({B+i*P,phys::kFirstId+(i+64)*phys::kIdStep});
    check(phys::MapPages(pages,fallback,log)==64,"large run");
    for (unsigned i=0;i<64;++i) if (i!=20) retained.push_back(B+i*P);
    check(phys::UnmapPages(B+20*P,1,log),"partial unmap of large run");
    for (auto page:retained) check(id(page)==phys::kFirstId+((page-B)/P+64)*phys::kIdStep,"large retained identity");
    if (expectGap) { check(gaps>0,"baseline must expose the retained-page gap");std::printf("[REPRO] old phys mapping retained_page_gaps=%u final_aliases_intact=1\n",gaps); }
    else { check(!gaps,"retained pages must stay mapped during every host unmap");
        for (auto& entry:phys::g.views) check(entry.second.size==P,"host view matches one Xbox page");
        std::printf("[PASS] physical mapping model checks=%u retained_page_gaps=%u page_bytes=65536\n",checks,gaps); }
}
