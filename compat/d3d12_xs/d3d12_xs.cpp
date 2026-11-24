#include <vector>
// d3d12_xs.dll — прототип слоя совместимости Xbox Series D3D12 -> PC D3D12 (VKD3D-Proton).
// Экспорты соответствуют импортам game_scarlett_master.exe и Xbox FidelityFX DLL.
// Сигнатуры D3D12XboxCreateDevice/D3D12ConfigureVirtualMemoryX восстановлены по
// использованию в GDK samples (публичных заголовков d3d12_xs.h в checkout нет).
#include <atomic>
#include <windows.h>
#include <d3d12.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>

#define XS_LOG(...) do { fprintf(stderr, "[D3D12_XS] " __VA_ARGS__); fputc('\n', stderr); fflush(stderr); } while (0)

// Диагностика: модуль+RVA вызывающего кода (для статического разбора callsite игры).
static void CallerStr(void* ret, char* out, size_t n)
{
    HMODULE mod = nullptr;
    if (ret && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                  GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                  (LPCSTR)ret, &mod)) {
        char path[MAX_PATH] = {};
        GetModuleFileNameA(mod, path, MAX_PATH);
        const char* base = strrchr(path, '\\'); base = base ? base + 1 : path;
        snprintf(out, n, "%s+0x%llx", base, (unsigned long long)((char*)ret - (char*)mod));
    } else snprintf(out, n, "%p", ret);
}
#define CALLER(buf) char buf[300]; CallerStr(__builtin_return_address(0), buf, sizeof buf)

// First-chance logging only: the game's own crash handler sometimes emits an
// empty .dmp. Never handle/skip the fault or alter the supplied CONTEXT.
static LONG CALLBACK TraceGraphicsFault(EXCEPTION_POINTERS* exception)
{
    const DWORD code = exception->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_IN_PAGE_ERROR && code != EXCEPTION_BREAKPOINT)
        return EXCEPTION_CONTINUE_SEARCH;
    static volatile LONG active = 0, count = 0, breakpoints = 0;
    if (InterlockedCompareExchange(&active, 1, 0)) return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedIncrement(code == EXCEPTION_BREAKPOINT ? &breakpoints : &count) <= 8) {
        const CONTEXT& r = *exception->ContextRecord;
        char where[300]; CallerStr((void*)r.Rip, where, sizeof where);
        XS_LOG("FIRST-CHANCE code=0x%08lX thread=%lu at=%s access=%llu address=%p", (unsigned long)code,
            GetCurrentThreadId(), where, exception->ExceptionRecord->NumberParameters ?
            (unsigned long long)exception->ExceptionRecord->ExceptionInformation[0] : 0,
            exception->ExceptionRecord->NumberParameters > 1 ? (void*)exception->ExceptionRecord->ExceptionInformation[1] : nullptr);
        XS_LOG("FAULT REG rip=%p rsp=%p rbp=%p rax=%p rbx=%p rcx=%p rdx=%p", (void*)r.Rip,
            (void*)r.Rsp, (void*)r.Rbp, (void*)r.Rax, (void*)r.Rbx, (void*)r.Rcx, (void*)r.Rdx);
        XS_LOG("FAULT REG rsi=%p rdi=%p r8=%p r9=%p r10=%p r11=%p r12=%p r13=%p r14=%p r15=%p",
            (void*)r.Rsi, (void*)r.Rdi, (void*)r.R8, (void*)r.R9, (void*)r.R10, (void*)r.R11,
            (void*)r.R12, (void*)r.R13, (void*)r.R14, (void*)r.R15);
        if (code == EXCEPTION_ACCESS_VIOLATION && (uintptr_t)r.Rip == (uintptr_t)GetModuleHandleA(nullptr) + 0xb5a5f8) {
            uintptr_t b0 = (uintptr_t)GetModuleHandleA(nullptr); uint64_t g[24] = {}; ReadProcessMemory(GetCurrentProcess(), (void*)(b0 + 0x75d5980), g, sizeof g, nullptr);
            for (int i = 0; i < 24; i++) XS_LOG("GLOBAL exe+%#x = %p", 0x75d5980 + i * 8, (void*)g[i]);
        }
        if (code == EXCEPTION_BREAKPOINT && breakpoints == 1) { unsigned char big[0x200] = {}; SIZE_T u = 0; if (ReadProcessMemory(GetCurrentProcess(), (void*)r.R14, big, sizeof big, &u) && u) { char path[MAX_PATH]; GetEnvironmentVariableA("XSX_BP_DUMP", path, sizeof path) ? (void)0 : (void)strcpy(path, "C:\\bp-blob.bin"); HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, 0, 0); DWORD wr; if (f != INVALID_HANDLE_VALUE) { WriteFile(f, big, (DWORD)u, &wr, 0); CloseHandle(f); } } }
        if (code == EXCEPTION_BREAKPOINT && (uintptr_t)r.Rip == (uintptr_t)GetModuleHandleA(nullptr) + 0x1dac734) {
            uintptr_t base = (uintptr_t)GetModuleHandleA(nullptr), obj = 0, t = 0; SIZE_T u;
            ReadProcessMemory(GetCurrentProcess(), (void*)(base + 0x7f00510), &obj, 8, &u);
            uintptr_t f[3] = {}; if (obj) ReadProcessMemory(GetCurrentProcess(), (void*)(obj + 0x2a0), f, 24, &u);
            uint32_t cnt = 0; uintptr_t data = 0; if (f[2]) { ReadProcessMemory(GetCurrentProcess(), (void*)(f[2] + 0x30), &cnt, 4, &u); ReadProcessMemory(GetCurrentProcess(), (void*)(f[2] + 0x38), &data, 8, &u); }
            XS_LOG("ASSERT1dac734 obj=%p [+2a0]=%p [+2a8]=%p [+2b0]=%p table.count=%u table.data=%p", (void*)obj, (void*)f[0], (void*)f[1], (void*)f[2], cnt, (void*)data);
            if (data && cnt) { for (int e = 0; e < 2; e++) { uintptr_t pe = 0; ReadProcessMemory(GetCurrentProcess(), (void*)(data + 0x10 + e * 16 + 8), &pe, 8, &u); unsigned char pb[64] = {}; if (pe && ReadProcessMemory(GetCurrentProcess(), (void*)pe, pb, 64, &u)) { char hh[200] = {}; for (int i = 0; i < 48; i++) sprintf(hh + i * 3, "%02x ", pb[i]); char who[200]; CallerStr((void*)(*(uintptr_t*)pb), who, sizeof who); XS_LOG("ASSERT1dac734 entry%d ptr=%p vt=%s: %s", e, (void*)pe, who, hh); } } }
            if (data && cnt) { uint32_t k[16] = {}; ReadProcessMemory(GetCurrentProcess(), (void*)data, k, sizeof k, &u); char h[300] = {}; for (int i = 0; i < 16; i++) sprintf(h + i * 9, "%08x ", k[i]); XS_LOG("ASSERT1dac734 table head: %s", h); }
        }
        if (code == EXCEPTION_BREAKPOINT) for (int w = 0; w < 3; ++w) {
            const void* q = w == 0 ? (void*)r.R14 : w == 1 ? (void*)r.Rsi : (void*)r.Rdi; unsigned char b[64] = {}; SIZE_T u = 0;
            if (ReadProcessMemory(GetCurrentProcess(), q, b, sizeof b, &u) && u) { char h[200] = {}; for (int i = 0; i < 40 && i < (int)u; i++) sprintf(h + i * 3, "%02x ", b[i]); XS_LOG("BP DUMP %s=%p: %s", w == 0 ? "r14" : w == 1 ? "rsi" : "rdi", q, h); }
        }
        for (unsigned i = 0; i < 24; ++i) {
            uintptr_t value = 0; SIZE_T used = 0;
            if (ReadProcessMemory(GetCurrentProcess(), (void*)(r.Rsp + i * 8), &value, sizeof value, &used) && used == sizeof value) {
                char name[300]; CallerStr((void*)value, name, sizeof name);
                XS_LOG("FAULT STACK +0x%x=%p %s", i * 8, (void*)value, name);
            }
        }
    }
    InterlockedExchange(&active, 0);
    return EXCEPTION_CONTINUE_SEARCH;
}


// ---- Диагностический трап (env XSX_TRAP="rva1,rva2,..."): int3-патч в exe, лог регистров/строк при входе, затем восстановление и single-step.
struct TrapEnt { uintptr_t addr; unsigned char orig; bool wantRet; };
struct RetTrap { uintptr_t ret; unsigned char orig; uintptr_t out; char what[96]; };
static RetTrap g_ret[64]; static volatile LONG g_nRet = 0;
static void FmtGuid(uintptr_t p, char* o, size_t n) { GUID g{}; if (!p || !ReadProcessMemory(GetCurrentProcess(), (void*)p, &g, sizeof g, nullptr)) { snprintf(o, n, "%p", (void*)p); return; }
    snprintf(o, n, "%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X", (unsigned long)g.Data1, g.Data2, g.Data3, g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]); }
static uintptr_t g_watchTable = 0; static TrapEnt g_traps[16]; static int g_nTraps = 0; static uintptr_t g_rearm = 0; static volatile LONG g_trapN = 0;
static void PeekStr(const char* tag, uintptr_t p)
{
    unsigned char b[48] = {}; SIZE_T u = 0; if (p < 0x10000 || !ReadProcessMemory(GetCurrentProcess(), (void*)p, b, 47, &u) || !u) return;
    int n = 0; while (n < 40 && b[n] >= 0x20 && b[n] < 0x7f) n++;
    if (n >= 3) XS_LOG("TRAP   %s -> \"%.*s\"", tag, n, b);
}

