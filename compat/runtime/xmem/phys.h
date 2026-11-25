// Эмуляция физических страниц Xbox (64 КиБ) с алиасингом.
// Xbox: id физической страницы можно отобразить в несколько VA (XMemMapPhysicalPages / ...Scatter) — это ОДНА память.
// Эмуляция: id -> смещение в общей pagefile-секции (разреженной); VA-резервы с MEM_PHYSICAL создаются как placeholder'ы
// (VirtualAlloc2 MEM_RESERVE_PLACEHOLDER), отображение = расщепление placeholder'а + MapViewOfFile3(MEM_REPLACE_PLACEHOLDER).
// VA вне наших placeholder-резервов (например, регионы REPLACE_PLACEHOLDER, которые мы коммитим) обслуживаются по-старому
// (коммит частной памяти, без алиасинга) — это логируется.
// Отключение: XSX_XMEM_ALIAS=0.
#pragma once
#include <windows.h>
#include <stdint.h>
#include <map>
#include <mutex>
#include <vector>

namespace phys {

static constexpr size_t kPage = 0x10000;
// id физической страницы = номер в 4-КиБ единицах (как PFN Xbox), кратен 16 для страницы 64 КиБ. Игра хранит id в упакованной таблице
// (21 бит, id >> 4) и восстанавливает как entry << 4 (exe 0x140f2cbc1) — счётчик подряд (0x1001, 0x1002…) там схлопывался по 16.
static constexpr uintptr_t kFirstId = 0x100000;
static constexpr uintptr_t kIdStep = 16;
static inline uint64_t IdOff(uintptr_t id) { return (uint64_t)((id - kFirstId) / kIdStep) * kPage; }
static constexpr uint64_t kSectionSize = 16ull << 30;    // 16 ГиБ (разреженно; память расходуется только при записи)

typedef PVOID (WINAPI* VirtualAlloc2Fn)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, void*, ULONG);
typedef PVOID (WINAPI* MapViewOfFile3Fn)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG, void*, ULONG);
typedef BOOL (WINAPI* UnmapViewOfFile2Fn)(HANDLE, PVOID, ULONG);
#ifndef MEM_RESERVE_PLACEHOLDER
#define MEM_RESERVE_PLACEHOLDER 0x00040000
#endif
#ifndef MEM_REPLACE_PLACEHOLDER
#define MEM_REPLACE_PLACEHOLDER 0x00004000
#endif
#ifndef MEM_PRESERVE_PLACEHOLDER
#define MEM_PRESERVE_PLACEHOLDER 0x00000002
#endif

