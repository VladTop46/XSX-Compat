// xmem.dll — прототип Xbox XMem на Wine. Сигнатуры восстановлены по callsite'ам (публичного xmem.h в checkout нет).
// Реализованы: Alloc/Free(+Default), VirtualAlloc (reserve/commit без PHYSICAL/LARGE_PAGES), VirtualQuery.
// Остальные экспорты логируют аргументы и возвращают отказ (NULL/0) — НЕ эмулируют семантику Xbox.
#include <windows.h>
#include <malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <stddef.h>
#include <stdarg.h>
#include "phys.h"

static constexpr size_t ALIGNMENT = 256;
#define XLOG(...) do { fprintf(stderr, "[XMem] " __VA_ARGS__); fputc('\n', stderr); fflush(stderr); } while (0)

static void PLog(const char* fmt, ...) { va_list a; va_start(a, fmt); fprintf(stderr, "[XMem] "); vfprintf(stderr, fmt, a); fputc('\n', stderr); fflush(stderr); va_end(a); }

extern "C" {

__declspec(dllexport) void* XMemAlloc(size_t size, uint64_t attributes)
{
    if (size == 0) size = 1;
    void* ptr = _aligned_malloc(size, ALIGNMENT);
    XLOG("Alloc size=%zu attrs=0x%llx -> %p", size, (unsigned long long)attributes, ptr);
    return ptr;
}
__declspec(dllexport) void XMemFree(void* ptr, uint64_t attributes)
{
    XLOG("Free ptr=%p attrs=0x%llx", ptr, (unsigned long long)attributes);
    if (ptr) _aligned_free(ptr);
}
// Сигнатуры Default-вариантов НЕ подтверждены; ведут себя как Alloc/Free.
__declspec(dllexport) void* XMemAllocDefault(size_t size, uint64_t attributes) { return XMemAlloc(size, attributes); }
__declspec(dllexport) void XMemFreeDefault(void* ptr, uint64_t attributes) { XMemFree(ptr, attributes); }

__declspec(dllexport) int XMemGetWorkingSetStatistics(uint32_t selector, void* output)
{
    XLOG("GetWorkingSetStatistics selector=0x%08x output=%p (unsupported)", selector, output);
    return 0;
}

// Callsite игры (RVA 0x1e1d002): XMemVirtualAlloc(0, size, 0x20402000 [RESERVE|PHYSICAL|LARGE_PAGES], 0x21, PAGE_READWRITE).
// На PC: PHYSICAL/LARGE_PAGES недоступны -> обычный VirtualAlloc. Атрибуты (0x21) игнорируются.
__declspec(dllexport) void* XMemVirtualAlloc(void* addr, size_t size, uint32_t type, uint64_t attrs, uint32_t protect)
{
    uint32_t t = type & ~(0x00400000u /*MEM_PHYSICAL*/ | 0x20000000u /*MEM_LARGE_PAGES*/);
    uint32_t prot = protect & 0xFF;                 // старшие биты PAGE_* (0x2000, 0x20000…) — Xbox-специфика; на PC отбрасываем
    if (!prot) prot = PAGE_READWRITE;
    void* p;
    if ((type & 0x00400000u) && (type & MEM_RESERVE) && !(type & (MEM_COMMIT | 0x40000u | 0x4000u)) && phys::Init(PLog)) {
        p = phys::ReservePlaceholder(addr, size);
        XLOG("VirtualAlloc addr=%p size=0x%zx type=0x%x attrs=0x%llx prot=0x%x -> %p PHYSICAL-резерв как placeholder (err=%lu)", addr, size, type, (unsigned long long)attrs, protect, p, p ? 0UL : GetLastError());
        if (p) return p;
    }
    if (t & (0x40000 | 0x4000)) {   // MEM_RESERVE_PLACEHOLDER | MEM_REPLACE_PLACEHOLDER: нужен VirtualAlloc2 (Win10 placeholder API; в Wine поддержан — placeholder-probe.exe)
        typedef PVOID (WINAPI* VA2)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, void*, ULONG);
        static VA2 va2 = (VA2)GetProcAddress(LoadLibraryW(L"kernelbase.dll"), "VirtualAlloc2");
        // REPLACE_PLACEHOLDER (регион под GPU-пул): коммитим целиком — игра создаёт ресурсы на всём диапазоне до/между Map-вызовами (память не трогается до использования)
        uint32_t tt = (t & 0x4000) ? (t | MEM_COMMIT) : t; if ((t & 0x4000) && (prot & (PAGE_NOACCESS))) prot = PAGE_READWRITE;
        p = va2 ? va2(nullptr, addr, size, tt, prot, nullptr, 0) : VirtualAlloc(addr, size, t & ~(0x40000u | 0x4000u), prot);
    } else p = VirtualAlloc(addr, size, t, prot);
    XLOG("VirtualAlloc addr=%p size=0x%zx type=0x%x(->0x%x) attrs=0x%llx prot=0x%x -> %p (err=%lu)",
         addr, size, type, t, (unsigned long long)attrs, protect, p, p ? 0UL : GetLastError());
    return p;
}
__declspec(dllexport) SIZE_T XMemVirtualQuery(const void* addr, MEMORY_BASIC_INFORMATION* info, SIZE_T len)
{
    SIZE_T r = VirtualQuery(addr, info, len);
    XLOG("VirtualQuery addr=%p len=%zu -> %zu", addr, len, r);
    return r;
}