#include <tlhelp32.h>
// Выставить аппаратные watch на запись (8 байт) во ВСЕХ потоках процесса, кроме текущего (текущий правится через CONTEXT).
static void WatchAllThreads(uintptr_t a0, uintptr_t a1)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0); if (snap == INVALID_HANDLE_VALUE) return; THREADENTRY32 te{sizeof te};
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == GetCurrentThreadId()) continue;
        HANDLE h = OpenThread(THREAD_ALL_ACCESS, FALSE, te.th32ThreadID); if (!h) continue;
        if (SuspendThread(h) != (DWORD)-1) { CONTEXT c{}; c.ContextFlags = CONTEXT_DEBUG_REGISTERS; if (GetThreadContext(h, &c)) { c.Dr0 = a0; c.Dr1 = a1; c.Dr7 |= (1u << 0) | (1u << 2) | (1u << 16) | (3u << 18) | (1u << 20) | (3u << 22); SetThreadContext(h, &c); } ResumeThread(h); }
        CloseHandle(h);
    }
    CloseHandle(snap);
}
static LONG CALLBACK TrapHandler(EXCEPTION_POINTERS* e)
{
    CONTEXT& r = *e->ContextRecord; const DWORD c = e->ExceptionRecord->ExceptionCode;
    if (c == EXCEPTION_SINGLE_STEP && (r.Dr6 & 0xF)) {
        static volatile LONG wn = 0; LONG k = InterlockedIncrement(&wn);
        for (int i = 0; i < 4; i++) if (r.Dr6 & (1u << i)) {
            uintptr_t wa = i == 0 ? r.Dr0 : i == 1 ? r.Dr1 : i == 2 ? r.Dr2 : r.Dr3; uint64_t val = 0; ReadProcessMemory(GetCurrentProcess(), (void*)wa, &val, 8, nullptr);
            char who[200]; CallerStr((void*)r.Rip, who, sizeof who); uintptr_t ra = 0; ReadProcessMemory(GetCurrentProcess(), (void*)r.Rsp, &ra, 8, nullptr); char w2[200]; CallerStr((void*)ra, w2, sizeof w2);
            if (k <= 120) XS_LOG("WATCH #%ld dr%d addr=%p val=%016llx rip=%s [rsp]=%s tid=%lu", k, i, (void*)wa, (unsigned long long)val, who, w2, GetCurrentThreadId());
            if (k <= 40) for (int q = 1; q < 40; q++) { uintptr_t v = 0; ReadProcessMemory(GetCurrentProcess(), (void*)(r.Rsp + q * 8), &v, 8, nullptr); uintptr_t b0 = (uintptr_t)GetModuleHandleA(nullptr); if (v > b0 && v < b0 + 0x8000000) XS_LOG("WATCH    stk+%#x exe+%#llx", q * 8, (unsigned long long)(v - b0)); }
            auto setData = [&](uintptr_t d) { if (d > 0x10000) { r.Dr0 = d; r.Dr1 = d + 8; r.Dr7 |= (1u << 0) | (1u << 2) | (1u << 16) | (3u << 18) | (1u << 20) | (3u << 22); WatchAllThreads(d, d + 8); XS_LOG("WATCH  group0 ctrl bytes @%p now watched (все потоки)", (void*)d); } };
            if (i == 2 && val > 0x10000 && g_watchTable != val) { g_watchTable = (uintptr_t)val; r.Dr3 = val + 0x38; r.Dr7 |= (1u << 6) | (1u << 28) | (3u << 30); uint64_t d = 0; ReadProcessMemory(GetCurrentProcess(), (void*)(val + 0x38), &d, 8, nullptr); setData((uintptr_t)d); }
            if (i == 3 && val > 0x10000) setData((uintptr_t)val);
        }
        r.Dr6 = 0; return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (c == EXCEPTION_SINGLE_STEP && g_rearm) { DWORD o; void* a = (void*)g_rearm; VirtualProtect(a, 1, PAGE_EXECUTE_READWRITE, &o); *(unsigned char*)a = 0xCC; VirtualProtect(a, 1, o, &o); g_rearm = 0; return EXCEPTION_CONTINUE_EXECUTION; }
    if (c != EXCEPTION_BREAKPOINT) return EXCEPTION_CONTINUE_SEARCH;
    for (LONG j = 0; j < g_nRet; j++) if (g_ret[j].ret == (uintptr_t)r.Rip && g_ret[j].orig != 0xCC) {
        uintptr_t v = 0; if (g_ret[j].out) ReadProcessMemory(GetCurrentProcess(), (void*)g_ret[j].out, &v, 8, nullptr);
        XS_LOG("TRAP RET %s -> hr=0x%08lX *out=%p", g_ret[j].what, (unsigned long)(uint32_t)r.Rax, (void*)v);
        DWORD o; void* a = (void*)g_ret[j].ret; VirtualProtect(a, 1, PAGE_EXECUTE_READWRITE, &o); *(unsigned char*)a = g_ret[j].orig; VirtualProtect(a, 1, o, &o); g_ret[j].ret = 0; return EXCEPTION_CONTINUE_EXECUTION;
    }
    for (int i = 0; i < g_nTraps; i++) if (g_traps[i].addr == (uintptr_t)r.Rip) {
        LONG n = InterlockedIncrement(&g_trapN);
        if (n <= 400) {
            uintptr_t ra = 0; ReadProcessMemory(GetCurrentProcess(), (void*)r.Rsp, &ra, 8, nullptr); char who[200]; CallerStr((void*)ra, who, sizeof who);
            XS_LOG("TRAP #%ld rva=%#llx tid=%lu rax=%p rcx=%p rdx=%p r8=%p r9=%p ret=%s", n, (unsigned long long)(r.Rip - (uintptr_t)GetModuleHandleA(nullptr)), GetCurrentThreadId(), (void*)r.Rax, (void*)r.Rcx, (void*)r.Rdx, (void*)r.R8, (void*)r.R9, who);
            PeekStr("rcx", r.Rcx); PeekStr("rdx", r.Rdx); PeekStr("r8", r.R8);
            if ((uintptr_t)r.Rip == (uintptr_t)GetModuleHandleA(nullptr) + 0x2757ec0 || (uintptr_t)r.Rip == (uintptr_t)GetModuleHandleA(nullptr) + 0x2758210) {
                uintptr_t it = r.Rcx; uint32_t f[8] = {}; ReadProcessMemory(GetCurrentProcess(), (void*)(it + 0x1d8), f, sizeof f, nullptr); uintptr_t p98 = 0; ReadProcessMemory(GetCurrentProcess(), (void*)(it + 0x98), &p98, 8, nullptr);
                XS_LOG("TRAP   item=%p mode[1d8]=%u [1dc]=%u state[1e0]=%u [1e4]=%u [98]=%p", (void*)it, f[0], f[1], f[2], f[3], (void*)p98); { uint32_t fl = 0; ReadProcessMemory(GetCurrentProcess(), (void*)(it + 0x1f8), &fl, 4, nullptr); XS_LOG("TRAP   flags[1f8]=%#x", fl); }
                for (int off = 0; off < 0x200; off += 0x10) { char tag[32]; uintptr_t pp = 0; ReadProcessMemory(GetCurrentProcess(), (void*)(it + off), &pp, 8, nullptr); snprintf(tag, sizeof tag, "item[%#x]->", off); PeekStr(tag, pp); snprintf(tag, sizeof tag, "item+%#x", off); PeekStr(tag, it + off); }
            }
            if ((uintptr_t)r.Rip == (uintptr_t)GetModuleHandleA(nullptr) + 0x48a50 && r.Rcx > 0x10000) {
                uintptr_t vt = 0, fn = 0; ReadProcessMemory(GetCurrentProcess(), (void*)r.Rcx, &vt, 8, nullptr); ReadProcessMemory(GetCurrentProcess(), (void*)(vt + 0x98), &fn, 8, nullptr);
                char a1[160], a2[160]; CallerStr((void*)vt, a1, sizeof a1); CallerStr((void*)fn, a2, sizeof a2); XS_LOG("TRAP   GetSize device=%p vt=%s fn=%s handle=%p", (void*)r.Rcx, a1, a2, (void*)r.Rdx);
            }
            { uintptr_t b0 = (uintptr_t)GetModuleHandleA(nullptr), rip = r.Rip;
              if (rip == b0 + 0x13316c7 || rip == b0 + 0x13316ea || rip == b0 + 0x1331696) { uint32_t m[8] = {}; if (r.Rax > 0x10000) ReadProcessMemory(GetCurrentProcess(), (void*)r.Rax, m, sizeof m, nullptr);
                XS_LOG("TRAP   RPF size-attr kind=%s meta=%08x %08x %08x %08x %08x %08x", rip == b0 + 0x1331696 ? "res(virt+phys)" : rip == b0 + 0x13316c7 ? "B(+0x10,44b)" : "C(+0x8,48b)", m[0], m[1], m[2], m[3], m[4], m[5]); } }
            if ((uintptr_t)r.Rip == (uintptr_t)GetModuleHandleA(nullptr) + 0x547941d && r.R8 == 0) {
                static std::atomic<int> k{0};
                if (k++ < 6) { XS_LOG("NULL PROGRAM effect=%p ctx=%p index=%llu", (void*)r.Rcx, (void*)r.Rdx, (unsigned long long)r.Rax);
                    for (int off = 0; off < 0xd00; off += 8) { uintptr_t pp = 0; ReadProcessMemory(GetCurrentProcess(), (void*)(r.Rcx + off), &pp, 8, nullptr); char tag[40]; snprintf(tag, sizeof tag, "effect[%#x]->", off); PeekStr(tag, pp); snprintf(tag, sizeof tag, "effect+%#x", off); if (off % 16 == 0) PeekStr(tag, r.Rcx + off); }
                    uint64_t arr = 0; ReadProcessMemory(GetCurrentProcess(), (void*)(r.Rcx + 0xc78), &arr, 8, nullptr); uint64_t pr[16] = {}; ReadProcessMemory(GetCurrentProcess(), (void*)arr, pr, sizeof pr, nullptr);
                    XS_LOG("NULL PROGRAM programs[0..7]=%p %p %p %p %p %p %p %p", (void*)pr[0], (void*)pr[1], (void*)pr[2], (void*)pr[3], (void*)pr[4], (void*)pr[5], (void*)pr[6], (void*)pr[7]); }
            }
            if ((uintptr_t)r.Rip == (uintptr_t)GetModuleHandleA(nullptr) + 0xfa09f0) {
                unsigned char b[48] = {}; SIZE_T u = 0; ReadProcessMemory(GetCurrentProcess(), (void*)r.Rdx, b, sizeof b, &u); char h[100] = {}; for (int q = 0; q < 32; q++) sprintf(h + q * 2, "%02x", b[q]);
                MEMORY_BASIC_INFORMATION mbi{}; VirtualQuery((void*)r.Rdx, &mbi, sizeof mbi);
                XS_LOG("TRAP   rootsig-src blob=%p head=%s (got=%zu) mem: base=%p type=%#lx state=%#lx", (void*)r.Rdx, h, (size_t)u, mbi.AllocationBase, mbi.Type, mbi.State);
            }
            { static FARPROC rf = GetProcAddress(GetModuleHandleA("kernelbase.dll"), "ReadFile");
              if (rf && (uintptr_t)r.Rip == (uintptr_t)rf) { uintptr_t ov = 0; ReadProcessMemory(GetCurrentProcess(), (void*)(r.Rsp + 0x28), &ov, 8, nullptr); uint64_t off = ~0ull;
                if (ov) { DWORD lo = 0, hi = 0; ReadProcessMemory(GetCurrentProcess(), (void*)(ov + 0x10), &lo, 4, nullptr); ReadProcessMemory(GetCurrentProcess(), (void*)(ov + 0x14), &hi, 4, nullptr); off = ((uint64_t)hi << 32) | lo; }
                wchar_t nm[300] = {}; GetFinalPathNameByHandleW((HANDLE)r.Rcx, nm, 299, 0); uintptr_t ra = 0; ReadProcessMemory(GetCurrentProcess(), (void*)r.Rsp, &ra, 8, nullptr); char who[120]; CallerStr((void*)ra, who, sizeof who);
                static std::atomic<int> k{0}; if (k++ < 3000) XS_LOG("READFILE %ls off=%#llx size=%#llx buf=%p ov=%p caller=%s", nm, (unsigned long long)off, (unsigned long long)r.R8, (void*)r.Rdx, (void*)ov, who); } }
            bool atLoad = (uintptr_t)r.Rip == (uintptr_t)GetModuleHandleA(nullptr) + 0x19f7d26;   // после чтения потока целиком: r15=буфер, edi=размер (GetSize)
            if (((uintptr_t)r.Rip == (uintptr_t)GetModuleHandleA(nullptr) + 0x407e700 && r.Rdx > 0x10000) || atLoad) {
                if (atLoad) { r.Rdx = r.Rdx; }
                uintptr_t blobp = atLoad ? r.R15 : r.Rdx; uint32_t sz = atLoad ? (uint32_t)r.Rdi : (uint32_t)r.R8; std::vector<unsigned char> all(sz < 0x400000 ? sz : 0x400000); SIZE_T u2 = 0; ReadProcessMemory(GetCurrentProcess(), (void*)blobp, all.data(), all.size(), &u2);
                uint32_t off = 0, total = 0; char chain[256] = {}; int cp = 0; bool trunc = false;
                while (off + 8 <= u2) { uint32_t csz = _byteswap_ulong(*(uint32_t*)&all[off + 4]); cp += snprintf(chain + cp, sizeof chain - cp, "%.4s:%x ", (char*)&all[off], csz); if (cp > 200) break; if (csz < 8) break; if (off + csz > sz) { trunc = true; total = off + csz; break; } off += csz; }
                static std::atomic<int> nd{0};
                if (trunc && u2 >= 4 && !memcmp(all.data(), "PSIN", 4)) { int k = nd++; uintptr_t ra = 0; ReadProcessMemory(GetCurrentProcess(), (void*)r.Rsp, &ra, 8, nullptr); char who[160]; CallerStr((void*)ra, who, sizeof who);
                    XS_LOG("PSO TRUNCATED%s blob=%p size=%#x need>=%#x chain=%s caller=%s", atLoad ? "(load)" : "", (void*)blobp, sz, total, chain, who);
                    for (int q = 1; q < 48; q++) { uintptr_t v = 0; ReadProcessMemory(GetCurrentProcess(), (void*)(r.Rsp + q * 8), &v, 8, nullptr); uintptr_t b0 = (uintptr_t)GetModuleHandleA(nullptr); if (v > b0 + 0x1000 && v < b0 + 0x6000000) XS_LOG("PSO    stk+%#x exe+%#llx", q * 8, (unsigned long long)(v - b0)); }
                    if (k < 6) { char path[64]; snprintf(path, sizeof path, "T:\\psotrunc_%d.bin", k); HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, 0, 0); DWORD wr; if (f != INVALID_HANDLE_VALUE) { WriteFile(f, all.data(), (DWORD)u2, &wr, 0); CloseHandle(f); } } }
            }
            if ((uintptr_t)r.Rip == (uintptr_t)GetModuleHandleA(nullptr) + 0x2756d90) {
                uintptr_t mgr = 0; ReadProcessMemory(GetCurrentProcess(), (void*)((uintptr_t)GetModuleHandleA(nullptr) + 0x99aa6c0), &mgr, 8, nullptr);
                uint32_t cnt = 0; uintptr_t arr = 0; if (mgr) { ReadProcessMemory(GetCurrentProcess(), (void*)(mgr + 0xc), &cnt, 4, nullptr); ReadProcessMemory(GetCurrentProcess(), (void*)(mgr + 0x10), &arr, 8, nullptr); }
                XS_LOG("TRAP   pkgmgr=%p items=%u arr=%p want_id=%08x", (void*)mgr, cnt, (void*)arr, (unsigned)r.Rdx);
                for (uint32_t it = 0; arr && it < cnt && it < 24; it++) { uint32_t id = 0; ReadProcessMemory(GetCurrentProcess(), (void*)(arr + it * 0x200 + 0x140), &id, 4, nullptr);
                    XS_LOG("TRAP   item[%u] id=%08x", it, id); for (int off = 0; off < 0x200; off += 0x10) { char tag[32]; snprintf(tag, sizeof tag, "  item+%#x", off); PeekStr(tag, arr + it * 0x200 + off); uintptr_t p2 = 0; ReadProcessMemory(GetCurrentProcess(), (void*)(arr + it * 0x200 + off), &p2, 8, nullptr); snprintf(tag, sizeof tag, "  [item+%#x]", off); PeekStr(tag, p2); } }
            }
            if ((uintptr_t)r.Rip == (uintptr_t)GetModuleHandleA(nullptr) + 0x1d4c7e0 && r.Rdx > 0x10000) {
                for (int off = 0x60; off <= 0x90; off += 0x10) { unsigned char sso[16] = {}; ReadProcessMemory(GetCurrentProcess(), (void*)(r.Rdx + off), sso, 16, nullptr);
                    uintptr_t ptr = *(uintptr_t*)sso; char tag[32]; snprintf(tag, sizeof tag, "def+%#x(ptr)", off); PeekStr(tag, ptr); snprintf(tag, sizeof tag, "def+%#x(inline)", off); PeekStr(tag, r.Rdx + off); }
                for (int off = 8; off < 0x140; off += 8) { uintptr_t ptr = 0; ReadProcessMemory(GetCurrentProcess(), (void*)(r.Rdx + off), &ptr, 8, nullptr); char tag[32]; snprintf(tag, sizeof tag, "def[%#x]->", off); PeekStr(tag, ptr); snprintf(tag, sizeof tag, "def+%#x", off); PeekStr(tag, r.Rdx + off); }
                uint32_t raw[0x30] = {}; ReadProcessMemory(GetCurrentProcess(), (void*)r.Rdx, raw, sizeof raw, nullptr); char h[600] = {}; for (int q = 0; q < 0x30; q++) sprintf(h + q * 9, "%08x ", raw[q]); XS_LOG("TRAP   def raw: %s", h);
            }
            { uint32_t st[8] = {}; ReadProcessMemory(GetCurrentProcess(), (void*)(r.Rsp + 0x20), st, sizeof st, nullptr); XS_LOG("TRAP   rbx=%p rsi=%p rdi=%p r12=%p r13=%p r14=%p r15=%p [rsp+20..3c]=%x %x %x %x %x %x %x %x", (void*)r.Rbx, (void*)r.Rsi, (void*)r.Rdi, (void*)r.R12, (void*)r.R13, (void*)r.R14, (void*)r.R15, st[0], st[1], st[2], st[3], st[4], st[5], st[6], st[7]); }
            if ((uintptr_t)r.Rip == (uintptr_t)GetModuleHandleA(nullptr) + 0x1d4f770 && r.Rdx > 0x10000) {
                uint32_t cnt = 0; uintptr_t data = 0; ReadProcessMemory(GetCurrentProcess(), (void*)(r.Rdx + 0x30), &cnt, 4, nullptr); ReadProcessMemory(GetCurrentProcess(), (void*)(r.Rdx + 0x38), &data, 8, nullptr);
                unsigned char g[0x30] = {}; if (data) ReadProcessMemory(GetCurrentProcess(), (void*)data, g, sizeof g, nullptr); char h[160] = {}; for (int q = 0; q < 0x30; q++) sprintf(h + q * 2, "%02x", g[q]);
                XS_LOG("TRAP   table=%p groups=%u data=%p head=%s", (void*)r.Rdx, cnt, (void*)data, h);
                for (uint32_t gi = 0; data && gi < cnt && gi < 4; gi++) { unsigned char gg[0x110] = {}; ReadProcessMemory(GetCurrentProcess(), (void*)(data + gi * 0x110), gg, sizeof gg, nullptr);
                    char line[600] = {}; int pos = 0; for (int sl = 0; sl < 16; sl++) if (!(gg[sl] & 0x80)) { uint32_t key = *(uint32_t*)(gg + 0x10 + sl * 16); uint64_t val = *(uint64_t*)(gg + 0x10 + sl * 16 + 8); pos += snprintf(line + pos, sizeof line - pos, " [%d]%08x->%llx", sl, key, (unsigned long long)val); }
                    XS_LOG("TRAP   group%u:%s", gi, line); }
            }
            uint32_t k = 0; if (r.R8 > 0x10000 && ReadProcessMemory(GetCurrentProcess(), (void*)r.R8, &k, 4, nullptr)) XS_LOG("TRAP   *r8=%08x", k);
        }
        if ((uintptr_t)r.Rip == (uintptr_t)GetModuleHandleA(nullptr) + 0x5cb532b) {   // эффект "im" получен из реестра: watch на его массив программ
            uintptr_t eff = r.Rax, arr = 0; uint16_t cnt = 0; ReadProcessMemory(GetCurrentProcess(), (void*)(eff + 0xc78), &arr, 8, nullptr); ReadProcessMemory(GetCurrentProcess(), (void*)(eff + 0xcf8), &cnt, 2, nullptr);
            uint64_t pr[48] = {}; if (arr) ReadProcessMemory(GetCurrentProcess(), (void*)arr, pr, sizeof pr, nullptr); int nz = 0; for (int i = 0; i < 48; i++) nz += pr[i] != 0;
            XS_LOG("TRAP effect 'im'=%p programs=%p nonnull(0..47)=%d prog[42]=%p [cf8]=%u", (void*)eff, (void*)arr, nz, (void*)pr[42], cnt);
            if (arr) { r.Dr0 = arr + 42 * 8; r.Dr1 = arr; r.Dr7 |= (1u << 0) | (1u << 2) | (1u << 16) | (3u << 18) | (1u << 20) | (3u << 22); r.ContextFlags |= CONTEXT_DEBUG_REGISTERS; WatchAllThreads(arr + 42 * 8, arr); }
        }
        if ((uintptr_t)r.Rip == (uintptr_t)GetModuleHandleA(nullptr) + 0x122888 && !(r.Dr7 & 1)) {   // первый bind программы в контекст: watch на ctx+0x12328
            r.Dr0 = r.Rsi + 0x12328; r.Dr7 |= (1u << 0) | (1u << 16) | (3u << 18); r.ContextFlags |= CONTEXT_DEBUG_REGISTERS;
            XS_LOG("TRAP ctx=%p: HW-watch на ctx+0x12328 (программа) в потоке %lu", (void*)r.Rsi, GetCurrentThreadId());
        }
        if ((uintptr_t)r.Rip == (uintptr_t)GetModuleHandleA(nullptr) + 0x5b1c385) {   // после записи g[0x1499aa6c0]=rdi (менеджер контента)
            uintptr_t mgr = r.Rdi; r.Dr0 = mgr + 0x8; r.Dr1 = mgr + 0x10; r.Dr7 |= (1u << 0) | (1u << 2) | (1u << 16) | (3u << 18) | (1u << 20) | (3u << 22);
            r.ContextFlags |= CONTEXT_DEBUG_REGISTERS; WatchAllThreads(mgr + 0x8, mgr + 0x10); g_watchTable = 1;
            XS_LOG("TRAP content-manager %p: HW-watch +8/+0x10 во всех потоках", (void*)mgr);
        }
        if ((uintptr_t)r.Rip == (uintptr_t)GetModuleHandleA(nullptr) + 0x2236de1) {
            uintptr_t obj = r.Rsi; r.Dr0 = obj + 0x2a0; r.Dr1 = obj + 0x2a8; r.Dr2 = obj + 0x2b0;
            r.Dr7 |= (1u << 0) | (1u << 2) | (1u << 4) | (1u << 16) | (3u << 18) | (1u << 20) | (3u << 22) | (1u << 24) | (3u << 26);
            r.ContextFlags |= CONTEXT_DEBUG_REGISTERS; XS_LOG("TRAP obj created %p: HW-watch on +2a0/+2a8/+2b0 (tid=%lu)", (void*)obj, GetCurrentThreadId());
        }
        if (g_traps[i].wantRet) {
            uintptr_t ra = 0; ReadProcessMemory(GetCurrentProcess(), (void*)r.Rsp, &ra, 8, nullptr);
            LONG j = InterlockedIncrement(&g_nRet) - 1; if (j < 64) { g_ret[j].ret = ra; g_ret[j].out = 0; snprintf(g_ret[j].what, sizeof g_ret[j].what, "rva %#llx", (unsigned long long)(r.Rip - (uintptr_t)GetModuleHandleA(nullptr))); DWORD o2; VirtualProtect((void*)ra, 1, PAGE_EXECUTE_READWRITE, &o2); g_ret[j].orig = *(unsigned char*)ra; *(unsigned char*)ra = 0xCC; VirtualProtect((void*)ra, 1, o2, &o2); }
        }
        { static FARPROC qai = GetProcAddress(GetModuleHandleA("xgameruntime.dll"), "QueryApiImpl");
          if (qai && (uintptr_t)r.Rip == (uintptr_t)qai) {
            uintptr_t ra = 0; ReadProcessMemory(GetCurrentProcess(), (void*)r.Rsp, &ra, 8, nullptr); char g1[48], g2[48]; FmtGuid(r.Rcx, g1, sizeof g1); FmtGuid(r.Rdx, g2, sizeof g2);
            char who[160]; CallerStr((void*)ra, who, sizeof who); XS_LOG("TRAP QueryApiImpl provider=%s iid=%s caller=%s", g1, g2, who);
            LONG j = InterlockedIncrement(&g_nRet) - 1; if (j < 64) { g_ret[j].ret = ra; g_ret[j].out = r.R8; snprintf(g_ret[j].what, sizeof g_ret[j].what, "QueryApiImpl %.8s/%.8s", g1, g2); DWORD o2; VirtualProtect((void*)ra, 1, PAGE_EXECUTE_READWRITE, &o2); g_ret[j].orig = *(unsigned char*)ra; *(unsigned char*)ra = 0xCC; VirtualProtect((void*)ra, 1, o2, &o2); }
          } }
        DWORD o; void* a = (void*)g_traps[i].addr; VirtualProtect(a, 1, PAGE_EXECUTE_READWRITE, &o); *(unsigned char*)a = g_traps[i].orig; VirtualProtect(a, 1, o, &o);
        g_rearm = g_traps[i].addr; r.EFlags |= 0x100; return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
static void InstallTraps()
{
    char env[512]; if (!GetEnvironmentVariableA("XSX_TRAP", env, sizeof env)) return;
    uintptr_t base = (uintptr_t)GetModuleHandleA(nullptr); AddVectoredExceptionHandler(1, &TrapHandler);
    for (char* t = strtok(env, ","); t && g_nTraps < 16; t = strtok(nullptr, ",")) {
        uintptr_t a = 0; DWORD o;
        if (char* bang = strchr(t, '!')) { *bang = 0; HMODULE m = LoadLibraryA(t); a = m ? (uintptr_t)GetProcAddress(m, bang + 1) : 0; *bang = '!'; if (!a) { XS_LOG("TRAP %s: not resolved", t); continue; } }
        else a = base + strtoull(t, nullptr, 16);
        bool wantRet = t[strlen(t) - 1] == 'r'; if (!VirtualProtect((void*)a, 1, PAGE_EXECUTE_READWRITE, &o)) continue;
        g_traps[g_nTraps].addr = a; g_traps[g_nTraps].wantRet = wantRet; g_traps[g_nTraps].orig = *(unsigned char*)a; *(unsigned char*)a = 0xCC; VirtualProtect((void*)a, 1, o, &o); g_nTraps++;
        XS_LOG("TRAP armed rva=%s", t);
    }
}
static volatile LONG g_deviceCreated = 0;
static volatile LONG g_vmCalls = 0;   // для проверки порядка Configure -> CreateDevice

static HMODULE Backend()
{
    static HMODULE m = LoadLibraryW(L"d3d12.dll");
    return m;
}

template <typename Fn>
static Fn Resolve(const char* name)
{
    HMODULE b = Backend();
    if (!b) return nullptr;
    return reinterpret_cast<Fn>(GetProcAddress(b, name));
}

// ---- Root signature deserializer (ранее реализованный, поведение сохранено) ----
using DeserializeFn = HRESULT (WINAPI*)(LPCVOID, SIZE_T, REFIID, void**);

extern "C" HRESULT WINAPI xsCreateRootSignatureDeserializer(
    LPCVOID data, SIZE_T size, REFIID iid, void** result)
{
    XS_LOG("Deserialize: data=%p size=%llu out=%p", data, (unsigned long long)size, result);
    if (!Backend()) {
        XS_LOG("Cannot load backend: %lu", GetLastError());
        return HRESULT_FROM_WIN32(GetLastError());
    }
    static auto fn = Resolve<DeserializeFn>("D3D12CreateRootSignatureDeserializer");
    if (!fn) return E_NOTIMPL;
    HRESULT hr = fn(data, size, iid, result);
    XS_LOG("Backend returned 0x%08lX", (unsigned long)hr);
    return hr;
}

// ---- D3D12SerializeVersionedRootSignature: сигнатура идентична PC ----
using SerializeVerFn = HRESULT (WINAPI*)(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC*, ID3DBlob**, ID3DBlob**);

extern "C" HRESULT WINAPI xsSerializeVersionedRootSignature(
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* desc, ID3DBlob** blob, ID3DBlob** error)
{
    CALLER(c);
    static auto fn = Resolve<SerializeVerFn>("D3D12SerializeVersionedRootSignature");
    if (!fn) { XS_LOG("SerializeVersionedRootSignature: backend missing"); return E_NOTIMPL; }
    HRESULT hr = fn(desc, blob, error);
    { static std::atomic<int> k{0}; if (SUCCEEDED(hr) && blob && *blob && k++ < 60) { auto* b = *blob; const unsigned char* q = (const unsigned char*)b->GetBufferPointer(); XS_LOG("  rootsig blob size=%zu head=%02x%02x%02x%02x %02x%02x%02x%02x", (size_t)b->GetBufferSize(), q[0],q[1],q[2],q[3],q[4],q[5],q[6],q[7]); } }
    XS_LOG("SerializeVersionedRootSignature ver=%d caller=%s -> 0x%08lX",
           desc ? (int)desc->Version : -1, c, (unsigned long)hr);
    return hr;
}


// ======================= XboxDeviceWrapper =======================
// Игра ожидает Xbox-раскладку vtable device. Возвращаем обёртку (объект в арене: [vtable][real]), vtable на 192 слота:
//  слоты 0..43  — PC ID3D12Device: asm smart-forward (xs_fwd_N) в настоящий device; структуры-возвраты: если RCX не в арене, а RDX в арене —
//                 Xbox-порядок (RCX=буфер, RDX=this) -> переставляем в PC-порядок [ДОКАЗАНО на слоте 26 GetCustomHeapProperties];
//  слот 0 (QI), 43 (GetAdapterLuid, 8 байт -> RAX), 76 (CreateCommittedResource, из FFX-находки: PC-аргументы, флаги ресурса &0x3FFFFF);
//  остальные слоты — заглушка: лог (слот, регистры, caller) и возврат 0 -> карта реально используемых слотов.
#include <atomic>
#include <d3dcompiler.h>
#include <vector>
#include <utility>
#include "d3d12_xs_fwd.inc"
extern "C" { uintptr_t g_arena_lo = 0, g_arena_hi = 0; }
static char* g_arenaTop = nullptr; static char* g_arenaCommitted = nullptr;
struct Wrap { void** vt; void* real; uint64_t kind; uint64_t pad[5]; };   // 64 байта
static constexpr size_t kListObjectBytes = 0x1000; // Observed game stores extend through +0x564.
struct ExclusiveSrwLock {
    SRWLOCK* lock;
    explicit ExclusiveSrwLock(SRWLOCK& value) : lock(&value) { AcquireSRWLockExclusive(lock); }
    ~ExclusiveSrwLock() { ReleaseSRWLockExclusive(lock); }
    ExclusiveSrwLock(const ExclusiveSrwLock&) = delete;
    ExclusiveSrwLock& operator=(const ExclusiveSrwLock&) = delete;
};
static Wrap* NewWrap(void** vt, void* real, uint64_t kind)
{
    // Initialization, commit frontier AND allocation cursor must be serialized.
    // Game worker threads faulted at NewWrap+0x45 writing into an uncommitted
    // page after racing the old unchecked frontier update.
    static SRWLOCK lock = SRWLOCK_INIT;
    ExclusiveSrwLock held(lock);
    const size_t bytes = kind == 3 ? kListObjectBytes : sizeof(Wrap);
    if (!g_arena_lo) {
        char* a = (char*)VirtualAlloc(nullptr, 512u << 20, MEM_RESERVE, PAGE_READWRITE);
        if (!a) return nullptr;
        g_arena_lo = (uintptr_t)a; g_arena_hi = (uintptr_t)a + (512u << 20); g_arenaTop = a; g_arenaCommitted = a;
    }
    if ((uintptr_t)g_arenaTop > g_arena_hi - bytes) return nullptr;
    if (g_arenaTop + bytes > g_arenaCommitted) {
        if (!VirtualAlloc(g_arenaCommitted, 1u << 20, MEM_COMMIT, PAGE_READWRITE)) return nullptr;
        g_arenaCommitted += (1u << 20);
    }
    Wrap* w = (Wrap*)g_arenaTop; g_arenaTop += bytes; memset(w, 0, bytes); w->vt = vt; w->real = real; w->kind = kind; return w;
}
// Xbox code accesses command-list fields directly, including +0x18 cursor and
// +0x28 limit. Do not put host ABI tags in those fields: cursor=0, limit=1
// selected the raw-packet fast path and caused the write-null at game+bff9a.
// Keeping both zero uses the game's existing virtual Draw slow path.
static SRWLOCK g_xboxListLock = SRWLOCK_INIT;
static std::vector<Wrap*> g_xboxLists; // Arena addresses are never reused.
static void TagXboxList(Wrap* w)
{
    ExclusiveSrwLock held(g_xboxListLock);
    g_xboxLists.push_back(w);
}
static bool IsXboxList(Wrap* w)
{
    AcquireSRWLockShared(&g_xboxListLock);
    bool found = false;
    for (Wrap* candidate : g_xboxLists) if (candidate == w) { found = true; break; }
    ReleaseSRWLockShared(&g_xboxListLock);
    return found;
}
// Explicit diagnostic mode requested by the user: valid native no-op PSOs,
// real command recording/submission, and counters. It does not run game shaders.
static bool BootstrapNoopEnabled()
{
    char value[4]{};
    return GetEnvironmentVariableA("XSX_DEV_NOOP_PSO", value, sizeof value) == 1 && value[0] == '1';
}
struct BootstrapCounters {
    std::atomic<unsigned> pso{0}, set{0}, barriers{0}, draw{0}, dispatch{0}, execute{0}, present{0};
};
static BootstrapCounters g_bootstrap;
static bool BindBootstrapGraphics(Wrap*);
static bool BindBootstrapCompute(Wrap*);
static HRESULT BootstrapGraphicsOutput(Wrap*, REFIID, void**);
static const int kDevSlots = 192;
static void* g_devVt[kDevSlots];
static std::atomic<int> g_slotCalls[kDevSlots];

template <int N> static uintptr_t __stdcall DevUnknown(void* self, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e, uintptr_t f)
{
    int n = ++g_slotCalls[N];
    if (n <= 12 || (n % 1000) == 0) {
        char who[300]; CallerStr(__builtin_return_address(0), who, sizeof who);
        XS_LOG("DEVICE slot %d (+0x%x) UNMAPPED call#%d rcx=%p rdx=%p r8=%p r9=%p [rsp+28]=%p [rsp+30]=%p caller=%s",
               N, N * 8, n, self, (void*)b, (void*)c, (void*)d, (void*)e, (void*)f, who);
        // дамп данных за указателями-аргументами (диагностика структур; только читаемые адреса)
        const uintptr_t ptrs[3] = { b, c, d };
        for (int k = 0; k < 3; k++) {
            if (ptrs[k] > 0x10000 && ptrs[k] < 0x7fffffff0000ull && !IsBadReadPtr((void*)ptrs[k], 48)) {
                const uint32_t* q = (const uint32_t*)ptrs[k];
                XS_LOG("   arg%d -> %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x", k + 1, q[0], q[1], q[2], q[3], q[4], q[5], q[6], q[7], q[8], q[9], q[10], q[11]);
            }
        }
    }
    return 0;
}
template <int... I> static void FillUnknown(std::integer_sequence<int, I...>) { ((g_devVt[I] = (void*)&DevUnknown<I>), ...); }

static HRESULT __stdcall Dev_QI(Wrap* w, REFIID riid, void** out)
{
    if (!out) return E_POINTER;
    *out = nullptr;
    ID3D12Device* real = (ID3D12Device*)w->real; void* o = nullptr;
    HRESULT hr = real->QueryInterface(riid, &o);
    if (SUCCEEDED(hr) && o == w->real) { *out = w; return hr; } // Keep the reference acquired by QI; Release forwards to real.
    XS_LOG("DEVICE QueryInterface {%08lX-%04X-%04X-..} -> real %p (не обёрнуто) hr=0x%08lX", riid.Data1, riid.Data2, riid.Data3, o, (unsigned long)hr);
    *out = o; return hr;
}
static uint64_t __stdcall Dev_GetAdapterLuid(Wrap* w)        // LUID — 8 байт: Xbox-код ждёт значение в RAX
{
    LUID l = {}; ((void* (*)(void*, LUID*))(*(void***)w->real)[43])(w->real, &l); return *(uint64_t*)&l;
}
static D3D12_RESOURCE_DESC PcResourceDesc(const D3D12_RESOURCE_DESC& xbox)
{
    D3D12_RESOURCE_DESC pc = xbox;
    // Preserve the existing FFX 0xc00000 stripping and also remove observed
    // Xbox 0x300000 allocation bits, which are unknown PC resource flags.
    pc.Flags = (D3D12_RESOURCE_FLAGS)((unsigned)xbox.Flags & 0x0fffff);
    if ((int)pc.Layout > 3) pc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    // Observed game depth target: R32G8X24_TYPELESS, flags=0x80300006.
    // PC D3D12 forbids DEPTH_STENCIL|UNORDERED_ACCESS on the same resource.
    // Allocate depth-only backing consistently in sizing AND creation. This
    // does not emulate Xbox UAV access to the depth surface (needs a shadow).
    if (((unsigned)xbox.Flags & 0x300000) && xbox.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
        xbox.Format == DXGI_FORMAT_R32G8X24_TYPELESS &&
        ((unsigned)pc.Flags & (D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) ==
        (D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) {
        pc.Flags = (D3D12_RESOURCE_FLAGS)((unsigned)pc.Flags & ~D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        static std::atomic<unsigned> calls{0};
        if (calls++ < 8) XS_LOG("RESOURCE depth backing flags=%#x->%#x (Xbox depth UAV unsupported; shadow required)",
            (unsigned)xbox.Flags, (unsigned)pc.Flags);
    }
    return pc;
}
static HRESULT __stdcall Dev_CreateCommittedResource76(Wrap* w, const D3D12_HEAP_PROPERTIES* hp, D3D12_HEAP_FLAGS hf, const D3D12_RESOURCE_DESC* rd,
                                                       D3D12_RESOURCE_STATES st, const D3D12_CLEAR_VALUE* cv, REFIID riid, void** out)
{
    D3D12_RESOURCE_DESC d = PcResourceDesc(*rd);
    HRESULT hr = ((ID3D12Device*)w->real)->CreateCommittedResource(hp, hf, &d, st, cv, riid, out);
    XS_LOG("DEVICE slot76 CreateCommittedResource dim=%d %llux%u fmt=%d flags=0x%x->0x%x state=0x%x -> 0x%08lX res=%p", (int)rd->Dimension, (unsigned long long)rd->Width, rd->Height,
           (int)rd->Format, (unsigned)rd->Flags, (unsigned)d.Flags, (unsigned)st, (unsigned long)hr, out ? *out : nullptr);
    return hr;
}


// ---------- трансляция CPU-handle'ов descriptor heap ----------
// Xbox-дескриптор heap = PC D3D12_DESCRIPTOR_HEAP_DESC (16 байт) + {void* ExpectedCpuBase; size_t Size} [по стеку игры, функция 0x141cf3280].
// Игра создаёт heap в своей памяти и требует GetCPUDescriptorHandleForHeapStart()==ExpectedCpuBase; VKD3D-handle'ы непрозрачны -> диапазоны {guest,size,real}.
struct HRange { uint64_t guest, size, real; };
static HRange g_hr[512]; static std::atomic<int> g_hrN{0};
static inline uint64_t XlatCpu(uint64_t h)
{
    int n = g_hrN.load(std::memory_order_acquire);
    for (int i = 0; i < n; i++) { const HRange& r = g_hr[i]; if (h - r.guest < r.size) return r.real + (h - r.guest); }
    return h;
}
static void __stdcall Dev_CreateCBV(Wrap* w, const D3D12_CONSTANT_BUFFER_VIEW_DESC* d, D3D12_CPU_DESCRIPTOR_HANDLE h) { h.ptr = XlatCpu(h.ptr); ((ID3D12Device*)w->real)->CreateConstantBufferView(d, h); }
static void __stdcall Dev_CreateSRV(Wrap* w, ID3D12Resource* r, const D3D12_SHADER_RESOURCE_VIEW_DESC* d, D3D12_CPU_DESCRIPTOR_HANDLE h) { h.ptr = XlatCpu(h.ptr); ((ID3D12Device*)w->real)->CreateShaderResourceView(r, d, h); }
static void __stdcall Dev_CreateUAV(Wrap* w, ID3D12Resource* r, ID3D12Resource* c, const D3D12_UNORDERED_ACCESS_VIEW_DESC* d, D3D12_CPU_DESCRIPTOR_HANDLE h) { h.ptr = XlatCpu(h.ptr); ((ID3D12Device*)w->real)->CreateUnorderedAccessView(r, c, d, h); }
static void __stdcall Dev_CreateRTV(Wrap* w, ID3D12Resource* r, const D3D12_RENDER_TARGET_VIEW_DESC* d, D3D12_CPU_DESCRIPTOR_HANDLE h) { h.ptr = XlatCpu(h.ptr); ((ID3D12Device*)w->real)->CreateRenderTargetView(r, d, h); }
static void __stdcall Dev_CreateDSV(Wrap* w, ID3D12Resource* r, const D3D12_DEPTH_STENCIL_VIEW_DESC* d, D3D12_CPU_DESCRIPTOR_HANDLE h) { h.ptr = XlatCpu(h.ptr); ((ID3D12Device*)w->real)->CreateDepthStencilView(r, d, h); }
static void __stdcall Dev_CreateSampler(Wrap* w, const D3D12_SAMPLER_DESC* d, D3D12_CPU_DESCRIPTOR_HANDLE h) { h.ptr = XlatCpu(h.ptr); ((ID3D12Device*)w->real)->CreateSampler(d, h); }
static void __stdcall Dev_CopyDescriptors(Wrap* w, UINT nd, const D3D12_CPU_DESCRIPTOR_HANDLE* ds, const UINT* dsz, UINT ns, const D3D12_CPU_DESCRIPTOR_HANDLE* ss, const UINT* ssz, D3D12_DESCRIPTOR_HEAP_TYPE t)
{
    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> d(nd), s(ns); for (UINT i = 0; i < nd; i++) d[i].ptr = XlatCpu(ds[i].ptr); for (UINT i = 0; i < ns; i++) s[i].ptr = XlatCpu(ss[i].ptr);
    ((ID3D12Device*)w->real)->CopyDescriptors(nd, d.data(), dsz, ns, s.data(), ssz, t);
}
static void __stdcall Dev_CopyDescriptorsSimple(Wrap* w, UINT n, D3D12_CPU_DESCRIPTOR_HANDLE d, D3D12_CPU_DESCRIPTOR_HANDLE s, D3D12_DESCRIPTOR_HEAP_TYPE t)
{ d.ptr = XlatCpu(d.ptr); s.ptr = XlatCpu(s.ptr); ((ID3D12Device*)w->real)->CopyDescriptorsSimple(n, d, s, t); }
static void __stdcall List_OMSetRenderTargets(Wrap* w, UINT n, const D3D12_CPU_DESCRIPTOR_HANDLE* rtv, BOOL single, const D3D12_CPU_DESCRIPTOR_HANDLE* dsv)
{
    D3D12_CPU_DESCRIPTOR_HANDLE r[8] = {}, dd = {}; UINT m = n < 8 ? n : 8; for (UINT i = 0; i < m && rtv; i++) r[i].ptr = XlatCpu(rtv[i].ptr); if (dsv) dd.ptr = XlatCpu(dsv->ptr);
    ((ID3D12GraphicsCommandList*)w->real)->OMSetRenderTargets(n, rtv ? r : nullptr, single, dsv ? &dd : nullptr);
}
static void __stdcall ListSlot47(Wrap* w, D3D12_CPU_DESCRIPTOR_HANDLE h, D3D12_CLEAR_FLAGS f, FLOAT d, UINT8 st, UINT nr, const D3D12_RECT* rc)
{
    auto* list = (ID3D12GraphicsCommandList*)w->real;
    if (IsXboxList(w)) {
        // Game RVA0x328b8: RDX points to {GPU VA, size, R16/R32_UINT},
        // or is null to unbind. This is IASetIndexBuffer, not a DSV handle.
        static std::atomic<unsigned> calls{0};
        if (calls++ < 12) { CALLER(c); XS_LOG("IA ABI Xbox list47 index_view=%p caller=%s", (void*)h.ptr, c); }
        list->IASetIndexBuffer((const D3D12_INDEX_BUFFER_VIEW*)h.ptr);
        return;
    }
    h.ptr = XlatCpu(h.ptr);
    list->ClearDepthStencilView(h, f, d, st, nr, rc);
}
static void __stdcall ListSlot48(Wrap* w, uintptr_t argument, uintptr_t third, uintptr_t fourth, const D3D12_RECT* rc)
{
    auto* list = (ID3D12GraphicsCommandList*)w->real;
    if (IsXboxList(w)) {
        // Game RVA0x32960: (startSlot=0, viewCount, views). Each 16-byte
        // view is built as {GPU VA, size, stride}; PC IASetVertexBuffers.
        static std::atomic<unsigned> calls{0};
        if (calls++ < 12) { CALLER(c); XS_LOG("IA ABI Xbox list48 start=%u count=%u views=%p caller=%s", (UINT)argument, (UINT)third, (void*)fourth, c); }
        list->IASetVertexBuffers((UINT)argument, (UINT)third, (const D3D12_VERTEX_BUFFER_VIEW*)fourth);
        return;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE h{XlatCpu(argument)};
    list->ClearRenderTargetView(h, (const FLOAT*)third, (UINT)fourth, rc);
}
static void __stdcall List_ClearUAVUint(Wrap* w, D3D12_GPU_DESCRIPTOR_HANDLE g, D3D12_CPU_DESCRIPTOR_HANDLE h, ID3D12Resource* r, const UINT* v, UINT nr, const D3D12_RECT* rc) { h.ptr = XlatCpu(h.ptr); ((ID3D12GraphicsCommandList*)w->real)->ClearUnorderedAccessViewUint(g, h, r, v, nr, rc); }
static void __stdcall List_ClearUAVFloat(Wrap* w, D3D12_GPU_DESCRIPTOR_HANDLE g, D3D12_CPU_DESCRIPTOR_HANDLE h, ID3D12Resource* r, const FLOAT* v, UINT nr, const D3D12_RECT* rc) { h.ptr = XlatCpu(h.ptr); ((ID3D12GraphicsCommandList*)w->real)->ClearUnorderedAccessViewFloat(g, h, r, v, nr, rc); }
// ---------- обёртки остальных объектов (queue=2, list=3, heap=4) ----------
static Wrap* g_devWrap = nullptr;
static inline void* Unwrap(void* p) { uintptr_t u = (uintptr_t)p; return (u >= g_arena_lo && u < g_arena_hi) ? ((Wrap*)p)->real : p; }
static const int kObjSlots = 192; static const char* kKindName[] = { "?", "device", "queue", "list", "heap", "pso", "allocator", "fence" };
static void* g_vtQueue[kObjSlots], *g_vtList[kObjSlots], *g_vtHeap[kObjSlots], *g_vtPso[kObjSlots], *g_vtAlloc[kObjSlots], *g_vtFence[kObjSlots];
static void* g_originalRootSlots[kObjSlots];
template<int N> static void __stdcall BootstrapRootSlot(Wrap* w, uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d)
{
    if (BootstrapNoopEnabled() && IsXboxList(w)) {
        // Diagnostic shaders have an empty root layout. Suppress guest root
        // writes consistently, including after Draw/Dispatch bound that layout.
        static std::atomic<unsigned> calls{0};
        if (!calls++) XS_LOG("BOOTSTRAP Xbox root slot%d suppressed (synthetic shader has no bindings)", N);
        return;
    }
    ((void (__stdcall*)(Wrap*, uintptr_t, uintptr_t, uintptr_t, uintptr_t))g_originalRootSlots[N])(w, a, b, c, d);
}
template<int... I> static void InstallBootstrapRootSlots(std::integer_sequence<int, I...>)
{
    ((g_originalRootSlots[I+33] = g_vtList[I+33], g_vtList[I+33] = (void*)&BootstrapRootSlot<I+33>), ...);
}
static std::atomic<int> g_objCalls[8][kObjSlots];
template <int K, int N> static uintptr_t __stdcall ObjUnknown(void* self, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e, uintptr_t f)
{
    int n = ++g_objCalls[K][N];
    if (n <= 6 || (n % 5000) == 0) {
        char who[300]; CallerStr(__builtin_return_address(0), who, sizeof who);
        XS_LOG("%s slot %d (+0x%x) UNMAPPED call#%d rcx=%p rdx=%p r8=%p r9=%p [rsp+28]=%p caller=%s", kKindName[K], N, N * 8, n, self, (void*)b, (void*)c, (void*)d, (void*)e, who);
    }
    return 0;
}
template <int K, int... I> static void FillObj(void** vt, std::integer_sequence<int, I...>) { ((vt[I] = (void*)&ObjUnknown<K, I>), ...); }
static HRESULT __stdcall Obj_QI(Wrap* w, REFIID riid, void** out)
{
    if (!out) return E_POINTER;
    *out = nullptr;
    IUnknown* real = (IUnknown*)w->real; void* o = nullptr; HRESULT hr = real->QueryInterface(riid, &o);
    if (SUCCEEDED(hr) && o == w->real) { *out = w; return hr; }
    XS_LOG("%s QueryInterface {%08lX-%04X-%04X-..} -> hr=0x%08lX (не обёрнуто)", kKindName[w->kind], riid.Data1, riid.Data2, riid.Data3, (unsigned long)hr);
    *out = o; return hr;
}
static HRESULT __stdcall Obj_GetDevice(Wrap* w, REFIID riid, void** out) { return g_devWrap ? ((HRESULT (__stdcall*)(Wrap*, REFIID, void**))g_devWrap->vt[0])(g_devWrap, riid, out) : E_FAIL; }
static HRESULT WrapOutput(HRESULT hr, void** out, void** vt, int kind)
{
    if (FAILED(hr)) return hr;
    if (!out || !*out) return E_FAIL;
    void* real = *out;
    *out = NewWrap(vt, real, kind);
    if (*out) { if (kind == 7) ((Wrap*)*out)->pad[0] = 1; return hr; }
    ((IUnknown*)real)->Release(); // Do not leak the backend's owned reference.
    return E_OUTOFMEMORY;
}
#include "fence_mirror.h"
static HRESULT __stdcall QueueSignal14(Wrap* w, ID3D12Fence* fence, UINT64 value)
{
    return ((ID3D12CommandQueue*)w->real)->Signal((ID3D12Fence*)Unwrap(fence), value);
}
static HRESULT __stdcall QueueWait15(Wrap* w, ID3D12Fence* fence, UINT64 value)
{
    return ((ID3D12CommandQueue*)w->real)->Wait((ID3D12Fence*)Unwrap(fence), value);
}
// Game RVA0x2482608: queue16(fence, value) is Xbox Signal. PC queue16
// instead writes a timestamp frequency through its first argument. The old
// forwarder overwrote the fence's vtable with 100000000 (0x5f5e100).
static HRESULT __stdcall QueueSlot16(Wrap* w, void* argument, UINT64 value)
{
    const uintptr_t p = (uintptr_t)argument;
    if (p >= g_arena_lo && p < g_arena_hi && !((p - g_arena_lo) % sizeof(Wrap)) &&
        ((Wrap*)argument)->kind == 7) {
        static std::atomic<unsigned> calls{0};
        if (calls++ < 12) {
            CALLER(c);
            XS_LOG("queue slot16 Xbox Signal fence=%p value=%llu caller=%s", argument, (unsigned long long)value, c);
        }
        return QueueSignal14(w, (ID3D12Fence*)argument, value);
    }
    // Keep the PC bootstrap's real GetTimestampFrequency contract intact.
    return ((ID3D12CommandQueue*)w->real)->GetTimestampFrequency((UINT64*)argument);
}
static void __stdcall Queue_ExecuteCommandLists(Wrap* w, UINT n, ID3D12CommandList* const* lists)
{
    std::vector<ID3D12CommandList*> r(n); for (UINT i = 0; i < n; i++) r[i] = (ID3D12CommandList*)Unwrap(lists[i]);
    ((ID3D12CommandQueue*)w->real)->ExecuteCommandLists(n, r.data());
    if (BootstrapNoopEnabled()) ++g_bootstrap.execute;
}
static HRESULT __stdcall QueuePresent32(Wrap*, UINT planes, const void*, const void*)
{
    const unsigned count = ++g_bootstrap.present;
    if (count <= 8 || !(count % 1000)) {
        CALLER(c);
        XS_LOG("BOOTSTRAP PresentX attempt=%u planes=%u pso=%u set=%u barriers=%u draw=%u dispatch=%u execute=%u native_present=0 noop=%u caller=%s",
            count, planes, g_bootstrap.pso.load(), g_bootstrap.set.load(), g_bootstrap.barriers.load(),
            g_bootstrap.draw.load(), g_bootstrap.dispatch.load(), g_bootstrap.execute.load(), BootstrapNoopEnabled(), c);
    }
    return S_OK; // Same unsupported PresentX behavior; do not claim presentation.
}
// Game RVA0x24841a6 consumes an output UINT64 as a GPU timestamp frequency.
// Xbox queue slot18 differs from PC slot18 (GetDesc); call the typed PC API.
static HRESULT __stdcall Queue_TimestampFrequency18(Wrap* w, UINT64* frequency)
{
    if (!frequency) return E_POINTER;
    *frequency = 0;
    const HRESULT hr = ((ID3D12CommandQueue*)w->real)->GetTimestampFrequency(frequency);
    XS_LOG("queue slot18 GetTimestampFrequency hr=0x%08lX frequency=%llu", (unsigned long)hr, (unsigned long long)*frequency);
    return hr;
}
static void TraceListQuery(int slot, void* caller, ID3D12QueryHeap* heap, D3D12_QUERY_TYPE type, UINT index)
{
    static std::atomic<unsigned> calls{0}; if (calls++ >= 24) return;
    char who[300]; CallerStr(caller, who, sizeof who);
    void* vt = nullptr; SIZE_T used = 0;
    if (heap) ReadProcessMemory(GetCurrentProcess(), heap, &vt, sizeof vt, &used);
    XS_LOG("LIST QUERY slot%d heap=%p vtable=%p type=%u index=%u caller=%s", slot, (void*)heap, vt, (unsigned)type, index, who);
}
static void __stdcall List_Slot52(Wrap* w, void* first, uintptr_t second, uintptr_t third, uintptr_t fourth)
{
    if (IsXboxList(w)) {
        // Proven incompatible call: game RVA0x50a62d8 passes two data pointers,
        // not (query heap, query type, index). Its Xbox operation is unresolved.
        // Do not send raw descriptor bytes into VKD3D's query-heap assertion.
        static std::atomic<unsigned> n{0};
        if (n++ < 12) {
            CALLER(c);
            XS_LOG("UNSUPPORTED Xbox list slot52 a=%p b=%p c=%p d=%p caller=%s (skipped, NOT BeginQuery)",
                first, (void*)second, (void*)third, (void*)fourth, c);
        }
        return;
    }
    const auto type = (D3D12_QUERY_TYPE)second;
    TraceListQuery(52, __builtin_return_address(0), (ID3D12QueryHeap*)first, type, (UINT)third);
    ((ID3D12GraphicsCommandList*)w->real)->BeginQuery((ID3D12QueryHeap*)first, type, (UINT)third);
}
static void __stdcall List_EndQuery53(Wrap* w, ID3D12QueryHeap* heap, D3D12_QUERY_TYPE type, UINT index)
{
    TraceListQuery(53, __builtin_return_address(0), heap, type, index);
    ((ID3D12GraphicsCommandList*)w->real)->EndQuery(heap, type, index);
}
static void __stdcall List_ResolveQuery54(Wrap* w, ID3D12QueryHeap* heap, D3D12_QUERY_TYPE type, UINT start, UINT count, ID3D12Resource* dst, UINT64 offset)
{
    TraceListQuery(54, __builtin_return_address(0), heap, type, start);
    ((ID3D12GraphicsCommandList*)w->real)->ResolveQueryData(heap, type, start, count, dst, offset);
}
static void __stdcall List_SetDescriptorHeaps(Wrap* w, UINT n, ID3D12DescriptorHeap* const* heaps)
{
    ID3D12DescriptorHeap* r[8] = {}; for (UINT i = 0; i < n && i < 8; i++) r[i] = (ID3D12DescriptorHeap*)Unwrap(heaps[i]);
    ((ID3D12GraphicsCommandList*)w->real)->SetDescriptorHeaps(n < 8 ? n : 8, r);
}
static void __stdcall List_SetPipelineState(Wrap* w, ID3D12PipelineState* p)
{
    if (BootstrapNoopEnabled() && IsXboxList(w)) { if (BindBootstrapGraphics(w)) ++g_bootstrap.set; return; }
    ((ID3D12GraphicsCommandList*)w->real)->SetPipelineState((ID3D12PipelineState*)Unwrap(p));
}
static void __stdcall ListDraw12(Wrap* w, UINT vertices, UINT instances, UINT start, UINT first_instance)
{
    if (BootstrapNoopEnabled() && IsXboxList(w)) {
        if (!BindBootstrapGraphics(w)) return;
        ++g_bootstrap.draw;
    }
    ((ID3D12GraphicsCommandList*)w->real)->DrawInstanced(vertices, instances, start, first_instance);
}
static void __stdcall ListDrawIndexed13(Wrap* w, UINT indices, UINT instances, UINT start, INT base, UINT first_instance)
{
    if (BootstrapNoopEnabled() && IsXboxList(w)) {
        if (!BindBootstrapGraphics(w)) return;
        ++g_bootstrap.draw;
    }
    ((ID3D12GraphicsCommandList*)w->real)->DrawIndexedInstanced(indices, instances, start, base, first_instance);
}
static void __stdcall ListDispatch14(Wrap* w, UINT x, UINT y, UINT z)
{
    if (BootstrapNoopEnabled() && IsXboxList(w)) {
        if (!BindBootstrapCompute(w)) return;
        ++g_bootstrap.dispatch;
    }
    ((ID3D12GraphicsCommandList*)w->real)->Dispatch(x, y, z);
}
static void __stdcall ListBarriers26(Wrap* w, UINT count, const D3D12_RESOURCE_BARRIER* barriers)
{
    if (BootstrapNoopEnabled()) ++g_bootstrap.barriers;
    ((ID3D12GraphicsCommandList*)w->real)->ResourceBarrier(count, barriers);
}
static void __stdcall ListSlot34(Wrap* w, uintptr_t argument, UINT value, UINT offset)
{
    auto* list = (ID3D12GraphicsCommandList*)w->real;
    if (!IsXboxList(w)) {
        list->SetGraphicsRoot32BitConstant((UINT)argument, value, offset);
        return;
    }
    // Game RVA0x327c5 passes a root-signature pointer, not a root index.
    // Confirm interface identity before giving the native object to VKD3D.
    auto* candidate = (IUnknown*)Unwrap((void*)argument);
    ID3D12RootSignature* root = nullptr;
    HRESULT hr = S_OK;
    if (candidate) {
        if (IsBadReadPtr(candidate, sizeof(void*))) hr = E_INVALIDARG;
        else hr = candidate->QueryInterface(__uuidof(ID3D12RootSignature), (void**)&root);
    }
    static std::atomic<unsigned> calls{0};
    if (calls++ < 12 || FAILED(hr)) {
        CALLER(c);
        XS_LOG("ROOT ABI Xbox list34 arg=%p QI_ID3D12RootSignature=0x%08lX root=%p caller=%s", (void*)argument, (unsigned long)hr, (void*)root, c);
    }
    if (SUCCEEDED(hr)) list->SetGraphicsRootSignature(root);
    else XS_LOG("UNSUPPORTED Xbox list34: invalid root signature; binding skipped");
    if (root) root->Release();
}
static void __stdcall ListSlot36(Wrap* w, UINT index, uintptr_t argument, const void* data, UINT offset)
{
    auto* list = (ID3D12GraphicsCommandList*)w->real;
    if (IsXboxList(w)) {
        // Game RVA0x321ff/0x3223a: R8 is an allocated descriptor-table
        // offset plus the shader-visible heap's GPU base. No count/data/offset.
        static std::atomic<unsigned> calls{0};
        if (calls++ < 12) { CALLER(c); XS_LOG("ROOT ABI Xbox list36 table index=%u GPU=%p caller=%s", index, (void*)argument, c); }
        list->SetGraphicsRootDescriptorTable(index, D3D12_GPU_DESCRIPTOR_HANDLE{argument});
    } else list->SetGraphicsRoot32BitConstants(index, (UINT)argument, data, offset);
}
static void __stdcall ListSlot40(Wrap* w, UINT index, uintptr_t argument, const void* data, UINT offset)
{
    auto* list = (ID3D12GraphicsCommandList*)w->real;
    if (IsXboxList(w)) {
        // Game RVA0x3246c: root index, dword count, source array, offset=0.
        // Raw-packet branch beside it copies exactly count dwords.
        static std::atomic<unsigned> calls{0};
        if (calls++ < 12) { CALLER(c); XS_LOG("ROOT ABI Xbox list40 constants index=%u count=%u data=%p offset=%u caller=%s", index, (UINT)argument, data, offset, c); }
        list->SetGraphicsRoot32BitConstants(index, (UINT)argument, data, offset);
    } else list->SetGraphicsRootShaderResourceView(index, argument);
}
static HRESULT __stdcall List_Reset(Wrap* w, ID3D12CommandAllocator* a, ID3D12PipelineState* p) { return ((ID3D12GraphicsCommandList*)w->real)->Reset((ID3D12CommandAllocator*)Unwrap(a), (ID3D12PipelineState*)Unwrap(p)); }
static uint64_t __stdcall Heap_GetCPUStart(Wrap* w) { if (w->pad[1]) return w->pad[1]; D3D12_CPU_DESCRIPTOR_HANDLE h = {}; ((void* (*)(void*, void*))(*(void***)w->real)[9])(w->real, &h); return h.ptr; }
static uint64_t __stdcall Heap_GetGPUStart(Wrap* w) { D3D12_GPU_DESCRIPTOR_HANDLE h = {}; ((void* (*)(void*, void*))(*(void***)w->real)[10])(w->real, &h); return h.ptr; }
// Слот 9 allocator'а (Xbox): наблюдался вызов (this, fn, ctx) из 0x1401884fe — регистрация колбэка; пока лог + S_OK.
static HRESULT __stdcall Alloc_Slot9(Wrap*, uintptr_t fn, uintptr_t ctx) { static std::atomic<int> n{0}; if (n++ < 4) XS_LOG("allocator slot9 (callback?) fn=%p ctx=%p -> S_OK", (void*)fn, (void*)ctx); return S_OK; }
static HRESULT __stdcall Queue_ExecuteX35(Wrap* w, UINT n, ID3D12CommandList* const* lists, uint64_t x, uint64_t y);
static void InitObjVts()
{
    static SRWLOCK lock = SRWLOCK_INIT;
    ExclusiveSrwLock held(lock);
    static bool done = false; if (done) return; done = true;
    FillObj<2>(g_vtQueue, std::make_integer_sequence<int, kObjSlots>{}); FillObj<3>(g_vtList, std::make_integer_sequence<int, kObjSlots>{}); FillObj<4>(g_vtHeap, std::make_integer_sequence<int, kObjSlots>{}); FillObj<5>(g_vtPso, std::make_integer_sequence<int, kObjSlots>{}); FillObj<6>(g_vtAlloc, std::make_integer_sequence<int, kObjSlots>{});
    FillObj<7>(g_vtFence, std::make_integer_sequence<int, kObjSlots>{});
    void (*fw[80])() = {
#define F(i) xs_fwd_##i
        F(0),F(1),F(2),F(3),F(4),F(5),F(6),F(7),F(8),F(9),F(10),F(11),F(12),F(13),F(14),F(15),F(16),F(17),F(18),F(19),F(20),F(21),F(22),F(23),F(24),F(25),F(26),F(27),F(28),F(29),F(30),F(31),
        F(32),F(33),F(34),F(35),F(36),F(37),F(38),F(39),F(40),F(41),F(42),F(43),F(44),F(45),F(46),F(47),F(48),F(49),F(50),F(51),F(52),F(53),F(54),F(55),F(56),F(57),F(58),F(59),F(60),F(61),F(62),F(63),
        F(64),F(65),F(66),F(67),F(68),F(69),F(70),F(71),F(72),F(73),F(74),F(75),F(76),F(77),F(78),F(79) };
    for (int i = 0; i < 18; i++) g_vtQueue[i] = (void*)fw[i];        // ID3D12CommandQueue: 0..17
    for (int i = 0; i < 60; i++) g_vtList[i] = (void*)fw[i];         // ID3D12GraphicsCommandList core: 0..59 (слоты >=60 у Xbox-списка ДРУГИЕ: на PC 70/71 = Initialize/ExecuteMetaCommand -> assert VKD3D)
    for (int i = 0; i < 11; i++) g_vtHeap[i] = (void*)fw[i];         // ID3D12DescriptorHeap: 0..10
    for (int i = 0; i < 9; i++) g_vtAlloc[i] = (void*)fw[i]; g_vtAlloc[0] = (void*)Obj_QI; g_vtAlloc[7] = (void*)Obj_GetDevice; g_vtAlloc[9] = (void*)Alloc_Slot9;   // ID3D12CommandAllocator: 0..8; слот 9 — Xbox-расширение
    for (int i = 0; i < 9; i++) g_vtPso[i] = (void*)fw[i]; g_vtPso[0] = (void*)Obj_QI; g_vtPso[7] = (void*)Obj_GetDevice;   // ID3D12PipelineState: 0..8; слоты >=9 — Xbox-расширения (лог)
    for (int i = 0; i < 12; ++i) g_vtFence[i] = (void*)fw[i];
    g_vtFence[0] = (void*)FenceQI; g_vtFence[1] = (void*)FenceAddRef; g_vtFence[2] = (void*)FenceRelease;
    g_vtFence[7] = (void*)Obj_GetDevice; g_vtFence[8] = (void*)FenceCompleted8;
    g_vtFence[10] = (void*)FenceSignal10; g_vtFence[12] = (void*)FenceAddress12;
    void** vts[3] = { g_vtQueue, g_vtList, g_vtHeap };
    for (void** vt : vts) { vt[0] = (void*)Obj_QI; vt[7] = (void*)Obj_GetDevice; }
    g_vtQueue[10] = (void*)Queue_ExecuteCommandLists; g_vtQueue[18] = (void*)Queue_TimestampFrequency18; g_vtQueue[35] = (void*)Queue_ExecuteX35; g_vtList[28] = (void*)List_SetDescriptorHeaps; g_vtList[25] = (void*)List_SetPipelineState; g_vtList[10] = (void*)List_Reset;
    g_vtQueue[14] = (void*)QueueSignal14; g_vtQueue[15] = (void*)QueueWait15;
    g_vtQueue[16] = (void*)QueueSlot16;
    g_vtQueue[32] = (void*)QueuePresent32;
    g_vtList[12] = (void*)ListDraw12; g_vtList[13] = (void*)ListDrawIndexed13;
    g_vtList[14] = (void*)ListDispatch14; g_vtList[26] = (void*)ListBarriers26;
    g_vtList[52] = (void*)List_Slot52; g_vtList[53] = (void*)List_EndQuery53; g_vtList[54] = (void*)List_ResolveQuery54;
    g_vtList[34] = (void*)ListSlot34;
    g_vtList[36] = (void*)ListSlot36; g_vtList[40] = (void*)ListSlot40;
    g_vtHeap[9] = (void*)Heap_GetCPUStart; g_vtHeap[10] = (void*)Heap_GetGPUStart;
    g_vtList[46] = (void*)List_OMSetRenderTargets; g_vtList[47] = (void*)ListSlot47; g_vtList[48] = (void*)ListSlot48; g_vtList[49] = (void*)List_ClearUAVUint; g_vtList[50] = (void*)List_ClearUAVFloat;
    InstallBootstrapRootSlots(std::make_integer_sequence<int, 14>{}); // Xbox root33..46, PC contracts retained.
}
// Слот 48 — CreateCommandQueue(xbox_desc*, riid, ppv) [по IID ID3D12CommandQueue в r8; 4 вызова подряд с 4 выходными глобалами].
// Раскладка Xbox-дескриптора НЕ известна: наблюдалось u32[0..5] = {2,0,k,m,1,0}, k=1,2,3,0; считаем u32[0] = тип очереди (2 = COMPUTE).
static HRESULT __stdcall Dev_CreateCommandQueue48(Wrap* w, const uint32_t* xd, REFIID riid, void** out)
{
    D3D12_COMMAND_QUEUE_DESC d = {}; uint32_t t = xd ? xd[0] : 0; d.Type = (D3D12_COMMAND_LIST_TYPE)(t <= 3 && t != 1 ? t : 0);
    HRESULT hr = ((ID3D12Device*)w->real)->CreateCommandQueue(&d, riid, out); InitObjVts(); hr = WrapOutput(hr, out, g_vtQueue, 2);
    XS_LOG("DEVICE slot48 CreateCommandQueue xbox_desc[0..5]=%u,%u,%u,%u,%u,%u -> type=%d hr=0x%08lX q=%p", xd ? xd[0] : 0, xd ? xd[1] : 0, xd ? xd[2] : 0, xd ? xd[3] : 0, xd ? xd[4] : 0, xd ? xd[5] : 0, (int)d.Type, (unsigned long)hr, out ? *out : nullptr);
    return hr;
}
// Слот 73 — CreateCommandList(xbox_desc*, allocator, pso, riid, ppv). Наблюдалось xd = {0,2,1,0,1,0,...}: тип списка = xd[0] (гипотеза; xd[1]=2 — неизвестное поле).
static HRESULT __stdcall Dev_CreateCommandList73(Wrap* w, const uint32_t* xd, ID3D12CommandAllocator* alloc, ID3D12PipelineState* pso, REFIID riid, void** out)
{
    uint32_t t = xd ? xd[0] : 0; D3D12_COMMAND_LIST_TYPE ty = (D3D12_COMMAND_LIST_TYPE)(t <= 3 ? t : 0);
    HRESULT hr = ((ID3D12Device*)w->real)->CreateCommandList(0, ty, (ID3D12CommandAllocator*)Unwrap(alloc), (ID3D12PipelineState*)Unwrap(pso), riid, out);
    if (hr == E_NOINTERFACE) {   // Xbox-IID (VKD3D не знает): создаём стандартный список, интерфейс считаем Xbox-вариантом
        XS_LOG("DEVICE slot73 riid неизвестен VKD3D -> создаю ID3D12GraphicsCommandList и отдаю как Xbox-интерфейс");
        hr = ((ID3D12Device*)w->real)->CreateCommandList(0, ty, (ID3D12CommandAllocator*)Unwrap(alloc), (ID3D12PipelineState*)Unwrap(pso), __uuidof(ID3D12GraphicsCommandList), out);
    }
    InitObjVts(); hr = WrapOutput(hr, out, g_vtList, 3);
    if (SUCCEEDED(hr)) {
        auto* list = (Wrap*)*out;
        TagXboxList(list); // Host metadata must not alias the game's inline command fields.
        *out = list;
    }
    XS_LOG("DEVICE slot73 riid={%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", riid.Data1, riid.Data2, riid.Data3, riid.Data4[0], riid.Data4[1], riid.Data4[2], riid.Data4[3], riid.Data4[4], riid.Data4[5], riid.Data4[6], riid.Data4[7]);
    XS_LOG("DEVICE slot73 CreateCommandList xbox_desc[0..5]=%u,%u,%u,%u,%u,%u -> type=%d alloc=%p pso=%p hr=0x%08lX list=%p", xd ? xd[0] : 0, xd ? xd[1] : 0, xd ? xd[2] : 0, xd ? xd[3] : 0, xd ? xd[4] : 0, xd ? xd[5] : 0,
           (int)ty, (void*)alloc, (void*)pso, (unsigned long)hr, out ? *out : nullptr);
    return hr;
}
static HRESULT __stdcall Dev_CreateCommandAllocator9(Wrap* w, D3D12_COMMAND_LIST_TYPE t, REFIID riid, void** out)
{
    HRESULT hr = ((ID3D12Device*)w->real)->CreateCommandAllocator(t, riid, out); InitObjVts(); hr = WrapOutput(hr, out, g_vtAlloc, 6);
    XS_LOG("DEVICE slot9 CreateCommandAllocator type=%d -> hr=0x%08lX alloc=%p", (int)t, (unsigned long)hr, out ? *out : nullptr); return hr;
}
// Game RVA0x1d0b2fa uses IID_ID3D12Fence with initial=0, flags=SHARED.
// PC CreateFence signature is the same; Xbox fence methods are not.
static HRESULT __stdcall Dev_CreateFence36(Wrap* w, UINT64 value, D3D12_FENCE_FLAGS flags, REFIID iid, void** out)
{
    HRESULT hr = ((ID3D12Device*)w->real)->CreateFence(value, flags, iid, out);
    InitObjVts(); hr = WrapOutput(hr, out, g_vtFence, 7);
    static std::atomic<unsigned> calls{0};
    if (calls++ < 12) XS_LOG("DEVICE slot36 CreateFence initial=%llu flags=%#x -> hr=0x%08lX fence=%p", (unsigned long long)value,
        (unsigned)flags, (unsigned long)hr, out ? *out : nullptr);
    return hr;
}
static HRESULT __stdcall Dev_CreateCommandList12(Wrap* w, UINT node, D3D12_COMMAND_LIST_TYPE type,
    ID3D12CommandAllocator* allocator, ID3D12PipelineState* pso, REFIID iid, void** out)
{
    HRESULT hr = ((ID3D12Device*)w->real)->CreateCommandList(node, type,
        (ID3D12CommandAllocator*)Unwrap(allocator), (ID3D12PipelineState*)Unwrap(pso), iid, out);
    InitObjVts(); hr = WrapOutput(hr, out, g_vtList, 3);
    return hr;
}

static HRESULT __stdcall Dev_CreateDescriptorHeap14(Wrap* w, const D3D12_DESCRIPTOR_HEAP_DESC* d, REFIID riid, void** out)
{
    HRESULT hr = ((ID3D12Device*)w->real)->CreateDescriptorHeap(d, riid, out); InitObjVts();
    uint64_t guestBase = 0, guestSize = 0;
    if (SUCCEEDED(hr) && out && *out) {
        UINT inc = ((ID3D12Device*)w->real)->GetDescriptorHandleIncrementSize(d->Type);
        const uint64_t* ext = (const uint64_t*)((const char*)d + 16);        // расширение Xbox-дескриптора: {base, size}
        if (!IsBadReadPtr(ext, 16) && ext[0] && !(ext[0] & 0xFFF) && ext[1] == (uint64_t)d->NumDescriptors * inc && g_hrN < 512) {
            guestBase = ext[0]; guestSize = ext[1];
            D3D12_CPU_DESCRIPTOR_HANDLE rs = {}; ((void* (*)(void*, void*))(*(void***)*out)[9])(*out, &rs);
            int i = g_hrN.load(); g_hr[i] = { guestBase, guestSize, rs.ptr }; g_hrN.store(i + 1, std::memory_order_release);
        }
        hr = WrapOutput(hr, out, g_vtHeap, 4);
        if (SUCCEEDED(hr)) ((Wrap*)*out)->pad[1] = guestBase;
    }
    static int n = 0; if (n++ < 12) XS_LOG("DEVICE slot14 CreateDescriptorHeap type=%d n=%u flags=0x%x guestBase=%#llx size=%#llx -> hr=0x%08lX heap=%p", (int)d->Type, d->NumDescriptors, (unsigned)d->Flags, (unsigned long long)guestBase, (unsigned long long)guestSize, (unsigned long)hr, out ? *out : nullptr);
    return hr;
}

// Режим трассировки XSX_DEV_TRACE=1: логирует каждый пересылаемый вызов device (слот, аргументы, возвращаемое значение). Только 6 аргументов передаются дальше.
static bool g_trace = false;
static void (*xs_fwd_ptrs[44])() = { xs_fwd_0,xs_fwd_1,xs_fwd_2,xs_fwd_3,xs_fwd_4,xs_fwd_5,xs_fwd_6,xs_fwd_7,xs_fwd_8,xs_fwd_9,xs_fwd_10,xs_fwd_11,xs_fwd_12,xs_fwd_13,xs_fwd_14,xs_fwd_15,xs_fwd_16,xs_fwd_17,xs_fwd_18,xs_fwd_19,xs_fwd_20,xs_fwd_21,xs_fwd_22,xs_fwd_23,xs_fwd_24,xs_fwd_25,xs_fwd_26,xs_fwd_27,xs_fwd_28,xs_fwd_29,xs_fwd_30,xs_fwd_31,xs_fwd_32,xs_fwd_33,xs_fwd_34,xs_fwd_35,xs_fwd_36,xs_fwd_37,xs_fwd_38,xs_fwd_39,xs_fwd_40,xs_fwd_41,xs_fwd_42,xs_fwd_43 };
template <int N> static uintptr_t __stdcall DevTrace(void* self, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e, uintptr_t f)
{
    typedef uintptr_t (__stdcall* Fn)(void*, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
    uintptr_t r = ((Fn)xs_fwd_ptrs[N])(self, b, c, d, e, f);
    static std::atomic<int> cnt[44]; int n = ++cnt[N];
    if (n <= 40) { char who[300]; CallerStr(__builtin_return_address(0), who, sizeof who);
        XS_LOG("TRACE dev slot %d call#%d rdx=%p r8=%p r9=%p -> rax=%p caller=%s", N, n, (void*)b, (void*)c, (void*)d, (void*)r, who); }
    return r;
}
template <int... I> static void FillTrace(std::integer_sequence<int, I...>) { ((g_devVt[I] = (void*)&DevTrace<I>), ...); }

// Слот 133 (+0x428) — CreatePlacedResourceX(void* addr, const D3D12_RESOURCE_DESC*, state, clear, riid, ppv) [расшифровано по callsite 0x141e86852 и дампу desc: буфер 176 МБ по адресу из XMemMapPhysicalPages].
// PC-аналог: ID3D12Device3::OpenExistingHeapFromAddress (слот 48) + CreatePlacedResource. Если не вышло — committed-буфер (БЕЗ привязки к памяти игры; логируется).
struct PlacedRes { uint64_t addr, size; void* res; };
static PlacedRes g_placed[256]; static std::atomic<int> g_placedN{0};
static HRESULT __stdcall Dev_CreatePlacedResourceX133(Wrap* w, void* addr, const D3D12_RESOURCE_DESC* desc, UINT state, const D3D12_CLEAR_VALUE* cv, REFIID riid, void** out)
{
    ID3D12Device* real = (ID3D12Device*)w->real; D3D12_RESOURCE_DESC dd = PcResourceDesc(*desc);
    ID3D12Heap* heap = nullptr; const char* path = "placed-on-address";
    HRESULT hr = ((HRESULT (*)(void*, const void*, REFIID, void**))(*(void***)real)[48])(real, addr, __uuidof(ID3D12Heap), (void**)&heap);
    if (SUCCEEDED(hr) && heap) {
        hr = real->CreatePlacedResource(heap, 0, &dd, D3D12_RESOURCE_STATE_COMMON, cv, riid, out);
        if (FAILED(hr)) hr = real->CreatePlacedResource(heap, 0, &dd, D3D12_RESOURCE_STATE_GENERIC_READ, cv, riid, out);
        D3D12_HEAP_DESC hd = {}; ((void* (*)(void*, void*))(*(void***)heap)[8])(heap, &hd);
        MEMORY_BASIC_INFORMATION mbi = {}; VirtualQuery(addr, &mbi, sizeof mbi);
        XS_LOG("DEVICE slot133 heap size=%#llx type=%d flags=%#x align=%#llx; VirtualQuery region=%#llx state=%#lx; resource width=%#llx -> placed hr=0x%08lX", (unsigned long long)hd.SizeInBytes, (int)hd.Properties.Type,
               (unsigned)hd.Flags, (unsigned long long)hd.Alignment, (unsigned long long)mbi.RegionSize, mbi.State, (unsigned long long)dd.Width, (unsigned long)hr);
        heap->Release();
        if (FAILED(hr)) { path = "committed-fallback (placed не удался)"; D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            hr = real->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &dd, D3D12_RESOURCE_STATE_COMMON, cv, riid, out); }
    } else {
        XS_LOG("DEVICE slot133 OpenExistingHeapFromAddress(%p) FAILED 0x%08lX -> fallback committed (память игры НЕ используется)", addr, (unsigned long)hr);
        path = "committed-fallback"; D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        hr = real->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &dd, D3D12_RESOURCE_STATE_COMMON, cv, riid, out);
    }
    if (SUCCEEDED(hr) && out && *out && g_placedN < 256) { int i = g_placedN.load(); g_placed[i] = { (uint64_t)addr, dd.Width, *out }; g_placedN.store(i + 1); }
    XS_LOG("DEVICE slot133 CreatePlacedResourceX addr=%p dim=%d width=%#llx state=%#x -> %s hr=0x%08lX res=%p", addr, (int)desc->Dimension, (unsigned long long)desc->Width, state, path, (unsigned long)hr, out ? *out : nullptr);
    return hr;
}
// Слоты 78/79 — SetFrameIntervalX(output=NULL, interval, frames, flags) / ScheduleFrameEventX(event, ...) [frame pacing Xbox; на PC нет аналога]: S_OK, один раз логируем.
static HRESULT __stdcall Dev_FramePacing(Wrap* w, uintptr_t a, uintptr_t b, uintptr_t c)
{
    static std::atomic<int> n{0}; if (n++ < 6) XS_LOG("DEVICE slot78/79 frame pacing (SetFrameIntervalX/ScheduleFrameEventX) a=%p b=%p c=%p -> S_OK (no-op)", (void*)a, (void*)b, (void*)c);
    return S_OK;
}

// Слот 127 (+0x3f8): «тяжёлая» Xbox-аллокация runtime (вызывается 2 раза в функции 0x14247ad10 между push/pop хука пула).
// Игра ставит через `__XMemAllocationHooks__` (экспорт exe; hooks[0]=Alloc(size, attrs)) хук категории 0x80 (атрибуты биты 16..23) с пулом 64/82 МБ и в конце проверяет,
// что runtime израсходовал пул почти целиком (остаток <= 0x1fffff, иначе int3 @0x247af2d). Настоящего Xbox-runtime у нас нет → «потребляем» пул сами:
// первым вызовом забираем (остаток - 1 МиБ) через хук. Смещения — RVA этой сборки exe (stack хуков 0x6843770, база пула 0x75e6ff0); при несовпадении структуры — пропуск.
static HRESULT __stdcall Dev_Slot127(Wrap* w, uintptr_t a, uintptr_t b, uintptr_t c)
{
    static std::atomic<int> n{0}; int k = n++;
    char* base = (char*)GetModuleHandleW(nullptr);
    typedef void* (*AllocHook)(size_t, uint64_t);
    auto hooks = (void**)GetProcAddress((HMODULE)base, "__XMemAllocationHooks__");
    uint64_t* top = (uint64_t*)(base + 0x6843770);                       // вершина стека хуков категории 0x80: {fn, userdata, ctx, next}
    uint64_t* poolBase = (uint64_t*)(base + 0x75e6ff0);
    if (hooks && !IsBadReadPtr(top, 8) && *top && !IsBadReadPtr((void*)*top, 32)) {
        uint64_t* ent = (uint64_t*)*top; uint64_t* ud = (uint64_t*)ent[1];
        if (ud && !IsBadReadPtr(ud, 16)) {
            if (!*poolBase) { ((AllocHook)hooks[0])(0x10000, 0x00808000); }              // первый вызов инициализирует пул
            uint64_t cursor = ud[0], size = ud[1], pb = *poolBase;
            if (pb && cursor >= pb && size) {
                uint64_t remaining = pb + size - cursor; uint64_t take = remaining > (1u << 20) ? ((remaining - (1u << 20)) & ~0xFFFFull) : 0;
                void* p = take ? ((AllocHook)hooks[0])((size_t)take, 0x00808000) : nullptr;
                XS_LOG("DEVICE slot127 call#%d: пул хука 0x80: base=%#llx size=%#llx cursor=%#llx remaining=%#llx -> взято %#llx (%p)", k, (unsigned long long)pb, (unsigned long long)size, (unsigned long long)cursor, (unsigned long long)remaining, (unsigned long long)take, p);
                return S_OK;
            }
        }
    }
    XS_LOG("DEVICE slot127 call#%d: хук пула не найден/не инициализирован (hooks=%p top=%p) -> S_OK без потребления", k, (void*)hooks, (void*)top);
    return S_OK;
}

// Нативные (ISA) pipeline state: слот 62 (+0x1f0) — из XboxBvhBuilder.dll (this, desc*, 0, 0, riid=ID3D12PipelineState, ppv); слот 67 (+0x218) — из FFX (this, desc*, riid, ppv).
// Трансляция ISA→PC — отдельная задача (другой агент); здесь: лог дескриптора + честный отказ E_NOTIMPL, выход обнуляется.
static void LogNativePso(const char* who, int slot, const void* desc)
{
    static std::atomic<int> n{0}; if (n++ >= 24) return; char buf[400] = {};
    if (desc && !IsBadReadPtr(desc, 0x48)) { const uint64_t* q = (const uint64_t*)desc; snprintf(buf, sizeof buf, "%016llx %016llx %016llx %016llx %016llx %016llx %016llx %016llx %016llx",
        (unsigned long long)q[0], (unsigned long long)q[1], (unsigned long long)q[2], (unsigned long long)q[3], (unsigned long long)q[4], (unsigned long long)q[5], (unsigned long long)q[6], (unsigned long long)q[7], (unsigned long long)q[8]); }
    XS_LOG("DEVICE slot%d NATIVE PSO (%s) desc=%p [%s] (native translation unavailable)", slot, who, desc, buf);
}
#include "native_pso_capture.h"

// Game RVA0x2477578: (this, desc, IID_ID3D12PipelineState, out).
// The following code tests HRESULT and packs *out >> 1 into the PSO cache.
// Returning S_OK without writing *out cached exe+0x6993a4, then faulted on
// AddRef at RVA0x247a09e. No native description may reach a PC PSO API until
// its shader/fixed-state layout has been decoded and translated.
static HRESULT __stdcall Dev_NativePso57(Wrap* w, const void* desc, REFIID iid, void** out)
{
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!desc) return E_INVALIDARG;
    if (!IsEqualGUID(iid, __uuidof(ID3D12PipelineState))) return E_NOINTERFACE;
    CapturePso57(desc, iid, __builtin_return_address(0));
    if (BootstrapNoopEnabled()) return BootstrapGraphicsOutput(w, iid, out);
    static std::atomic<unsigned> calls{0};
    const unsigned n = ++calls;
    if (n <= 8 || !(n % 1000))
        XS_LOG("DEVICE slot57 native PSO call#%u desc=%p -> E_NOTIMPL out=NULL (translation required)", n, desc);
    return E_NOTIMPL;
}

// Game RVA0x247a5c2: base PSO, update count, array of tagged 0x14c-byte
// records, IID, out. Semantic method/type names remain unconfirmed. It is
// NOT an ordinary PC CreateGraphicsPipelineState descriptor or a callback.
static HRESULT __stdcall Dev_PsoUpdates55(Wrap*, ID3D12PipelineState* base,
    UINT count, const void* updates, REFIID iid, void** out)
{
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!base || (count && !updates)) return E_INVALIDARG;
    if (!IsEqualGUID(iid, __uuidof(ID3D12PipelineState))) return E_NOINTERFACE;
    if (BootstrapNoopEnabled()) {
        // Diagnostic fixed-state updates keep the same valid no-op PSO.
        return ((IUnknown*)base)->QueryInterface(iid, out);
    }
    static std::atomic<unsigned> calls{0};
    if (calls++ < 8)
        XS_LOG("DEVICE slot55 PSO updates base=%p count=%u updates=%p -> E_NOTIMPL out=NULL", (void*)base, count, updates);
    return E_NOTIMPL;
}
// ЗАГЛУШКА (маркируется в логе): пустой compute-PSO вместо нативного ISA-шейдера, чтобы XboxBvhBuilder не бросал исключение при инициализации. Построение BVH с ним НЕ работает.
static ID3D12PipelineState* GetStubComputePso(ID3D12Device* dev)
{
    static SRWLOCK lock = SRWLOCK_INIT;
    ExclusiveSrwLock held(lock);
    struct Cached { ID3D12Device* owner; ID3D12PipelineState* pso; };
    static std::vector<Cached> cache; // Owns one PSO reference per device for the DLL lifetime.
    for (const auto& entry : cache) if (entry.owner == dev) return entry.pso;
    HMODULE comp = LoadLibraryW(L"d3dcompiler_47.dll"); auto compile = comp ? (decltype(&D3DCompile))GetProcAddress(comp, "D3DCompile") : nullptr; if (!compile) return nullptr;
    struct Blobs { ID3DBlob *cs = nullptr, *sig = nullptr, *error = nullptr;
        ~Blobs() { if (cs) cs->Release(); if (sig) sig->Release(); if (error) error->Release(); } } blobs;
    static const char* src = "[numthreads(1,1,1)] void main() {}";
    if (FAILED(compile(src, strlen(src), "stub", nullptr, nullptr, "main", "cs_5_0", 0, 0, &blobs.cs, &blobs.error))) return nullptr;
    if (blobs.error) { blobs.error->Release(); blobs.error = nullptr; }
    D3D12_ROOT_SIGNATURE_DESC rd = {}; HMODULE d3d = LoadLibraryW(L"d3d12.dll"); typedef HRESULT (WINAPI* SerFn)(const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**, ID3DBlob**);
    auto ser = (SerFn)GetProcAddress(d3d, "D3D12SerializeRootSignature");
    if (!ser || FAILED(ser(&rd, D3D_ROOT_SIGNATURE_VERSION_1_0, &blobs.sig, &blobs.error))) return nullptr;
    ID3D12RootSignature* rs = nullptr;
    if (FAILED(dev->CreateRootSignature(0, blobs.sig->GetBufferPointer(), blobs.sig->GetBufferSize(), __uuidof(ID3D12RootSignature), (void**)&rs))) return nullptr;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {}; pd.pRootSignature = rs; pd.CS = { blobs.cs->GetBufferPointer(), blobs.cs->GetBufferSize() };
    ID3D12PipelineState* pso = nullptr;
    const HRESULT hr = dev->CreateComputePipelineState(&pd, __uuidof(ID3D12PipelineState), (void**)&pso);
    rs->Release();
    if (FAILED(hr)) return nullptr;
    cache.push_back({dev, pso});
    return pso;
}
#include "bootstrap_noop_pso.h"
static HRESULT __stdcall Dev_NativePso62(Wrap* w, const void* desc, uintptr_t, uintptr_t, REFIID riid, void** ppv)
{
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    LogNativePso("BVH-builder-style", 62, desc);
    ID3D12PipelineState* st = GetStubComputePso((ID3D12Device*)w->real);
    if (st) { HRESULT hr = st->QueryInterface(riid, ppv); InitObjVts(); hr = WrapOutput(hr, ppv, g_vtPso, 5);
        XS_LOG("DEVICE slot62 -> ЗАГЛУШКА: пустой compute-PSO %p (BVH не функционален) hr=0x%08lX", (void*)st, (unsigned long)hr); return hr; }
    if (ppv) *ppv = nullptr; return E_NOTIMPL;
}
static HRESULT __stdcall Dev_NativePso67(Wrap* w, const void* desc, REFIID iid, void** ppv)
{
    LogNativePso("FFX-style", 67, desc);
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (BootstrapNoopEnabled()) {
        if (!IsEqualGUID(iid, __uuidof(ID3D12PipelineState))) return E_NOINTERFACE;
        auto* pso = GetStubComputePso((ID3D12Device*)w->real);
        return pso ? WrapOutput(pso->QueryInterface(iid, ppv), ppv, g_vtPso, 5) : E_FAIL;
    }
    return E_NOTIMPL;
}

// Слот 41 — CreateCommandSignature(desc, rootsig, riid, ppv): логируем дескриптор (Xbox может добавлять свои типы indirect-аргументов).
static HRESULT __stdcall Dev_CreateCommandSignature41(Wrap* w, const D3D12_COMMAND_SIGNATURE_DESC* d, ID3D12RootSignature* rs, REFIID riid, void** out)
{
    HRESULT hr = ((ID3D12Device*)w->real)->CreateCommandSignature(d, rs, riid, out);
    if (FAILED(hr)) {   // Xbox-специфичные типы indirect-аргументов (наблюдался 105, stride 12 = 3×uint): пробуем DISPATCH_MESH(9), затем DISPATCH(2)
        std::vector<D3D12_INDIRECT_ARGUMENT_DESC> tmp;
        for (int variant : { 9, 2 }) {
            tmp.assign(d->pArgumentDescs, d->pArgumentDescs + d->NumArgumentDescs); bool changed = false; for (auto& t : tmp) if ((int)t.Type >= 100) { t.Type = (D3D12_INDIRECT_ARGUMENT_TYPE)variant; changed = true; }
            if (!changed) break; D3D12_COMMAND_SIGNATURE_DESC nd = *d; nd.pArgumentDescs = tmp.data();
            hr = ((ID3D12Device*)w->real)->CreateCommandSignature(&nd, rs, riid, out);
            XS_LOG("DEVICE slot41   Xbox-тип indirect-аргумента -> PC тип %d: hr=0x%08lX", variant, (unsigned long)hr); if (SUCCEEDED(hr)) break;
        }
    }
    char a[400] = {}; int o = 0;
    for (UINT i = 0; i < d->NumArgumentDescs && i < 16 && o < 360; i++) o += snprintf(a + o, sizeof a - o, "%u:(%d)", i, (int)d->pArgumentDescs[i].Type);
    XS_LOG("DEVICE slot41 CreateCommandSignature stride=%u nargs=%u args=[%s] rootsig=%p -> hr=0x%08lX", d->ByteStride, d->NumArgumentDescs, a, (void*)rs, (unsigned long)hr);
    return hr;
}

// Слот 86 (+0x2b0) — нативный PSO из игры: (this, desc*{size, ptr…}, riid=ID3D12PipelineState, ppv), 6 вызовов. ЗАГЛУШКА (как слот 62): пустой compute-PSO. Графические PSO этим не заменяются корректно.
static HRESULT __stdcall Dev_NativePso86(Wrap* w, const void* desc, REFIID riid, void** ppv)
{
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    LogNativePso("game-style", 86, desc);
    ID3D12PipelineState* st = GetStubComputePso((ID3D12Device*)w->real);
    if (st) {
        HRESULT hr = st->QueryInterface(riid, ppv); InitObjVts(); hr = WrapOutput(hr, ppv, g_vtPso, 5);
        static std::atomic<unsigned> n{0};
        if (n++ < 6) XS_LOG("DEVICE slot86 -> ЗАГЛУШКА: пустой compute-PSO (native shader NOT executed) hr=0x%08lX", (unsigned long)hr);
        return hr;
    }
    if (ppv) *ppv = nullptr; return E_NOTIMPL;
}
// Слот 74 (+0x250) — heap по диапазону VA: (this, VA, pages(64 КиБ), ppv). Заглушка: обычный DEFAULT-heap ограниченного размера (<=512 МиБ) — память игры НЕ используется.
static HRESULT __stdcall Dev_HeapFromRange74(Wrap* w, void* va, uint64_t pages, void** ppv)
{
    uint64_t want = pages * 0x10000, sz = want > (512ull << 20) ? (512ull << 20) : want;
    D3D12_HEAP_DESC hd = {}; hd.SizeInBytes = sz; hd.Properties.Type = D3D12_HEAP_TYPE_DEFAULT; hd.Alignment = 0x10000; hd.Flags = D3D12_HEAP_FLAG_ALLOW_ALL_BUFFERS_AND_TEXTURES;
    ID3D12Heap* h = nullptr; HRESULT hr = ((ID3D12Device*)w->real)->CreateHeap(&hd, __uuidof(ID3D12Heap), (void**)&h); if (ppv) *ppv = h;
    static std::atomic<int> n{0}; if (n++ < 16) XS_LOG("DEVICE slot74 heap-from-range va=%p pages=%llu (want %#llx -> %#llx) -> hr=0x%08lX heap=%p (ЗАГЛУШКА, память игры не используется)", va, (unsigned long long)pages, (unsigned long long)want, (unsigned long long)sz, (unsigned long)hr, (void*)h);
    return hr;
}

// Слот 61 (+0x1e8): CreateSamplerX-подобный: (this, const sampler_desc*, void* out16) — Xbox пишет «сырой» 16-байтный HW-дескриптор сэмплера в память игры [по callsite 0x1412b9800 и дампу: в desc MaxLOD=12.0f].
// Модель Xbox: игра САМА копирует такие блобы в heap-память. На PC дескрипторы непрозрачны (VKD3D) → ЗАГЛУШКА: нулевой блоб; корректная эмуляция требует зеркалирования записи игры в PC-heap.
static HRESULT __stdcall Dev_CreateSamplerBlob61(Wrap*, const void* desc, void* out)
{
    static std::atomic<int> n{0}; if (out && !IsBadWritePtr(out, 16)) memset(out, 0, 16);
    if (n++ < 6) XS_LOG("DEVICE slot61 sampler-blob desc=%p out=%p -> нулевой блоб (ЗАГЛУШКА, дескрипторы Xbox не эмулируются)", desc, out);
    return S_OK;
}

// Слот 25 — GetResourceAllocationInfo(mask, n, descs): структура 16 байт через скрытый указатель; Xbox-порядок (RCX=ret, RDX=this) или PC (RCX=this, RDX=ret) определяем сами.
// Флаги ресурса Xbox (&0xC00000...) маскируем (иначе VKD3D возвращает «ошибочный» размер, а игра проверяет размер < 4 ГБ — int3 @0x344196).
static D3D12_RESOURCE_ALLOCATION_INFO* __stdcall Dev_GetResourceAllocationInfo25(uintptr_t a, uintptr_t b, UINT mask, UINT n, const D3D12_RESOURCE_DESC* descs)
{
    bool xbox = (b >= g_arena_lo && b < g_arena_hi) && !(a >= g_arena_lo && a < g_arena_hi);
    Wrap* w = (Wrap*)(xbox ? b : a); D3D12_RESOURCE_ALLOCATION_INFO* ret = (D3D12_RESOURCE_ALLOCATION_INFO*)(xbox ? a : b);
    std::vector<D3D12_RESOURCE_DESC> d; d.reserve(n);
    for (UINT i = 0; i < n; ++i) d.push_back(PcResourceDesc(descs[i]));
    ((void* (*)(void*, void*, UINT, UINT, const D3D12_RESOURCE_DESC*))(*(void***)w->real)[25])(w->real, ret, mask, n, d.data());
    if (n == 1 && ret->SizeInBytes == ~0ull) {   // VKD3D считает дескриптор невалидным: логируем поля и пробуем «починить» типичные расхождения Xbox
        XS_LOG("DEVICE slot25 INVALID desc: dim=%d align=%#llx %llux%u depth/arr=%u mips=%u fmt=%d samples=%u/%u layout=%d flags=%#x", (int)descs[0].Dimension, (unsigned long long)descs[0].Alignment, (unsigned long long)descs[0].Width, descs[0].Height, descs[0].DepthOrArraySize, descs[0].MipLevels, (int)descs[0].Format, descs[0].SampleDesc.Count, descs[0].SampleDesc.Quality, (int)descs[0].Layout, (unsigned)descs[0].Flags);
        D3D12_RESOURCE_DESC f = d[0]; if (!f.DepthOrArraySize) f.DepthOrArraySize = 1; if (!f.MipLevels) f.MipLevels = 1; if (!f.SampleDesc.Count) f.SampleDesc.Count = 1; f.Alignment = 0;
        if (f.Layout == D3D12_TEXTURE_LAYOUT_UNKNOWN || (int)f.Layout > 3) f.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        ((void* (*)(void*, void*, UINT, UINT, const D3D12_RESOURCE_DESC*))(*(void***)w->real)[25])(w->real, ret, mask, 1, &f);
        XS_LOG("DEVICE slot25   после нормализации -> size=%#llx", (unsigned long long)ret->SizeInBytes);
    }
    static std::atomic<int> k{0}; if (n && (k++ < 6 || ret->SizeInBytes > 0xFFFFFFFFull)) XS_LOG("DEVICE slot25 GetResourceAllocationInfo(%s порядок) dim=%d %llux%u fmt=%d flags=%#x -> size=%#llx align=%#llx", xbox ? "Xbox" : "PC", (int)descs[0].Dimension, (unsigned long long)descs[0].Width, descs[0].Height, (int)descs[0].Format, (unsigned)descs[0].Flags, (unsigned long long)ret->SizeInBytes, (unsigned long long)ret->Alignment);
    return ret;
}

// Слот 108 (+0x360) — GetResourceAllocationInfo1-подобный: (ret, this, mask, n, descs, info1*) [Xbox-порядок]; PC-аналог — слот 56. Та же нормализация дескрипторов, что в слоте 25.
static D3D12_RESOURCE_ALLOCATION_INFO* __stdcall Dev_GetResourceAllocationInfo108(uintptr_t a, uintptr_t b, UINT mask, UINT n, const D3D12_RESOURCE_DESC* descs, void* info1)
{
    bool xbox = (b >= g_arena_lo && b < g_arena_hi) && !(a >= g_arena_lo && a < g_arena_hi);
    Wrap* w = (Wrap*)(xbox ? b : a); D3D12_RESOURCE_ALLOCATION_INFO* ret = (D3D12_RESOURCE_ALLOCATION_INFO*)(xbox ? a : b);
    std::vector<D3D12_RESOURCE_DESC> d(descs, descs + n);
    for (auto& x : d) { x = PcResourceDesc(x); if (!x.DepthOrArraySize) x.DepthOrArraySize = 1; if (!x.MipLevels) x.MipLevels = 1; if (!x.SampleDesc.Count) x.SampleDesc.Count = 1; }
    ((void* (*)(void*, void*, UINT, UINT, const D3D12_RESOURCE_DESC*, void*))(*(void***)w->real)[56])(w->real, ret, mask, n, d.data(), info1);
    static std::atomic<int> k{0}; if (k++ < 6 || ret->SizeInBytes > 0xFFFFFFFFull) XS_LOG("DEVICE slot108 GetResourceAllocationInfo1(%s) n=%u dim=%d %llux%u fmt=%d -> size=%#llx align=%#llx", xbox ? "Xbox" : "PC", n, n ? (int)descs[0].Dimension : -1, n ? (unsigned long long)descs[0].Width : 0ull, n ? descs[0].Height : 0u, n ? (int)descs[0].Format : -1, (unsigned long long)ret->SizeInBytes, (unsigned long long)ret->Alignment);
    return ret;
}

// Queue слот 35 (+0x118) — ExecuteCommandListsX(count, lists**, X, 0) [callsite 0x142482689: edx=1, r8=&массив списков, r9=64-бит значение (вероятно fence/flags)]: разворачиваем списки и вызываем PC ExecuteCommandLists.
static HRESULT __stdcall Queue_ExecuteX35(Wrap* w, UINT n, ID3D12CommandList* const* lists, uint64_t x, uint64_t y)
{
    std::vector<ID3D12CommandList*> r(n); for (UINT i = 0; i < n; i++) r[i] = (ID3D12CommandList*)Unwrap(lists[i]);
    ((ID3D12CommandQueue*)w->real)->ExecuteCommandLists(n, r.data());
    if (BootstrapNoopEnabled()) ++g_bootstrap.execute;
    static std::atomic<int> k{0}; if (k++ < 6) XS_LOG("queue slot35 ExecuteCommandListsX n=%u lists[0]=%p x=%#llx y=%#llx", n, n ? (void*)lists[0] : nullptr, (unsigned long long)x, (unsigned long long)y);
    return S_OK;
}
// Device слот 8 (PC CreateCommandQueue): тоже оборачиваем.
static HRESULT __stdcall Dev_CreateCommandQueue8(Wrap* w, const D3D12_COMMAND_QUEUE_DESC* d, REFIID riid, void** out)
{
    HRESULT hr = ((ID3D12Device*)w->real)->CreateCommandQueue(d, riid, out); InitObjVts(); hr = WrapOutput(hr, out, g_vtQueue, 2);
    XS_LOG("DEVICE slot8 CreateCommandQueue(PC) type=%d -> hr=0x%08lX q=%p", d ? (int)d->Type : -1, (unsigned long)hr, out ? *out : nullptr); return hr;
}

// Device слот 80 (+0x280) = WaitFrameEventX-подобный (по анализу пользователя): (this, type, timeout, p3, [rsp+0x20]=flags, [rsp+0x28]=UINT64* frameToken).
// Выход — frame token (не очередь!); игра использует его как R9 в helper 0x2482660 и индекс (token&0xf). Реализация: монотонный токен, пейсинг ~60 Гц.
static HRESULT __stdcall Dev_WaitFrameEvent80(Wrap*, uintptr_t type, uintptr_t timeout, uintptr_t a3, uintptr_t flags, uint64_t* out)
{
    static std::atomic<uint64_t> tok{0}; static std::atomic<int> n{0}; static LARGE_INTEGER f = {}, last = {};
    if (!f.QuadPart) { QueryPerformanceFrequency(&f); QueryPerformanceCounter(&last); }
    LARGE_INTEGER now; QueryPerformanceCounter(&now); LONGLONG want = f.QuadPart / 60;
    if (now.QuadPart - last.QuadPart < want) Sleep((DWORD)((want - (now.QuadPart - last.QuadPart)) * 1000 / f.QuadPart));
    QueryPerformanceCounter(&last);
    uint64_t t = ++tok; if (out && !IsBadWritePtr(out, 8)) *out = t;
    if (n++ < 4) XS_LOG("DEVICE slot80 WaitFrameEventX type=%llu timeout=%#llx flags=%llu -> token=%llu", (unsigned long long)type, (unsigned long long)timeout, (unsigned long long)flags, (unsigned long long)t);
    return S_OK;
}
// Device слот 81 (+0x288): (this, queue, id=0x11100, uint32* count [in=6], void* out) -> HRESULT [callsite 0x142482710]. Игра требует S_OK, count!=0 и в out[0] значение 0x10000 или 0x1000 (похоже на размер страницы/выравнивание).
// ПРЕДПОЛОЖЕНИЕ (семантика не подтверждена): count=1, out[0]=0x10000, остальное 0.
static HRESULT __stdcall Dev_QueueInfo81(Wrap*, void* queue, uint32_t id, uint32_t* count, void* out)
{
    if (count && !IsBadWritePtr(count, 4)) *count = 1; if (out && !IsBadWritePtr(out, 16)) { memset(out, 0, 16); *(uint32_t*)out = 0x10000; }
    static std::atomic<int> n{0}; if (n++ < 3) XS_LOG("DEVICE slot81 queue-info id=%#x queue=%p -> count=1 out[0]=0x10000 (ПРЕДПОЛОЖЕНИЕ)", id, queue);
    return S_OK;
}
static void InitDevVt()
{
    static SRWLOCK lock = SRWLOCK_INIT;
    ExclusiveSrwLock held(lock);
    static bool done = false; if (done) return; done = true;
    FillUnknown(std::make_integer_sequence<int, kDevSlots>{});
    void (*fw[44])() = { xs_fwd_0, xs_fwd_1, xs_fwd_2, xs_fwd_3, xs_fwd_4, xs_fwd_5, xs_fwd_6, xs_fwd_7, xs_fwd_8, xs_fwd_9, xs_fwd_10, xs_fwd_11, xs_fwd_12, xs_fwd_13, xs_fwd_14,
        xs_fwd_15, xs_fwd_16, xs_fwd_17, xs_fwd_18, xs_fwd_19, xs_fwd_20, xs_fwd_21, xs_fwd_22, xs_fwd_23, xs_fwd_24, xs_fwd_25, xs_fwd_26, xs_fwd_27, xs_fwd_28, xs_fwd_29, xs_fwd_30,
        xs_fwd_31, xs_fwd_32, xs_fwd_33, xs_fwd_34, xs_fwd_35, xs_fwd_36, xs_fwd_37, xs_fwd_38, xs_fwd_39, xs_fwd_40, xs_fwd_41, xs_fwd_42, xs_fwd_43 };
    for (int i = 0; i < 44; i++) g_devVt[i] = (void*)fw[i];
    if (getenv("XSX_DEV_TRACE")) { g_trace = true; FillTrace(std::make_integer_sequence<int, 44>{}); }
    g_devVt[0] = (void*)Dev_QI; g_devVt[43] = (void*)Dev_GetAdapterLuid; g_devVt[76] = (void*)Dev_CreateCommittedResource76; g_devVt[48] = (void*)Dev_CreateCommandQueue48; g_devVt[9] = (void*)Dev_CreateCommandAllocator9; g_devVt[14] = (void*)Dev_CreateDescriptorHeap14;
    g_devVt[55] = (void*)Dev_PsoUpdates55; g_devVt[57] = (void*)Dev_NativePso57;
    g_devVt[36] = (void*)Dev_CreateFence36;
    g_devVt[41] = (void*)Dev_CreateCommandSignature41; g_devVt[8] = (void*)Dev_CreateCommandQueue8; g_devVt[80] = (void*)Dev_WaitFrameEvent80; g_devVt[81] = (void*)Dev_QueueInfo81; g_devVt[12] = (void*)Dev_CreateCommandList12; g_devVt[25] = (void*)Dev_GetResourceAllocationInfo25; g_devVt[108] = (void*)Dev_GetResourceAllocationInfo108; g_devVt[61] = (void*)Dev_CreateSamplerBlob61; g_devVt[62] = (void*)Dev_NativePso62; g_devVt[74] = (void*)Dev_HeapFromRange74; g_devVt[86] = (void*)Dev_NativePso86; g_devVt[67] = (void*)Dev_NativePso67; g_devVt[127] = (void*)Dev_Slot127; g_devVt[133] = (void*)Dev_CreatePlacedResourceX133; g_devVt[78] = (void*)Dev_FramePacing; g_devVt[79] = (void*)Dev_FramePacing;
    g_devVt[17] = (void*)Dev_CreateCBV; g_devVt[18] = (void*)Dev_CreateSRV; g_devVt[19] = (void*)Dev_CreateUAV; g_devVt[20] = (void*)Dev_CreateRTV; g_devVt[21] = (void*)Dev_CreateDSV;
    g_devVt[22] = (void*)Dev_CreateSampler; g_devVt[23] = (void*)Dev_CopyDescriptors; g_devVt[24] = (void*)Dev_CopyDescriptorsSimple; g_devVt[73] = (void*)Dev_CreateCommandList73;
}

// ---- D3D12XboxCreateDevice(adapter, params, riid, ppDevice) ----
// [Гипотеза] layout D3D12XBOX_CREATE_DEVICE_PARAMETERS начинается с UINT Version (SDK version).
// Остальные поля (ring sizes, debug flags, DXR flags) на PC не имеют аналога и игнорируются.
// ВНИМАНИЕ: возвращается обычный PC device. Xbox vtable (напр. слот 76 CreateCommittedResource)
// и native PSO/resource flags этим слоем НЕ адаптируются — это отдельный ABI-адаптер.
extern "C" HRESULT WINAPI xsXboxCreateDevice(
    IUnknown* adapter, const void* params, REFIID riid, void** ppDevice)
{
    static PVOID fault_trace = AddVectoredExceptionHandler(1, &TraceGraphicsFault); 
    (void)fault_trace;
    CALLER(c);
    XS_LOG("XboxCreateDevice adapter=%p params=%p riid=%08lx out=%p caller=%s vm_configured_before=%ld",
           (void*)adapter, params, (unsigned long)riid.Data1, ppDevice, c, g_vmCalls);
    // Layout D3D12XBOX_CREATE_DEVICE_PARAMETERS не подтверждён: только hex-дамп первых 32 байт.
    // Игра (callsite RVA 0x24785b7) обнуляет 0x50 байт и пишет: +0x00=0x20004, +0x08=0x100000,
    // +0x20/+0x24=bool-флаги из настроек, +0x28=1, +0x3c=0x20. Дампим 0x50 байт.
    if (params && !IsBadReadPtr(params, 0x50)) {
        const uint32_t* w = static_cast<const uint32_t*>(params);
        for (int i = 0; i < 0x50 / 4; i += 4)
            XS_LOG("  params+0x%02x: %08x %08x %08x %08x", i * 4, w[i], w[i+1], w[i+2], w[i+3]);
    }
    if (!ppDevice) return E_INVALIDARG;
    *ppDevice = nullptr;

    using CreateFn = HRESULT (WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
    static auto create = Resolve<CreateFn>("D3D12CreateDevice");
    if (!create) { XS_LOG("  D3D12CreateDevice not found"); return E_NOTIMPL; }

    // Xbox Series = RDNA2 с DXR; пробуем 12_1, затем 12_0.
    HRESULT hr = create(nullptr, D3D_FEATURE_LEVEL_12_1, riid, ppDevice);
    if (FAILED(hr)) hr = create(nullptr, D3D_FEATURE_LEVEL_12_0, riid, ppDevice);
    if (SUCCEEDED(hr)) InterlockedIncrement(&g_deviceCreated);
    const char* wrapEnv = getenv("XSX_DEVICE_WRAP");
    if (SUCCEEDED(hr) && *ppDevice && !(wrapEnv && !strcmp(wrapEnv, "0"))) {
        InitDevVt();
        hr = WrapOutput(hr, ppDevice, g_devVt, 1);
        if (FAILED(hr)) return hr;
        Wrap* w = (Wrap*)*ppDevice; g_devWrap = w;
        XS_LOG("  device WRAPPED: wrapper=%p real=%p (192-слотовая Xbox-vtable; XSX_DEVICE_WRAP=0 отключает) vt[25]=%p vt[26]=%p", (void*)w, w->real, g_devVt[25], g_devVt[26]);
    }
    XS_LOG("  -> 0x%08lX device=%p", (unsigned long)hr, ppDevice ? *ppDevice : nullptr);
    return hr;
}

// ---- D3D12ConfigureVirtualMemoryX(const void* cfg) -> HRESULT ----
// [Статически из game_scarlett_master.exe, callsite RVA 0x1e1cfe0] Один аргумент RCX: указатель на
// 32-байтовую структуру на стеке, обнулённую, с dword +4 = N+2, где N = ceil(size/16GiB),
// size = 8 GiB (или 16/24 GiB — зависит от результата функции RVA 0x75140). Результат проверяется
// `test eax,eax; jns ok; int3` — любой отказ ФАТАЛЕН для игры, поэтому E_NOTIMPL недопустим.
// Сразу после вызова игра делает XMemVirtualAlloc(0, size, MEM_RESERVE|MEM_PHYSICAL|MEM_LARGE_PAGES, 0x21, PAGE_READWRITE).
// Вывод: no-op S_OK валиден как политика; настоящая семантика (резерв GPU VA) на PC эмулируется самим XMem shim.
// Политики notimpl/fail оставлены только для диагностики (игра на них упадёт).
extern "C" HRESULT WINAPI xsConfigureVirtualMemoryX(const void* cfg)
{
    CALLER(c);
    InterlockedIncrement(&g_vmCalls);
    const char* pol = getenv("XSX_VM_POLICY");
    HRESULT hr = S_OK;
    if (pol && !strcmp(pol, "notimpl")) hr = E_NOTIMPL;
    else if (pol && !strcmp(pol, "fail")) hr = E_FAIL;
    uint32_t w[8] = {};
    if (cfg && !IsBadReadPtr(cfg, 32)) memcpy(w, cfg, 32);
    XS_LOG("ConfigureVirtualMemoryX cfg=%p [%08x %08x %08x %08x %08x %08x %08x %08x] caller=%s call#%ld device_already_created=%ld policy=%s -> 0x%08lX",
           cfg, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7], c, g_vmCalls, g_deviceCreated,
           pol ? pol : "ok", (unsigned long)hr);
    if (g_deviceCreated) XS_LOG("  WARNING: вызван ПОСЛЕ создания device");
    return hr;
}

// Ранняя установка диагностических трапов (XSX_TRAP): до создания синглтонов игры.
extern "C" BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) InstallTraps();
    return TRUE;
}
