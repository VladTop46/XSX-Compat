// Прокси xgameruntime.dll (кладётся в каталог игры G:\, настоящий Microsoft-runtime остаётся в C:\windows\system32).
// Все экспорты пробрасываются в настоящий runtime. Перехват: QueryApiImpl для провайдера XPackage
// (AF406016-E850-4AA8-A88D-2F3DCB9DAC7E / IID 0B9D556E-AB70-43F4-BAA0-9999AC4CB040), который у PC-runtime
// для неупакованного процесса возвращает E_GAMERUNTIME_SERVICE_NOT_AVAILABLE (0x8924010D) — из-за этого
// пустой менеджер контента игры (g 0x1499aa6c0) и int3 в exe+0x1dac734 (см. STATIC-GAME-ANALYSIS.md).
//
// Раскладка vtable провайдера снята с xgameruntime.thunks.dll (GDK 261000), см. xpackage-vtable-slots.txt:
//  0 QI, 1 AddRef, 2 Release, 3 GetCurrentProcessPackageIdentifier, 4 IsPackagedProcess, 5 CreateInstallationMonitor,
//  6 CloseInstallationMonitorHandle, 7 GetInstallationProgress, 8 UpdateInstallationMonitor, 9 RegisterInstallationProgressChanged,
//  10 UnregisterInstallationProgressChanged, 11 GetUserLocale, 12 FindChunkAvailability, 13 EnumerateChunkAvailability,
//  14 ChangeChunkInstallOrder, 15 InstallChunks, 16 InstallChunksAsync, 17 InstallChunksResult, 18 EstimateDownloadSize,
//  19 UninstallChunks, 22 UnregisterPackageInstalled, 24 GetMountPathSize, 25 GetMountPath, 26 CloseMountHandle,
//  30 GetWriteStats, 32 UninstallUWPInstance, 33 EnumerateFeatures, 34 UninstallPackage, 37 MountWithUiAsync,
//  38 MountWithUiResult, 39 EnumeratePackages, 40 RegisterPackageInstalled, 41 GetPackageKind.
// Неизвестные слоты логируются и возвращают E_NOTIMPL.
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <utility>

#define XLOG(...) do { std::fprintf(stderr, "[XGR] " __VA_ARGS__); std::fputc('\n', stderr); std::fflush(stderr); } while (0)

static HMODULE g_real = nullptr;
extern "C" { void* g_fwd[7] = {}; }
static const char* const kNames[7] = { "DllCanUnloadNow", "InitializeApiImpl", "InitializeApiImplEx", "InitializeApiImplEx2",
                                       "QueryApiImpl", "UninitializeApiImpl", "XErrorReport" };

static void LoadReal()
{
    if (g_real) return;
    wchar_t sys[MAX_PATH]; GetSystemDirectoryW(sys, MAX_PATH); wcscat_s(sys, L"\\xgameruntime.dll");
    g_real = LoadLibraryW(sys);
    if (!g_real) { XLOG("FATAL: настоящий runtime %ls не загружен (err=%lu)", sys, GetLastError()); return; }
    for (int i = 0; i < 7; i++) g_fwd[i] = (void*)GetProcAddress(g_real, kNames[i]);
    XLOG("proxy: настоящий runtime %ls загружен (%p)", sys, (void*)g_real);
}

