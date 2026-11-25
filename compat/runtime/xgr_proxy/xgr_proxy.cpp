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

// ---------------- generic logging wrapper (XUser provider, diagnostics) ----------------
// Wrapper object {vtbl, real}. Each slot trampoline logs its index once-per-slot (and every call to
// slots in XSX_XGR_TRACE_ALL) then tail-jumps into the real provider with `this` replaced; all
// argument registers/stack are preserved. Slot names: compat/reports/xuser-vtable-slots.txt.
static const GUID kXUserProvider = {0x01ACD177, 0x91F9, 0x4763, {0xA3, 0x8E, 0xCC, 0xBB, 0x55, 0xCE, 0x32, 0xE0}};
static const char* XUserSlotName(int i)
{
    static const char* n[64] = {"QueryInterface","AddRef","Release","DuplicateHandle|GetGamertag?","CloseHandle","Compare","GetMaxUsers",
        "AddAsync","AddResult","GetLocalId","FindUserByLocalId","GetId","FindUserById","GetIsGuest","GetState","slot15",
        "GetGamerPictureAsync","GetGamerPictureResultSize","GetGamerPictureResult","GetAgeGroup","CheckPrivilege",
        "ResolvePrivilegeWithUiAsync","ResolvePrivilegeWithUiResult","GetTokenAndSignatureAsync","GetTokenAndSignatureResultSize",
        "GetTokenAndSignatureResult","GetTokenAndSignatureUtf16Async","GetTokenAndSignatureUtf16ResultSize","GetTokenAndSignatureUtf16Result",
        "ResolveIssueWithUiAsync","ResolveIssueWithUiResult","ResolveIssueWithUiUtf16Async","ResolveIssueWithUiUtf16Result",
        "RegisterForChangeEvent","UnregisterForChangeEvent","GetSignOutDeferral","CloseSignOutDeferralHandle","AddByIdWithUiAsync",
        "AddByIdWithUiResult","slot39","slot40","slot41","IsStoreUser","PlatformRemoteConnectSetEventHandlers",
        "PlatformRemoteConnectCancelPrompt","PlatformSpopPromptSetEventHandlers","PlatformSpopPromptComplete","IsSignOutPresent",
        "SignOutAsync","SignOutResult"};
    return i < 64 && n[i] ? n[i] : "?";
}
static std::atomic<unsigned> g_slotCalls[64];
extern "C" void xgr_LogSlot(unsigned slot, uintptr_t a1, uintptr_t a2, uintptr_t a3)
{
    const unsigned k = g_slotCalls[slot & 63]++;
    if (k < 6 || !(k % 1000)) XLOG("XUser slot %u %s call#%u args=(%p,%p,%p)", slot, XUserSlotName((int)slot), k + 1, (void*)a1, (void*)a2, (void*)a3);
}
struct Wrapped { void** vt; void* real; };
#define TRAMP(i) \
    extern "C" void xgr_tramp_##i(); \
    asm(".globl xgr_tramp_" #i "\nxgr_tramp_" #i ":\n" \
        "\tpush %rcx\n\tpush %rdx\n\tpush %r8\n\tpush %r9\n\tsub $0x28,%rsp\n" \
        "\tmov $" #i ",%ecx\n\tmov 0x38(%rsp),%rdx\n\tmov 0x30(%rsp),%r8\n\tmov 0x28(%rsp),%r9\n" \
        "\tcall xgr_LogSlot\n\tadd $0x28,%rsp\n\tpop %r9\n\tpop %r8\n\tpop %rdx\n\tpop %rcx\n" \
        "\tmov 8(%rcx),%rcx\n\tmov (%rcx),%rax\n\tjmp *" #i "*8(%rax)\n");
#define T8(b) TRAMP(b##0) TRAMP(b##1) TRAMP(b##2) TRAMP(b##3) TRAMP(b##4) TRAMP(b##5) TRAMP(b##6) TRAMP(b##7)
TRAMP(0) TRAMP(1) TRAMP(2) TRAMP(3) TRAMP(4) TRAMP(5) TRAMP(6) TRAMP(7) TRAMP(8) TRAMP(9)
TRAMP(10) TRAMP(11) TRAMP(12) TRAMP(13) TRAMP(14) TRAMP(15) TRAMP(16) TRAMP(17) TRAMP(18) TRAMP(19)
TRAMP(20) TRAMP(21) TRAMP(22) TRAMP(23) TRAMP(24) TRAMP(25) TRAMP(26) TRAMP(27) TRAMP(28) TRAMP(29)
TRAMP(30) TRAMP(31) TRAMP(32) TRAMP(33) TRAMP(34) TRAMP(35) TRAMP(36) TRAMP(37) TRAMP(38) TRAMP(39)
TRAMP(40) TRAMP(41) TRAMP(42) TRAMP(43) TRAMP(44) TRAMP(45) TRAMP(46) TRAMP(47) TRAMP(48) TRAMP(49)
static void* g_wrapVt[50] = {
#define E(i) (void*)xgr_tramp_##i
    E(0),E(1),E(2),E(3),E(4),E(5),E(6),E(7),E(8),E(9),E(10),E(11),E(12),E(13),E(14),E(15),E(16),E(17),E(18),E(19),
    E(20),E(21),E(22),E(23),E(24),E(25),E(26),E(27),E(28),E(29),E(30),E(31),E(32),E(33),E(34),E(35),E(36),E(37),E(38),E(39),
    E(40),E(41),E(42),E(43),E(44),E(45),E(46),E(47),E(48),E(49) };
#undef E
static void LogPairOnce(const GUID* p, const GUID* i, HRESULT hr)
{
    static SRWLOCK lock = SRWLOCK_INIT; static GUID seen[128][2]; static int n = 0;
    AcquireSRWLockExclusive(&lock);
    bool found = false; for (int k = 0; k < n; k++) if (IsEqualGUID(seen[k][0], *p) && IsEqualGUID(seen[k][1], *i)) { found = true; break; }
    if (!found && n < 128) { seen[n][0] = *p; seen[n][1] = *i; n++; }
    ReleaseSRWLockExclusive(&lock);
    if (!found) XLOG("QueryApiImpl FIRST provider={%08lX-%04X-%04X} iid={%08lX-%04X-%04X} -> hr=0x%08lX", p->Data1, p->Data2, p->Data3, i->Data1, i->Data2, i->Data3, (unsigned long)hr);
}
// ---------------- offline local XUser provider (XSX_XUSER=0 disables) ----------------
// Slots: compat/reports/xuser-vtable-slots.txt (GDK 261000 thunks). One local adult user "Player",
// signed in; both silent (AddDefaultUserSilently) and UI adds succeed. Async completion uses the
// REAL runtime XAsync provider {073B7DCB}: Begin(7) -> Schedule(9) -> DoWork completes(10).
static const GUID kXAsyncProvider = {0x073B7DCB, 0x1FCF, 0x4030, {0x94, 0xBE, 0xE3, 0xC9, 0xEB, 0x62, 0x34, 0x28}};
static const GUID kXUserGamertagIid = {0xCEF4FAC0, 0x7676, 0x4A94, {0xA1, 0x19, 0x4C, 0x43, 0xF9, 0xEB, 0x5B, 0x74}};
static const GUID kXUserExtIids[] = {
    {0xEB9BF948, 0x18DC, 0x4D82, {0xBB, 0xCC, 0x40, 0xE0, 0xA8, 0x09, 0xC4, 0xC0}},
    {0x5131D685, 0x4394, 0x4EE6, {0x8C, 0x18, 0xBF, 0xB5, 0xD4, 0xAE, 0xF1, 0xFF}},
    {0x079415E3, 0x6727, 0x437F, {0x8E, 0x9D, 0x8F, 0x8F, 0x9B, 0x24, 0x39, 0xF7}},
    {0x26F3C674, 0xA2FE, 0x44FA, {0xB6, 0xC4, 0xA3, 0x23, 0xBC, 0x94, 0xFF, 0x53}} };
static bool XUserLocalOn() { static bool on = [] { char v[4]; return !(GetEnvironmentVariableA("XSX_XUSER", v, sizeof v) > 0 && v[0] == '0'); }(); return on; }
struct XUserObj { int magic; } static g_localUser = {0x55534552};
static const uint64_t kLocalXuid = 0x0009000000C0FFEEull, kLocalId = 1;
static void* g_xasync = nullptr;   // real XAsync provider object
struct XAsyncProviderData { void* async; uint64_t bufferSize; void* buffer; void* context; };
typedef HRESULT (CALLBACK* XAsyncProviderFn)(uint32_t op, const XAsyncProviderData* data);
static HRESULT CALLBACK U_AddProvider(uint32_t op, const XAsyncProviderData* d)
{
    if (op == 1 /*DoWork*/) {
        auto complete = (HRESULT(STDMETHODCALLTYPE*)(void*, void*, HRESULT, uint64_t))(*(void***)g_xasync)[10];
        complete(g_xasync, d->async, S_OK, 0);
    }
    return S_OK;
}
static HRESULT CompleteLater(void* block, const char* what)
{
    if (!g_xasync) return E_FAIL;
    auto begin = (HRESULT(STDMETHODCALLTYPE*)(void*, void*, void*, const void*, const char*, XAsyncProviderFn))(*(void***)g_xasync)[7];
    auto schedule = (HRESULT(STDMETHODCALLTYPE*)(void*, void*, uint32_t))(*(void***)g_xasync)[9];
    HRESULT hr = begin(g_xasync, block, nullptr, (const void*)&g_localUser, what, U_AddProvider);
    if (SUCCEEDED(hr)) hr = schedule(g_xasync, block, 0);
    return hr;
}
typedef uintptr_t U;
static HRESULT STDMETHODCALLTYPE U_QI(void* o, REFIID, void** out) { if (out) *out = o; return S_OK; }
static ULONG STDMETHODCALLTYPE U_AddRef(void*) { return 2; }
static ULONG STDMETHODCALLTYPE U_Release(void*) { return 1; }
static HRESULT STDMETHODCALLTYPE U_Duplicate(void*, void* h, void** out) { if (!out) return E_POINTER; *out = h; return S_OK; }
static void STDMETHODCALLTYPE U_Close(void*, void*) {}
static int32_t STDMETHODCALLTYPE U_Compare(void*, void* a, void* b) { return a == b ? 0 : (a < b ? -1 : 1); }
static HRESULT STDMETHODCALLTYPE U_MaxUsers(void*, uint32_t* n) { if (!n) return E_POINTER; *n = 1; return S_OK; }
static HRESULT STDMETHODCALLTYPE U_AddAsync(void*, uint32_t options, void* block)
{ HRESULT hr = CompleteLater(block, "XUserAddAsync"); XLOG("XUser(local) AddAsync options=%u block=%p -> 0x%08lX", options, block, (unsigned long)hr); return hr; }
static HRESULT STDMETHODCALLTYPE U_AddResult(void*, void* block, void** user)
{ if (!user) return E_POINTER; *user = &g_localUser; XLOG("XUser(local) AddResult block=%p -> S_OK user=%p (\"Player\")", block, (void*)&g_localUser); return S_OK; }
static HRESULT STDMETHODCALLTYPE U_GetLocalId(void*, void*, uint64_t* id) { if (!id) return E_POINTER; *id = kLocalId; return S_OK; }
static HRESULT STDMETHODCALLTYPE U_FindByLocalId(void*, uint64_t id, void** out) { if (!out) return E_POINTER; if (id != kLocalId) return (HRESULT)0x89245101; *out = &g_localUser; return S_OK; }
static HRESULT STDMETHODCALLTYPE U_GetId(void*, void*, uint64_t* xuid) { if (!xuid) return E_POINTER; *xuid = kLocalXuid; return S_OK; }
static HRESULT STDMETHODCALLTYPE U_FindById(void*, uint64_t xuid, void** out) { if (!out) return E_POINTER; if (xuid != kLocalXuid) return (HRESULT)0x89245101; *out = &g_localUser; return S_OK; }
static HRESULT STDMETHODCALLTYPE U_IsGuest(void*, void*, bool* g) { if (!g) return E_POINTER; *g = false; return S_OK; }
static HRESULT STDMETHODCALLTYPE U_State(void*, void*, uint32_t* st) { if (!st) return E_POINTER; *st = 0 /*SignedIn*/; return S_OK; }
static HRESULT STDMETHODCALLTYPE U_AgeGroup(void*, void*, uint32_t* g) { if (!g) return E_POINTER; *g = 3 /*Adult*/; return S_OK; }
static HRESULT STDMETHODCALLTYPE U_CheckPrivilege(void*, void*, uint32_t, uint32_t priv, bool* has, uint32_t* reason)
{ if (!has) return E_POINTER; *has = true; if (reason) *reason = 0; static int n; if (n++ < 8) XLOG("XUser(local) CheckPrivilege %u -> granted", priv); return S_OK; }
static HRESULT STDMETHODCALLTYPE U_RegisterChange(void*, void*, void*, void* cb, uint64_t* token) { if (token) *token = 0x5501; XLOG("XUser(local) RegisterForChangeEvent cb=%p", cb); return S_OK; }
static bool STDMETHODCALLTYPE U_UnregisterChange(void*, uint64_t, bool) { return true; }
static HRESULT STDMETHODCALLTYPE U_IsStoreUser(void*, void*, bool* b) { if (b) *b = false; return S_OK; }
static bool STDMETHODCALLTYPE U_SignOutPresent(void*) { return false; }
static HRESULT STDMETHODCALLTYPE U_Gamertag(void*, void*, uint32_t component, size_t size, char* buf, size_t* used)
{
    static const char tag[] = "Player";
    if (used) *used = sizeof tag;
    if (!buf || size < sizeof tag) return (HRESULT)0x8007007A; /* ERROR_INSUFFICIENT_BUFFER */
    memcpy(buf, tag, sizeof tag); static int n; if (n++ < 4) XLOG("XUser(local) GetGamertag component=%u -> Player", component); return S_OK;
}
static std::atomic<unsigned> g_uMissing[64];
extern "C" HRESULT xgr_UMissing(unsigned slot)
{ if (g_uMissing[slot & 63]++ < 4) XLOG("XUser(local) slot %u %s NOT IMPLEMENTED -> E_NOTIMPL", slot, XUserSlotName((int)slot)); return E_NOTIMPL; }
#define UMISS(i) extern "C" void xgr_umiss_##i(); asm(".globl xgr_umiss_" #i "\nxgr_umiss_" #i ":\n\tsub $0x28,%rsp\n\tmov $" #i ",%ecx\n\tcall xgr_UMissing\n\tadd $0x28,%rsp\n\tret\n");
UMISS(15) UMISS(16) UMISS(17) UMISS(18) UMISS(21) UMISS(22) UMISS(23) UMISS(24) UMISS(25) UMISS(26) UMISS(27) UMISS(28)
UMISS(29) UMISS(30) UMISS(31) UMISS(32) UMISS(35) UMISS(36) UMISS(37) UMISS(38) UMISS(39) UMISS(40) UMISS(41)
UMISS(43) UMISS(44) UMISS(45) UMISS(46) UMISS(48) UMISS(49)
static void* g_uVt[50] = { (void*)U_QI, (void*)U_AddRef, (void*)U_Release, (void*)U_Duplicate, (void*)U_Close, (void*)U_Compare,
    (void*)U_MaxUsers, (void*)U_AddAsync, (void*)U_AddResult, (void*)U_GetLocalId, (void*)U_FindByLocalId, (void*)U_GetId,
    (void*)U_FindById, (void*)U_IsGuest, (void*)U_State, (void*)xgr_umiss_15, (void*)xgr_umiss_16, (void*)xgr_umiss_17,
    (void*)xgr_umiss_18, (void*)U_AgeGroup, (void*)U_CheckPrivilege, (void*)xgr_umiss_21, (void*)xgr_umiss_22, (void*)xgr_umiss_23,
    (void*)xgr_umiss_24, (void*)xgr_umiss_25, (void*)xgr_umiss_26, (void*)xgr_umiss_27, (void*)xgr_umiss_28, (void*)xgr_umiss_29,
    (void*)xgr_umiss_30, (void*)xgr_umiss_31, (void*)xgr_umiss_32, (void*)U_RegisterChange, (void*)U_UnregisterChange,
    (void*)xgr_umiss_35, (void*)xgr_umiss_36, (void*)xgr_umiss_37, (void*)xgr_umiss_38, (void*)xgr_umiss_39, (void*)xgr_umiss_40,
    (void*)xgr_umiss_41, (void*)U_IsStoreUser, (void*)xgr_umiss_43, (void*)xgr_umiss_44, (void*)xgr_umiss_45, (void*)xgr_umiss_46,
    (void*)U_SignOutPresent, (void*)xgr_umiss_48, (void*)xgr_umiss_49 };
static void* g_uGtVt[4] = { (void*)U_QI, (void*)U_AddRef, (void*)U_Release, (void*)U_Gamertag };
static struct { void** vt; } g_uObj = { g_uVt }, g_uGtObj = { g_uGtVt };
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
    if (XUserLocalOn() && provider && iid && out && IsEqualGUID(*provider, kXUserProvider)) {
        if (!g_xasync && fn) fn(&kXAsyncProvider, &kXAsyncProvider, &g_xasync);
        bool main = IsEqualGUID(*iid, kXUserProvider); for (auto& g : kXUserExtIids) main |= IsEqualGUID(*iid, g) != 0;
        if (IsEqualGUID(*iid, kXUserGamertagIid)) { *out = &g_uGtObj; LogPairOnce(provider, iid, S_OK); return S_OK; }
        if (main) { *out = &g_uObj; LogPairOnce(provider, iid, S_OK);
            static int k; if (!k++) XLOG("QueryApiImpl(XUser) -> ЛОКАЛЬНЫЙ офлайн-провайдер (Player), xasync=%p", g_xasync); return S_OK; }
    }
    HRESULT hr = fn ? fn(provider, iid, out) : E_FAIL;
    if (provider && iid) LogPairOnce(provider, iid, hr);
    if (SUCCEEDED(hr) && out && *out && provider && IsEqualGUID(*provider, kXUserProvider)) {
        static Wrapped* w = nullptr;   // the runtime returns a singleton; keep one wrapper
        if (!w || w->real != *out) { w = new Wrapped{g_wrapVt, *out}; XLOG("QueryApiImpl(XUser) -> logging wrapper %p over real %p", (void*)w, *out); }
        *out = w;
    }
    if (FAILED(hr) && provider) XLOG("QueryApiImpl({%08lX-...}, {%08lX-...}) -> настоящий runtime hr=0x%08lX", (unsigned long)provider->Data1, iid ? (unsigned long)iid->Data1 : 0ul, (unsigned long)hr);
    return hr;
}

extern "C" BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) LoadReal();
    return TRUE;
}