// ---- Физические страницы Xbox (64 КиБ). Сигнатуры по callsite'ам игры (аллокатор GPU-памяти, RVA 0x1e6c1a0):
//   BOOL XMemAllocatePhysicalPages(uint32 attrs, SIZE_T* inOutPages, ULONG_PTR* pageArray)   (eax=1 и *pages == запрошенному)
//   void* XMemMapPhysicalPages(void* va, SIZE_T pages, const ULONG_PTR* pageArray)            (адрес или NULL)
//   BOOL  XMemFreePhysicalPages(SIZE_T pages, const ULONG_PTR* pageArray)
// Эмуляция: id страниц — счётчик; Map коммитит [va, va+pages*64K) в ранее зарезервированном VA. Алиасинг страницы НЕ поддерживается.
using phys::kPage;
static volatile LONG64 g_nextPage = 0x1000, g_liveIds = 0;
__declspec(dllexport) BOOL XMemAllocatePhysicalPages(uint32_t attrs, size_t* pages, uintptr_t* array)
{
    if (!pages || !array) return FALSE;
    size_t n = *pages; for (size_t i = 0; i < n; i++) array[i] = (uintptr_t)InterlockedIncrement64(&g_nextPage);
    InterlockedAdd64(&g_liveIds, (LONG64)n);
    XLOG("AllocatePhysicalPages attrs=0x%x pages=%zu (%zu MiB) -> ids %#llx..%#llx", attrs, n, n * kPage >> 20, n ? (unsigned long long)array[0] : 0ULL, n ? (unsigned long long)array[n - 1] : 0ULL);
    return TRUE;
}
__declspec(dllexport) void* XMemMapPhysicalPages(void* va, size_t pages, const uintptr_t* array)
{
    if (!va || !pages) { XLOG("MapPhysicalPages va=%p pages=%zu -> NULL (bad args)", va, pages); return nullptr; }
    if (phys::g.ok && !array) { bool ok = phys::UnmapPages((uintptr_t)va, pages, PLog); XLOG("MapPhysicalPages va=%p pages=%zu array=NULL -> unmap %s", va, pages, ok ? "ok" : "FAIL"); return va; }
    size_t viaSection = 0; std::vector<size_t> fb;
    if (phys::g.ok && array) {
        std::vector<std::pair<uintptr_t, uintptr_t>> pg(pages); for (size_t i = 0; i < pages; i++) pg[i] = {(uintptr_t)va + i * kPage, array[i]};
        viaSection = phys::MapPages(pg, fb, PLog);
    } else { fb.resize(pages); for (size_t i = 0; i < pages; i++) fb[i] = i; }
    size_t okFb = 0; for (size_t i : fb) if (VirtualAlloc((char*)va + i * kPage, kPage, MEM_COMMIT, PAGE_READWRITE)) okFb++;
    XLOG("MapPhysicalPages va=%p pages=%zu (%zu MiB) first=%#llx -> section=%zu private=%zu/%zu views=%llu", va, pages, pages * kPage >> 20, array ? (unsigned long long)array[0] : 0ULL, viaSection, okFb, fb.size(), (unsigned long long)phys::g.viewCount);
    return (viaSection + okFb == pages) ? va : nullptr;
}
__declspec(dllexport) BOOL XMemFreePhysicalPages(size_t pages, const uintptr_t* array)
{
    InterlockedAdd64(&g_liveIds, -(LONG64)pages);
    XLOG("FreePhysicalPages pages=%zu (ids released; VA commit not reverted)", pages);
    return TRUE;
}


// XMemMapPhysicalPagesScatter(void** addrs, SIZE_T count, const ULONG_PTR* pageIds) -> BOOL [по callsite 0x140f2cc3e: rcx=массив VA 64К-страниц, edx=число, r8=массив id].
__declspec(dllexport) BOOL XMemMapPhysicalPagesScatter(void** addrs, size_t count, const uintptr_t* ids)
{
    size_t viaSection = 0, unm = 0; std::vector<size_t> fb;
    if (phys::g.ok) {
        std::vector<std::pair<uintptr_t, uintptr_t>> pg; pg.reserve(count); std::vector<size_t> idx;
        for (size_t i = 0; i < count; i++) {
            if (!ids || !ids[i]) { phys::UnmapPages((uintptr_t)addrs[i], 1, PLog); unm++; continue; }
            pg.push_back({(uintptr_t)addrs[i], ids[i]}); idx.push_back(i);
        }
        std::vector<size_t> fbl; viaSection = phys::MapPages(pg, fbl, PLog); for (size_t k : fbl) fb.push_back(idx[k]);
    } else { for (size_t i = 0; i < count; i++) fb.push_back(i); }
    size_t okFb = 0; for (size_t i : fb) if (VirtualAlloc(addrs[i], kPage, MEM_COMMIT, PAGE_READWRITE)) okFb++;
    static int n = 0;
    if (n++ < 16) { uintptr_t mn = ~0ull, mx = 0; for (size_t i = 0; ids && i < count; i++) { mn = ids[i] < mn ? ids[i] : mn; mx = ids[i] > mx ? ids[i] : mx; }
        XLOG("MapPhysicalPagesScatter count=%zu first=%p ids=[%#llx..%#llx] -> section=%zu private=%zu/%zu unmapped=%zu views=%llu", count, count ? addrs[0] : nullptr, (unsigned long long)mn, (unsigned long long)mx, viaSection, okFb, fb.size(), unm, (unsigned long long)phys::g.viewCount); }
    return viaSection + okFb + unm == count;
}

#define XSTUB(name) \
    __declspec(dllexport) uintptr_t name(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e, uintptr_t f) \
    { XLOG(#name " UNIMPLEMENTED args=(%p,%p,%p,%p,%p,%p) -> 0", (void*)a, (void*)b, (void*)c, (void*)d, (void*)e, (void*)f); return 0; }
XSTUB(XMemIsToolsMemAvailable)
XSTUB(XMemTransferMemory)

}
