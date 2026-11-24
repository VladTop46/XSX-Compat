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
static constexpr uintptr_t kFirstId = 0x1001;            // XMemAllocatePhysicalPages выдаёт id с 0x1001
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

struct View { size_t size; uintptr_t id0; };              // view, отображающий подряд идущие id начиная с id0
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
    HMODULE kb = LoadLibraryW(L"kernelbase.dll");
    g.va2 = (VirtualAlloc2Fn)GetProcAddress(kb, "VirtualAlloc2");
    g.mv3 = (MapViewOfFile3Fn)GetProcAddress(kb, "MapViewOfFile3");
    g.uv2 = (UnmapViewOfFile2Fn)GetProcAddress(kb, "UnmapViewOfFile2");
    if (!g.va2 || !g.mv3 || !g.uv2) { log("phys: нет VirtualAlloc2/MapViewOfFile3/UnmapViewOfFile2 -> без алиасинга"); return false; }
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
static bool InOurRegion(uintptr_t va, size_t size)
{
    auto it = g.regions.upper_bound(va); if (it == g.regions.begin()) return false; --it;
    return va >= it->first && va + size <= it->first + it->second;
}
static bool IdOk(uintptr_t id) { return id >= kFirstId && (uint64_t)(id - kFirstId + 1) * kPage <= kSectionSize; }

// Снять все view, пересекающиеся с [va, va+size); части view вне диапазона восстанавливаются (переотображаются).
static bool UnmapRange(uintptr_t va, size_t size, void (*log)(const char*, ...))
{
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
            void* r = g.mv3(g.section, GetCurrentProcess(), (void*)b, (ULONG64)(id - kFirstId) * kPage, s, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0);
            if (!r) { log("phys: remap хвоста %p+%#zx err=%lu", (void*)b, s, GetLastError()); return false; }
            g.views[b] = {s, id}; g.viewCount++; return true;
        };
        if (vb < va) { size_t s = va - vb; if (!remap(vb, s, v.id0)) return false; }
        uintptr_t vend = vb + v.size;
        if (vend > end) { size_t s = vend - end; if (!remap(end, s, v.id0 + (end - vb) / kPage)) return false; }
    }
    return true;
}

// Отобразить подряд идущие id [id0, id0+n) в VA va (va внутри нашего placeholder-резерва).
static bool MapRun(uintptr_t va, size_t n, uintptr_t id0, void (*log)(const char*, ...))
{
    size_t size = n * kPage;
    if (!UnmapRange(va, size, log)) return false;
    // расщепить placeholder до точного [va, va+size) (если он уже точный — VirtualFree вернёт ошибку, это нормально)
    VirtualFree((void*)va, size, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
    void* r = g.mv3(g.section, GetCurrentProcess(), (void*)va, (ULONG64)(id0 - kFirstId) * kPage, size, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0);
    if (!r) { log("phys: MapViewOfFile3(va=%p, id=%#llx, n=%zu) err=%lu", (void*)va, (unsigned long long)id0, n, GetLastError()); return false; }
    g.views[va] = {size, id0}; g.viewCount++;
    return true;
}

// Частная (не разделяемая) память на месте страницы va внутри нашего резерва.
static bool MapPrivate(uintptr_t va, void (*log)(const char*, ...))
{
    if (!UnmapRange(va, kPage, log)) return false;
    VirtualFree((void*)va, kPage, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
    void* r = g.va2(nullptr, (void*)va, kPage, MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0);
    if (!r) { log("phys: private replace %p err=%lu", (void*)va, GetLastError()); return false; }
    g.priv[va] = kPage; return true;
}

// Общая точка: отобразить список (va_i, id_i). Подряд идущие VA с подряд идущими id объединяются в один view.
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
        while (i + n < pages.size() && pages[i + n].first == va + n * kPage && pages[i + n].second == id + n && IdOk(pages[i + n].second) && InOurRegion(va + n * kPage, kPage)) n++;
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

} // namespace phys