struct View { size_t size; uintptr_t id0; }; // New views cover exactly one64-KiB Xbox page.
struct State {
    std::mutex m;
    bool init = false, ok = false;
    HANDLE section = nullptr;
    VirtualAlloc2Fn va2 = nullptr; MapViewOfFile3Fn mv3 = nullptr; UnmapViewOfFile2Fn uv2 = nullptr;
    std::map<uintptr_t, size_t> regions;                   // наши placeholder-резервы: base -> size
    std::map<uintptr_t, View> views;                       // base -> view
    std::map<uintptr_t, size_t> priv;                      // частные 64К-страницы внутри наших резервов (нулевой тайл и т.п.)
    uint64_t mapCalls = 0, viewCount = 0, fallbacks = 0;
};
static State g;
// ---- native-gfx (XSX_NATIVE_GFX=1): physical pages = amdgpu GTT BOs via xsxgpu unixlib. ----
// The CPU view is an mmap of the BO placed over our placeholder; the GPU sees the same page at
// the same VA. Wine's own bookkeeping keeps reporting the placeholder (documented limitation).
typedef int (WINAPI* XgOpenFn)(unsigned*, unsigned*, unsigned long long*);
typedef int (WINAPI* XgMapFn)(void*, unsigned long long, const ULONG_PTR*, unsigned long long);
typedef int (WINAPI* XgUnmapFn)(void*, unsigned long long);
typedef int (WINAPI* XgWriteFn)(unsigned long long, unsigned long long, const void*, unsigned long long);
static struct { bool on = false; XgMapFn map = nullptr; XgUnmapFn unmap = nullptr; XgWriteFn write = nullptr; uintptr_t nextPrivId = 0; uint64_t maps = 0, unmaps = 0, failures = 0; } gn;
static bool NativeWanted()
{
    char v[8]; DWORD n = GetEnvironmentVariableA("XSX_NATIVE_GFX", v, sizeof v);
    return n && v[0] == '1';
}
static bool NativeInit(void (*log)(const char*, ...))
{
    HMODULE m = LoadLibraryA("xsxgpu.dll");
    if (!m) { log("phys: XSX_NATIVE_GFX=1, но xsxgpu.dll не загружена err=%lu -> секции", GetLastError()); return false; }
    auto op = (XgOpenFn)GetProcAddress(m, "xsxgpu_open");
    gn.map = (XgMapFn)GetProcAddress(m, "xsxgpu_phys_map"); gn.unmap = (XgUnmapFn)GetProcAddress(m, "xsxgpu_phys_unmap"); gn.write = (XgWriteFn)GetProcAddress(m, "xsxgpu_phys_write");
    unsigned fam = 0, dev = 0; unsigned long long vaMax = 0;
    const int r = op && gn.map && gn.unmap ? op(&fam, &dev, &vaMax) : -1;
    if (r) { log("phys: xsxgpu_open r=%d -> секции", r); return false; }
    // Private pages (XSX_XMEM_DUP_PRIVATE) take unique ids from the top of the id space.
    gn.nextPrivId = kFirstId + kIdStep * ((32ull << 30) / kPage - 1);
    gn.on = true;
    log("phys: NATIVE GPU-память: физические страницы = GTT BO amdgpu (family=%u device=%#x va_max=%#llx)", fam, dev, vaMax);
    return true;
}

static bool Enabled()
{
    char v[8]; DWORD n = GetEnvironmentVariableA("XSX_XMEM_ALIAS", v, sizeof v);
    return !(n && v[0] == '0');
}
static bool Init(void (*log)(const char*, ...))
{
    if (g.init) return g.ok;
    g.init = true;
    if (!Enabled()) { log("phys: алиасинг отключён (XSX_XMEM_ALIAS=0)"); return false; }
    const bool native = NativeWanted() && NativeInit(log);
    HMODULE kb = LoadLibraryW(L"kernelbase.dll");
    g.va2 = (VirtualAlloc2Fn)GetProcAddress(kb, "VirtualAlloc2");
    g.mv3 = (MapViewOfFile3Fn)GetProcAddress(kb, "MapViewOfFile3");
    g.uv2 = (UnmapViewOfFile2Fn)GetProcAddress(kb, "UnmapViewOfFile2");
    if (!g.va2 || !g.mv3 || !g.uv2) { log("phys: нет VirtualAlloc2/MapViewOfFile3/UnmapViewOfFile2 -> без алиасинга"); return false; }
    if (native) { g.ok = true; return true; }   // placeholders still reserved via VirtualAlloc2; no section needed
    g.section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, (DWORD)(kSectionSize >> 32), (DWORD)kSectionSize, nullptr);
    if (!g.section) { log("phys: CreateFileMapping(%llu ГиБ) err=%lu -> без алиасинга", (unsigned long long)(kSectionSize >> 30), GetLastError()); return false; }
    g.ok = true;
    log("phys: секция физических страниц %llu ГиБ создана, алиасинг включён", (unsigned long long)(kSectionSize >> 30));
    return true;
}