// Проброс экспортов без знания сигнатур: tail-jump через таблицу.
#define FWD(idx, name) \
    extern "C" void name(); \
    asm(".globl " #name "\n" #name ":\n\tjmp *g_fwd+" #idx "*8(%rip)\n");
FWD(0, xgr_DllCanUnloadNow)
FWD(1, xgr_InitializeApiImpl)
FWD(2, xgr_InitializeApiImplEx)
FWD(3, xgr_InitializeApiImplEx2)
FWD(5, xgr_UninitializeApiImpl)
FWD(6, xgr_XErrorReport)

// ---------------- XPackage ----------------
static const GUID kXPackageProvider = {0xAF406016, 0xE850, 0x4AA8, {0xA8, 0x8D, 0x2F, 0x3D, 0xCB, 0x9D, 0xAC, 0x7E}};
static const GUID kXPackageIid      = {0x0B9D556E, 0xAB70, 0x43F4, {0xBA, 0xA0, 0x99, 0x99, 0xAC, 0x4C, 0xB0, 0x40}};

struct XVersion { uint16_t major, minor, build, revision; };
struct XPackageDetails { const char* packageIdentifier; XVersion version; uint32_t kind; const char* displayName; const char* description;
                         const char* publisher; const char* storeId; bool installing; uint32_t index; uint32_t count; bool ageRestricted; const char* titleId; };
struct XPackageChunkSelector { uint32_t type; union { const char* str; uint32_t chunkId; }; };
struct XPackageInstallationProgress { uint64_t totalBytes, installedBytes, launchBytes; bool launchable, completed; };
typedef bool (CALLBACK* EnumPkgCb)(void*, const XPackageDetails*);
typedef bool (CALLBACK* ChunkCb)(void*, const XPackageChunkSelector*, uint32_t);

static const char kPkgId[] = "RockstarGamesInc.GTA6_xsx_local";   // <= 33 символа (XPACKAGE_IDENTIFIER_MAX_LENGTH)
static const uint32_t kChunks[] = { 1000,1001,1002,1003,1004,1005,1006,1007,1008,1009,1010,1011,1012,1013,1014,
                                    3000,3001,3002,3003,3004,3005,3006,3007,3008,3009 };   // layout_*_xs.xml

static void SelStr(const XPackageChunkSelector* s, char* o, size_t n)
{
    if (!s) { snprintf(o, n, "null"); return; }
    if (s->type == 2) snprintf(o, n, "chunk:%u", s->chunkId);
    else snprintf(o, n, "%s:%s", s->type == 0 ? "lang" : s->type == 1 ? "tag" : "feature", s->str && !IsBadStringPtrA(s->str, 128) ? s->str : "?");
}

static std::atomic<long> g_ref{1};
static HRESULT __stdcall P_QI(void* self, REFIID, void** out) { if (out) *out = self; g_ref++; return S_OK; }
static ULONG __stdcall P_AddRef(void*) { return (ULONG)++g_ref; }
static ULONG __stdcall P_Release(void*) { return (ULONG)--g_ref; }
static HRESULT __stdcall P_GetCurrentProcessPackageIdentifier(void*, size_t n, char* buf)
{
    XLOG("XPackageGetCurrentProcessPackageIdentifier(size=%zu) -> \"%s\"", n, kPkgId);
    if (!buf || n < sizeof kPkgId) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    memcpy(buf, kPkgId, sizeof kPkgId); return S_OK;
}
static bool __stdcall P_IsPackagedProcess(void*) { XLOG("XPackageIsPackagedProcess -> true"); return true; }
struct Monitor { uint32_t magic = 0x4d4f4e49; };
static HRESULT __stdcall P_CreateInstallationMonitor(void*, const char* id, uint32_t n, XPackageChunkSelector* sel, uint32_t ms, void* q, void** out)
{
    char s0[96] = "-"; if (n && sel) SelStr(sel, s0, sizeof s0);
    XLOG("XPackageCreateInstallationMonitor(%s, n=%u sel0=%s, ms=%u, q=%p)", id ? id : "null", n, s0, ms, q);
    if (!out) return E_POINTER; *out = new Monitor(); return S_OK;
}
static void __stdcall P_CloseInstallationMonitorHandle(void*, void* m) { XLOG("XPackageCloseInstallationMonitorHandle(%p)", m); delete (Monitor*)m; }
static void __stdcall P_GetInstallationProgress(void*, void* m, XPackageInstallationProgress* p)
{
    static std::atomic<int> k{0}; if (k++ < 4) XLOG("XPackageGetInstallationProgress(%p) -> completed", m);
    if (p) { p->totalBytes = p->installedBytes = p->launchBytes = 1; p->launchable = p->completed = true; }
}
static bool __stdcall P_UpdateInstallationMonitor(void*, void*) { return false; }
static HRESULT __stdcall P_RegisterInstallationProgressChanged(void*, void* m, void*, void*, uint64_t* tok) { XLOG("XPackageRegisterInstallationProgressChanged(%p)", m); if (tok) *tok = 1; return S_OK; }
static bool __stdcall P_UnregisterInstallationProgressChanged(void*, void*, uint64_t, bool) { return true; }
static HRESULT __stdcall P_GetUserLocale(void*, size_t n, char* buf) { XLOG("XPackageGetUserLocale -> en-US"); if (!buf || n < 6) return E_INVALIDARG; memcpy(buf, "en-US", 6); return S_OK; }
static HRESULT __stdcall P_FindChunkAvailability(void*, const char* id, uint32_t n, XPackageChunkSelector* sel, uint32_t* av)
{
    char s0[96] = "-"; if (n && sel) SelStr(sel, s0, sizeof s0);
    XLOG("XPackageFindChunkAvailability(%s, n=%u sel0=%s) -> Ready", id ? id : "null", n, s0);
    if (av) *av = 0; return S_OK;
}
static HRESULT __stdcall P_EnumerateChunkAvailability(void*, const char* id, uint32_t type, void* ctx, ChunkCb cb)
{
    XLOG("XPackageEnumerateChunkAvailability(%s, type=%u)", id ? id : "null", type);
    if (!cb) return E_INVALIDARG;
    XPackageChunkSelector s{}; s.type = type;
    if (type == 2) { for (uint32_t c : kChunks) { s.chunkId = c; if (!cb(ctx, &s, 0)) break; } }
    else if (type == 0) { s.str = "en-US"; cb(ctx, &s, 0); }
    return S_OK;
}
static HRESULT __stdcall P_EstimateDownloadSize(void*, const char*, uint32_t, XPackageChunkSelector*, uint64_t* sz, bool* confirm)
{ XLOG("XPackageEstimateDownloadSize -> 0"); if (sz) *sz = 0; if (confirm) *confirm = false; return S_OK; }
static HRESULT __stdcall P_ChangeChunkInstallOrder(void*, const char*, uint32_t, XPackageChunkSelector*) { XLOG("XPackageChangeChunkInstallOrder -> S_OK"); return S_OK; }
static HRESULT __stdcall P_EnumeratePackages(void*, uint32_t kind, uint32_t scope, void* ctx, EnumPkgCb cb)
{
    { HMODULE m = nullptr; GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)(void*)cb, &m);
      XLOG("XPackageEnumeratePackages(kind=%u, scope=%u) cb=%p (exe+%#llx) ctx=%p", kind, scope, (void*)cb, (unsigned long long)((uintptr_t)cb - (uintptr_t)m), ctx); }
    if (!cb) return E_INVALIDARG;
    if (kind == 0) {   // Game: только текущий пакет
        XPackageDetails d{}; d.packageIdentifier = kPkgId; d.version = {1, 0, 2, 1}; d.kind = 0; d.displayName = "Grand Theft Auto VI";
        d.description = "Grand Theft Auto VI"; d.publisher = "Rockstar Games"; d.storeId = "9NL3WWNZLZZN"; d.installing = false;
        d.index = 0; d.count = 1; d.ageRestricted = false; d.titleId = "7952987A";
        cb(ctx, &d);
    } else if (kind == 1) {
        // Content-пакеты. Игра хэширует storeId (joaat c таблицей exe 0x1465b23b0, cb exe+0x2937f20);
        // обязательный ключ 0xc27bfea1 == joaat("core"). Список задаётся env XSX_XGR_CONTENT (через запятую), по умолчанию "core".
        static char list[512]; if (!GetEnvironmentVariableA("XSX_XGR_CONTENT", list, sizeof list)) strcpy(list, "core");
        char tmp[512]; strcpy(tmp, list); uint32_t n = 0; for (char* t = strtok(tmp, ","); t; t = strtok(nullptr, ",")) n++;
        strcpy(tmp, list); uint32_t i = 0;
        for (char* t = strtok(tmp, ","); t; t = strtok(nullptr, ","), i++) {
            static char ids[16][40]; snprintf(ids[i & 15], sizeof ids[0], "%s", t);
            XPackageDetails d{}; d.packageIdentifier = ids[i & 15]; d.version = {1, 0, 2, 1}; d.kind = 1; d.displayName = ids[i & 15];
            d.description = ids[i & 15]; d.publisher = "Rockstar Games"; d.storeId = ids[i & 15]; d.installing = false;
            d.index = i; d.count = n; d.ageRestricted = false; d.titleId = "7952987A";
            XLOG("  -> content package id=storeId=\"%s\" (%u/%u)", ids[i & 15], i + 1, n);
            if (!cb(ctx, &d)) break;
        }
    }
    return S_OK;
}
static HRESULT __stdcall P_GetPackageKind(void*, const char* id, uint32_t* k) { XLOG("XPackageGetPackageKind(%s) -> Game", id ? id : "null"); if (k) *k = 0; return S_OK; }
static HRESULT __stdcall P_RegisterPackageInstalled(void*, void*, void*, void*, uint64_t* tok) { XLOG("XPackageRegisterPackageInstalled -> S_OK"); if (tok) *tok = 2; return S_OK; }
static bool __stdcall P_UnregisterPackageInstalled(void*, uint64_t, bool) { return true; }
static HRESULT __stdcall P_EnumerateFeatures(void*, const char* id, void*, void*) { XLOG("XPackageEnumerateFeatures(%s) -> нет фич", id ? id : "null"); return S_OK; }
static HRESULT __stdcall P_GetWriteStats(void*, uint64_t* s) { if (s) memset(s, 0, 32); return S_OK; }