// Резерв VA под физические страницы как placeholder.
static void* ReservePlaceholder(void* addr, size_t size)
{
    void* p = g.va2(nullptr, addr, size, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0);
    if (p) { std::lock_guard<std::mutex> l(g.m); g.regions[(uintptr_t)p] = size; }
    return p;
}
static uintptr_t RegionBase(uintptr_t va)
{
    std::lock_guard<std::mutex> l(g.m);
    auto it = g.regions.upper_bound(va); if (it == g.regions.begin()) return 0; --it;
    return (va >= it->first && va < it->first + it->second) ? it->first : 0;
}
static bool InOurRegion(uintptr_t va, size_t size)
{
    auto it = g.regions.upper_bound(va); if (it == g.regions.begin()) return false; --it;
    return va >= it->first && va + size <= it->first + it->second;
}
static bool IdOk(uintptr_t id) { return id >= kFirstId && (id % kIdStep) == 0 && IdOff(id) + kPage <= (gn.on ? (32ull << 30) : kSectionSize); }

static bool MapOne(uintptr_t va, size_t size, uintptr_t id0)
{
    VirtualFree((void*)va, size, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);   // расщепить placeholder до [va, va+size) (ошибка = уже точный)
    void* r = g.mv3(g.section, GetCurrentProcess(), (void*)va, (ULONG64)IdOff(id0), size, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0);
    if (!r) return false;
    g.views[va] = {size, id0}; g.viewCount++; return true;
}
// Снять все view, пересекающиеся с [va, va+size); части view вне диапазона восстанавливаются (переотображаются).
static bool UnmapRange(uintptr_t va, size_t size, void (*log)(const char*, ...))
{
    if (gn.on) {   // exact range: BO overlays are per-64KiB-page mmaps, neighbours are untouched
        const int r = gn.unmap((void*)va, size / kPage); gn.unmaps++;
        if (r) { gn.failures++; log("phys: NATIVE unmap %p+%#zx r=%d", (void*)va, size, r); return false; }
        return true;
    }
    uintptr_t end = va + size;
    for (auto it = g.priv.lower_bound(va); it != g.priv.end() && it->first < end;) {
        if (!VirtualFree((void*)it->first, it->second, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) log("phys: release private %p err=%lu", (void*)it->first, GetLastError());
        it = g.priv.erase(it);
    }
    for (;;) {
        auto it = g.views.upper_bound(va);
        if (it != g.views.begin()) { auto p = std::prev(it); if (p->first + p->second.size > va) it = p; }
        if (it == g.views.end() || it->first >= end) break;
        uintptr_t vb = it->first; View v = it->second; g.views.erase(it);
        if (!g.uv2(GetCurrentProcess(), (void*)vb, MEM_PRESERVE_PLACEHOLDER)) { log("phys: UnmapViewOfFile2(%p) err=%lu", (void*)vb, GetLastError()); return false; }
        g.viewCount--;
        // восстановить левый/правый хвосты
        auto remap = [&](uintptr_t b, size_t s, uintptr_t id) {
            if (!s) return true;
            if (!g.va2) return false;
            if (s != v.size && !VirtualFree((void*)b, s, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) { /* уже точный placeholder */ }
            void* r = g.mv3(g.section, GetCurrentProcess(), (void*)b, (ULONG64)IdOff(id), s, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0);
            if (r) { g.views[b] = {s, id}; g.viewCount++; return true; }
            DWORD err = GetLastError(); size_t ok = 0, pages = s / kPage;
            for (size_t k = 0; k < pages; k++) ok += MapOne(b + k * kPage, kPage, id + k * kIdStep);
            if (ok != pages) { log("phys: remap хвоста %p+%#zx err=%lu, постранично %zu/%zu", (void*)b, s, err, ok, pages); return false; }
            return true;
        };
        if (vb < va) { size_t s = va - vb; if (!remap(vb, s, v.id0)) return false; }
        uintptr_t vend = vb + v.size;
        if (vend > end) { size_t s = vend - end; if (!remap(end, s, v.id0 + (end - vb) / kPage * kIdStep)) return false; }
    }
    return true;
}

// Keep native views at the independently remappable Xbox page granularity.
// Coalescing a run into one view forces UnmapRange to remove the whole view
// while restoring its tails. Game threads writing a retained neighbor then
// see an unbacked interval, even though their page was not requested unmapped.
// The map mutex cannot protect those ordinary CPU writes.
static bool MapRun(uintptr_t va, size_t n, uintptr_t id0, void (*log)(const char*, ...))
{
    if (gn.on) {
        std::vector<ULONG_PTR> ids(n); for (size_t k = 0; k < n; k++) ids[k] = id0 + k * kIdStep;
        const int r = gn.map((void*)va, n, ids.data(), 1); gn.maps++;
        if (r) { gn.failures++; log("phys: NATIVE map va=%p id=%#llx n=%zu r=%d", (void*)va, (unsigned long long)id0, n, r); return false; }
        g.viewCount += n; return true;
    }
    size_t size = n * kPage;
    if (!UnmapRange(va, size, log)) return false;
    size_t ok = 0;
    for (size_t k = 0; k < n; k++) ok += MapOne(va + k * kPage, kPage, id0 + k * kIdStep);
    if (ok != n) { log("phys: page views va=%p id=%#llx n=%zu mapped=%zu/%zu err=%lu", (void*)va, (unsigned long long)id0, n, ok, n, GetLastError()); return false; }
    return true;
}

// Частная (не разделяемая) память на месте страницы va внутри нашего резерва.
static bool MapPrivate(uintptr_t va, void (*log)(const char*, ...))
{
    if (gn.on) { const uintptr_t id = gn.nextPrivId; gn.nextPrivId -= kIdStep; return MapRun(va, 1, id, log); }
    if (!UnmapRange(va, kPage, log)) return false;
    VirtualFree((void*)va, kPage, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
    void* r = g.va2(nullptr, (void*)va, kPage, MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0);
    if (!r) { log("phys: private replace %p err=%lu", (void*)va, GetLastError()); return false; }
    g.priv[va] = kPage; return true;
}

// Общая точка: список (va_i,id_i). Runs batch bookkeeping only; every native
// section view remains one64-KiB page so partial remaps retain neighbors.
// Возвращает число страниц, отображённых через секцию; остальное (va вне наших резервов) — false в out-флаге fallback.
static size_t MapPages(const std::vector<std::pair<uintptr_t, uintptr_t>>& pages, std::vector<size_t>& fallbackIdx, void (*log)(const char*, ...))
{
    std::lock_guard<std::mutex> l(g.m);
    size_t done = 0, i = 0; g.mapCalls++;
    while (i < pages.size()) {
        uintptr_t va = pages[i].first, id = pages[i].second;
        if (id == ~(uintptr_t)0 && InOurRegion(va, kPage)) { if (MapPrivate(va, log)) done++; else fallbackIdx.push_back(i); i++; continue; }
        if (!InOurRegion(va, kPage) || !IdOk(id)) { fallbackIdx.push_back(i); g.fallbacks++; i++; continue; }
        size_t n = 1;
        while (i + n < pages.size() && pages[i + n].first == va + n * kPage && pages[i + n].second == id + n * kIdStep && IdOk(pages[i + n].second) && InOurRegion(va + n * kPage, kPage)) n++;
        if (MapRun(va, n, id, log)) done += n; else for (size_t k = 0; k < n; k++) fallbackIdx.push_back(i + k);
        i += n;
    }
    return done;
}
static bool UnmapPages(uintptr_t va, size_t n, void (*log)(const char*, ...))
{
    std::lock_guard<std::mutex> l(g.m);
    return UnmapRange(va, n * kPage, log);
}

// Write bytes into physical page `id` at `offset` (< one page). Native: BO CPU mapping; sections: temporary view.
static bool WritePage(uintptr_t id, size_t offset, const void* src, size_t size)
{
    if (!IdOk(id) || offset + size > kPage) return false;
    if (gn.on) return gn.write && gn.write(id, offset, src, size) == 0;
    if (!g.section) return false;
    const uint64_t off = IdOff(id);
    void* v = MapViewOfFile(g.section, FILE_MAP_WRITE, (DWORD)(off >> 32), (DWORD)off, kPage);
    if (!v) return false;
    memcpy((char*)v + offset, src, size); UnmapViewOfFile(v); return true;
}
} // namespace phys