template <int N> static HRESULT __stdcall P_Unknown(void*, void* a, void* b, void* c)
{
    XLOG("XPackage provider slot %d НЕ реализован (a=%p b=%p c=%p) -> E_NOTIMPL", N, a, b, c);
    return E_NOTIMPL;
}
template <int... I> static void FillUnknown(void** vt, std::integer_sequence<int, I...>) { ((vt[I] = (void*)&P_Unknown<I>), ...); }

static void* g_pkgVt[48];
static void* g_pkgObj[1] = { g_pkgVt };
static void InitPkg()
{
    static bool done = false; if (done) return; done = true;
    FillUnknown(g_pkgVt, std::make_integer_sequence<int, 48>{});
    g_pkgVt[0] = (void*)P_QI; g_pkgVt[1] = (void*)P_AddRef; g_pkgVt[2] = (void*)P_Release;
    g_pkgVt[3] = (void*)P_GetCurrentProcessPackageIdentifier; g_pkgVt[4] = (void*)P_IsPackagedProcess;
    g_pkgVt[5] = (void*)P_CreateInstallationMonitor; g_pkgVt[6] = (void*)P_CloseInstallationMonitorHandle;
    g_pkgVt[7] = (void*)P_GetInstallationProgress; g_pkgVt[8] = (void*)P_UpdateInstallationMonitor;
    g_pkgVt[9] = (void*)P_RegisterInstallationProgressChanged; g_pkgVt[10] = (void*)P_UnregisterInstallationProgressChanged;
    g_pkgVt[11] = (void*)P_GetUserLocale; g_pkgVt[12] = (void*)P_FindChunkAvailability; g_pkgVt[13] = (void*)P_EnumerateChunkAvailability;
    g_pkgVt[14] = (void*)P_ChangeChunkInstallOrder; g_pkgVt[18] = (void*)P_EstimateDownloadSize;
    g_pkgVt[22] = (void*)P_UnregisterPackageInstalled; g_pkgVt[30] = (void*)P_GetWriteStats; g_pkgVt[33] = (void*)P_EnumerateFeatures;
    g_pkgVt[39] = (void*)P_EnumeratePackages; g_pkgVt[40] = (void*)P_RegisterPackageInstalled; g_pkgVt[41] = (void*)P_GetPackageKind;
}

typedef HRESULT (WINAPI* QueryApiImplFn)(const GUID*, const GUID*, void**);
extern "C" HRESULT WINAPI xgr_QueryApiImpl(const GUID* provider, const GUID* iid, void** out)
{
    LoadReal();
    if (provider && iid && IsEqualGUID(*provider, kXPackageProvider) && IsEqualGUID(*iid, kXPackageIid)) {
        InitPkg(); if (!out) return E_POINTER; g_ref++; *out = g_pkgObj;
        static std::atomic<int> k{0}; if (k++ < 3) XLOG("QueryApiImpl(XPackage) -> собственный провайдер %p", (void*)g_pkgObj);
        return S_OK;
    }
    auto fn = (QueryApiImplFn)g_fwd[4];
    HRESULT hr = fn ? fn(provider, iid, out) : E_FAIL;
    if (FAILED(hr) && provider) XLOG("QueryApiImpl({%08lX-...}, {%08lX-...}) -> настоящий runtime hr=0x%08lX", (unsigned long)provider->Data1, iid ? (unsigned long)iid->Data1 : 0ul, (unsigned long)hr);
    return hr;
}

extern "C" BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) LoadReal();
    return TRUE;
}
