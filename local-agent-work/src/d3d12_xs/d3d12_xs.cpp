#include <vector>
#include <map>
#include <set>
#include <string>
#include <array>
#include <chrono>
#include <thread>
#include <memory>
#include "frame_clock.h"
#include "guest_footprint.h"
#include "surface_blit.h"
// d3d12_xs.dll — прототип слоя совместимости Xbox Series D3D12 -> PC D3D12 (VKD3D-Proton).
// Экспорты соответствуют импортам game_scarlett_master.exe и Xbox FidelityFX DLL.
// Сигнатуры D3D12XboxCreateDevice/D3D12ConfigureVirtualMemoryX восстановлены по
// использованию в GDK samples (публичных заголовков d3d12_xs.h в checkout нет).
#include <atomic>
#include <dxgi1_4.h>
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

#include "return_trace.h"
static int ReturnTraceMode()
{
    static const int mode = [] {
        char value[8]{};
        GetEnvironmentVariableA("XSX_TRACE_RETURNS", value, sizeof value);
        return value[0] == '2' ? 2 : value[0] == '1' ? 1 : 0;
    }();
    return mode;
}
static uint32_t ReturnTraceGetError() { return GetLastError(); }
static void ReturnTraceSetError(uint32_t error) { SetLastError(error); }
static bool ReturnTraceReadWord(uintptr_t address, uintptr_t& value)
{
    SIZE_T read = 0;
    return ReadProcessMemory(GetCurrentProcess(), (const void*)address, &value, sizeof value, &read) && read == sizeof value;
}
static void ReturnTraceEmit(const xsx::returns::Event& event)
{
    static const char* names[] = { "export", "device", "queue", "list", "heap", "pso", "allocator", "fence", "resource" };
    char caller[300]; CallerStr((void*)event.caller, caller, sizeof caller);
    // One stdio operation keeps this record together across game threads.
    std::fprintf(stderr, "[D3D12_XS] RET %s call=%llu end=%llu failure=%llu tick=%llu tid=%lu %s slot=%d method=%s object=%p hr=0x%08lx value=%p out=%p output=%p readable=%u caller=%s\n",
        xsx::returns::FindingName(event.finding), (unsigned long long)event.call, (unsigned long long)event.returned,
        (unsigned long long)event.failure, (unsigned long long)GetTickCount64(), GetCurrentThreadId(),
        names[event.site.kind], event.site.slot, event.site.method, (void*)event.object,
        (unsigned long)(uint32_t)event.value, (void*)event.value, (void*)event.output,
        (void*)event.output_value, event.output_readable, caller);
    std::fflush(stderr);
}
static void ConfigureReturnTrace()
{
    static const bool initialized = [] {
        xsx::returns::hooks = {ReturnTraceMode, ReturnTraceGetError, ReturnTraceSetError, ReturnTraceReadWord, ReturnTraceEmit};
        return true;
    }();
    (void)initialized;
}
static void ReturnTraceSnapshot()
{
    if (!ReturnTraceMode()) return;
    std::fprintf(stderr, "[D3D12_XS] RET SNAPSHOT methods=%llu started=%llu returned=%llu failures=%llu (typed guest-facing D3D12 returns; unresolved/void contracts excluded)\n",
        (unsigned long long)xsx::returns::installed.load(), (unsigned long long)xsx::returns::calls.load(),
        (unsigned long long)xsx::returns::returned.load(), (unsigned long long)xsx::returns::failures.load());
}

// First-chance logging only: the game's own crash handler sometimes emits an
// empty .dmp. Never handle/skip the fault or alter the supplied CONTEXT.
extern "C" { extern uintptr_t g_arena_lo, g_arena_hi; }
struct ExclusiveSrwLock {
    SRWLOCK* lock;
    explicit ExclusiveSrwLock(SRWLOCK& value) : lock(&value) { AcquireSRWLockExclusive(lock); }
    ~ExclusiveSrwLock() { ReleaseSRWLockExclusive(lock); }
    ExclusiveSrwLock(const ExclusiveSrwLock&) = delete;
    ExclusiveSrwLock& operator=(const ExclusiveSrwLock&) = delete;
};
struct SharedSrwLock { SRWLOCK* l; explicit SharedSrwLock(SRWLOCK& s) : l(&s) { AcquireSRWLockShared(l); } ~SharedSrwLock() { ReleaseSRWLockShared(l); } };
static std::map<uintptr_t, std::pair<DWORD, uintptr_t>> g_freed; static SRWLOCK g_freedLock = SRWLOCK_INIT; static uintptr_t g_freeFn = 0;
static void DumpAllThreads();
static void FrameClockSnapshot();
#include "writer_task_trace.h"
#include "writer_fault.h"
static LONG CALLBACK TraceGraphicsFault(EXCEPTION_POINTERS* exception)
{
    const DWORD code = exception->ExceptionRecord->ExceptionCode;
    // Our observer handles these injected instructions later in the VEH chain.
    // They must not consume the fatal-breakpoint budget or trigger crash dumps.
    if (IsWriterTaskBreakpoint(exception)) return EXCEPTION_CONTINUE_SEARCH;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_IN_PAGE_ERROR && code != EXCEPTION_BREAKPOINT) {
        if (code == 0x40010006 || code == 0x4001000A || code == 0x406D1388 || code == EXCEPTION_SINGLE_STEP || code == 0x80000004) return EXCEPTION_CONTINUE_SEARCH;
        static volatile LONG other = 0; if (InterlockedIncrement(&other) <= 20) { char where[300]; CallerStr(exception->ExceptionRecord->ExceptionAddress, where, sizeof where); XS_LOG("OTHER-EXCEPTION code=0x%08lX thread=%lu at=%s", (unsigned long)code, GetCurrentThreadId(), where); }
        return EXCEPTION_CONTINUE_SEARCH;
    }
    static volatile LONG active = 0, count = 0, breakpoints = 0;
    if (InterlockedCompareExchange(&active, 1, 0)) return EXCEPTION_CONTINUE_SEARCH;
    if (code != EXCEPTION_BREAKPOINT && exception->ExceptionRecord->NumberParameters > 1 && exception->ExceptionRecord->ExceptionInformation[1] == ~(ULONG_PTR)0) {   // фоновые AV по адресу -1 только из kernel32+0x20cad не считаем
        char w[200]; CallerStr(exception->ExceptionRecord->ExceptionAddress, w, sizeof w); if (!strcmp(w, "kernel32.dll+0x20cad")) { InterlockedExchange(&active, 0); return EXCEPTION_CONTINUE_SEARCH; } }
    if (InterlockedIncrement(code == EXCEPTION_BREAKPOINT ? &breakpoints : &count) <= 8) {
        const CONTEXT& r = *exception->ContextRecord;
        char where[300]; CallerStr((void*)r.Rip, where, sizeof where);
        XS_LOG("FIRST-CHANCE code=0x%08lX thread=%lu at=%s access=%llu address=%p", (unsigned long)code,
            GetCurrentThreadId(), where, exception->ExceptionRecord->NumberParameters ?
            (unsigned long long)exception->ExceptionRecord->ExceptionInformation[0] : 0,
            exception->ExceptionRecord->NumberParameters > 1 ? (void*)exception->ExceptionRecord->ExceptionInformation[1] : nullptr);
        ReturnTraceSnapshot();
        FrameClockSnapshot();
        if (code == EXCEPTION_BREAKPOINT) DumpWriterFault(r);
        XS_LOG("FAULT REG rip=%p rsp=%p rbp=%p rax=%p rbx=%p rcx=%p rdx=%p", (void*)r.Rip,
            (void*)r.Rsp, (void*)r.Rbp, (void*)r.Rax, (void*)r.Rbx, (void*)r.Rcx, (void*)r.Rdx);
        XS_LOG("FAULT REG rsi=%p rdi=%p r8=%p r9=%p r10=%p r11=%p r12=%p r13=%p r14=%p r15=%p",
            (void*)r.Rsi, (void*)r.Rdi, (void*)r.R8, (void*)r.R9, (void*)r.R10, (void*)r.R11,
            (void*)r.R12, (void*)r.R13, (void*)r.R14, (void*)r.R15);
        if (code == EXCEPTION_ACCESS_VIOLATION && (uintptr_t)r.Rip == (uintptr_t)GetModuleHandleA(nullptr) + 0xb5a5f8) {
            uintptr_t b0 = (uintptr_t)GetModuleHandleA(nullptr); uint64_t g[24] = {}; ReadProcessMemory(GetCurrentProcess(), (void*)(b0 + 0x75d5980), g, sizeof g, nullptr);
            for (int i = 0; i < 24; i++) XS_LOG("GLOBAL exe+%#x = %p", 0x75d5980 + i * 8, (void*)g[i]);
        }
        if (code == EXCEPTION_BREAKPOINT && (uintptr_t)r.Rip == (uintptr_t)GetModuleHandleA(nullptr) + 0x187fb) {
            uintptr_t b0 = (uintptr_t)GetModuleHandleA(nullptr); uint64_t hdr[2] = {}; ReadProcessMemory(GetCurrentProcess(), (void*)(r.R14 - 0x10), hdr, 16, nullptr);
            XS_LOG("BADFREE@187fb ptr=%p hdr=%016llx %016llx tid=%lu", (void*)r.R14, (unsigned long long)hdr[0], (unsigned long long)hdr[1], GetCurrentThreadId());
            { SharedSrwLock gg(g_freedLock); auto it = g_freed.find(r.R14); if (it != g_freed.end()) XS_LOG("BADFREE@187fb ПРЕДЫДУЩИЙ вызов FreeChecked с этим ptr: tid=%lu ret=exe+%#llx", it->second.first, (unsigned long long)(it->second.second - b0)); else XS_LOG("BADFREE@187fb: этот ptr раньше в FreeChecked не приходил"); } }
        if (code == EXCEPTION_BREAKPOINT) { static volatile LONG once = 0; char v[4]; if (GetEnvironmentVariableA("XSX_THREAD_DUMP", v, sizeof v) && !InterlockedExchange(&once, 1)) DumpAllThreads(); }
        if (code == EXCEPTION_BREAKPOINT && (uintptr_t)r.Rip == (uintptr_t)GetModuleHandleA(nullptr) + 0x55aa5eb) {
            unsigned char b[256] = {}; SIZE_T u = 0; ReadProcessMemory(GetCurrentProcess(), (void*)(r.Rcx - 0x40), b, sizeof b, &u);
            for (int row = 0; row < 16; row++) { char h[80] = {}; for (int q = 0; q < 16; q++) sprintf(h + q * 3, "%02x ", b[row * 16 + q]); XS_LOG("HEAPNODE %p: %s", (void*)(r.Rcx - 0x40 + row * 16), h); }
            { static unsigned char big[0x4000]; SIZE_T u2 = 0; ReadProcessMemory(GetCurrentProcess(), (void*)(r.Rcx - 0x2000), big, sizeof big, &u2);
              HANDLE f = CreateFileA("T:\\heapnode.bin", GENERIC_WRITE, 0, 0, CREATE_ALWAYS, 0, 0); DWORD wr; if (f != INVALID_HANDLE_VALUE) { WriteFile(f, big, (DWORD)u2, &wr, 0); CloseHandle(f); } }
            MEMORY_BASIC_INFORMATION mbi{}; VirtualQuery((void*)r.Rcx, &mbi, sizeof mbi); XS_LOG("HEAPNODE region base=%p alloc=%p size=%#zx type=%#lx", mbi.BaseAddress, mbi.AllocationBase, mbi.RegionSize, mbi.Type);
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
        { const uintptr_t regs[] = { r.Rcx, r.Rdx, r.Rbx, r.Rsi, r.Rdi, r.R14, r.R15 }; const char* nm[] = { "rcx", "rdx", "rbx", "rsi", "rdi", "r14", "r15" };
          for (int q = 0; q < 7; q++) { uintptr_t v = regs[q]; if (v < 0x10000 || IsBadReadPtr((void*)v, 64)) continue; bool arena = v >= g_arena_lo && v < g_arena_hi; const uint64_t* w = (const uint64_t*)v;
            XS_LOG("FAULT OBJ %s=%p%s: %016llx %016llx %016llx %016llx %016llx %016llx", nm[q], (void*)v, arena ? " [ARENA kind=" : "", (unsigned long long)w[0], (unsigned long long)w[1], (unsigned long long)w[2], (unsigned long long)w[3], (unsigned long long)w[4], (unsigned long long)w[5]); } }
        { uintptr_t b0 = (uintptr_t)GetModuleHandleA(nullptr); int shown = 0; for (unsigned i = 0; i < 1024 && shown < 40; ++i) { uintptr_t v = 0; SIZE_T u = 0;
            if (!ReadProcessMemory(GetCurrentProcess(), (void*)(r.Rsp + i * 8), &v, 8, &u) || u != 8) break; if (v > b0 + 0x1000 && v < b0 + 0x8000000) { XS_LOG("DEEPSTACK +%#x exe+%#llx", i * 8, (unsigned long long)(v - b0)); shown++; } } }
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
static uintptr_t g_watchTable = 0; static TrapEnt g_traps[16]; static int g_nTraps = 0; static thread_local uintptr_t g_rearm = 0; static volatile LONG g_trapN = 0;
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

// Дамп стеков всех потоков при первом EXCEPTION_BREAKPOINT (если XSX_THREAD_DUMP=1): RIP и адреса exe из первых 256 qword стека.
static void DumpAllThreads()
{
    struct Rec { DWORD tid; uintptr_t rip, rsp; std::vector<std::pair<int, uintptr_t>> frames; };
    std::vector<Rec> recs; uintptr_t b0 = (uintptr_t)GetModuleHandleA(nullptr);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0); if (snap == INVALID_HANDLE_VALUE) return; THREADENTRY32 te{sizeof te};
    std::vector<HANDLE> susp;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == GetCurrentThreadId()) continue;
        HANDLE h = OpenThread(THREAD_ALL_ACCESS, FALSE, te.th32ThreadID); if (!h) continue;
        if (SuspendThread(h) == (DWORD)-1) { CloseHandle(h); continue; }
        susp.push_back(h); Rec r{te.th32ThreadID, 0, 0, {}}; CONTEXT c{}; c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        if (GetThreadContext(h, &c)) { r.rip = c.Rip; r.rsp = c.Rsp; int shown = 0;
            for (int q = 0; q < 512 && shown < 16; q++) { uintptr_t v = 0; if (!ReadProcessMemory(GetCurrentProcess(), (void*)(c.Rsp + q * 8), &v, 8, nullptr)) break; if (v > b0 && v < b0 + 0x8000000) { r.frames.push_back({q * 8, v - b0}); shown++; } } }
        recs.push_back(std::move(r));
    }
    CloseHandle(snap);
    for (HANDLE h : susp) { ResumeThread(h); CloseHandle(h); }
    for (auto& r : recs) { char who[200]; CallerStr((void*)r.rip, who, sizeof who); XS_LOG("THREAD %lu rip=%s", r.tid, who); for (auto& f : r.frames) XS_LOG("THREAD %lu   stk+%#x exe+%#llx", r.tid, f.first, (unsigned long long)f.second); }
}

// ---- Диагностика двойного освобождения в аллокаторе игры «Global» (объект в exe+0xabcc310; метод Free = vtable+0x60 по коду 0x14455aeb0) ----
// XSX_FREE_TRACE=1: поток ждёт появления объекта, ставит трап на Free; на каждом вызове запоминает (адрес → поток, вызывающий); повторное освобождение того же адреса
// без промежуточного выделения (выделение не отслеживаем — возможны ложные срабатывания при реюзе адреса) печатается как DOUBLEFREE с обоими вызывающими.
static void OnFreeTrap(CONTEXT& r)
{
    uintptr_t ra = 0; ReadProcessMemory(GetCurrentProcess(), (void*)r.Rsp, &ra, 8, nullptr); uintptr_t p = r.Rdx; if (!p) return;
    ExclusiveSrwLock g(g_freedLock); g_freed[p] = { GetCurrentThreadId(), ra };
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
    if (g_freeFn && (uintptr_t)r.Rip == g_freeFn) { OnFreeTrap(r);
        for (int i = 0; i < g_nTraps; i++) if (g_traps[i].addr == g_freeFn) { DWORD o; void* a = (void*)g_freeFn; VirtualProtect(a, 1, PAGE_EXECUTE_READWRITE, &o); *(unsigned char*)a = g_traps[i].orig; VirtualProtect(a, 1, o, &o); g_rearm = g_freeFn; r.EFlags |= 0x100; return EXCEPTION_CONTINUE_EXECUTION; } }
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
            if ((uintptr_t)r.Rip == (uintptr_t)GetModuleHandleA(nullptr) + 0x28cfaf2) {
                uintptr_t vt = 0, fn = 0; ReadProcessMemory(GetCurrentProcess(), (void*)r.Rcx, &vt, 8, nullptr); ReadProcessMemory(GetCurrentProcess(), (void*)(vt + 0x230), &fn, 8, nullptr);
                char a1[160], a2[160]; CallerStr((void*)vt, a1, sizeof a1); CallerStr((void*)fn, a2, sizeof a2);
                uint64_t st[4] = {}; ReadProcessMemory(GetCurrentProcess(), (void*)(r.Rsp + 0x20), st, sizeof st, nullptr);
                static std::atomic<int> k{0}; if (k++ < 4000) XS_LOG("BLOCKREAD obj=%p vt=%s fn=%s edx=%#llx file=%p off=%#llx a5=%#llx a6=%p a7=%p comp=%#llx", (void*)r.Rcx, a1, a2, (unsigned long long)r.Rdx, (void*)r.R8, (unsigned long long)r.R9, (unsigned long long)st[0], (void*)st[1], (void*)st[2], (unsigned long long)(st[3] & 0xff));
            }
            if ((uintptr_t)r.Rip == (uintptr_t)GetModuleHandleA(nullptr) + 0x4dcbc40) {
                uint32_t f[8] = {}; uintptr_t data = 0; ReadProcessMemory(GetCurrentProcess(), (void*)(r.Rcx), f, sizeof f, nullptr); ReadProcessMemory(GetCurrentProcess(), (void*)(r.Rcx + 8), &data, 8, nullptr);
                uintptr_t ra = 0; ReadProcessMemory(GetCurrentProcess(), (void*)r.Rsp, &ra, 8, nullptr);
                static std::atomic<int> nl{0}; if (f[4] - f[6] < 0x2000 || nl++ < 5) XS_LOG("LINBUF tid=%lu obj=%p data=%p cap=%#x used=%#x req=%#x ret=exe+%#llx", GetCurrentThreadId(), (void*)r.Rcx, (void*)data, f[4], f[6], (unsigned)r.R8, (unsigned long long)(ra - (uintptr_t)GetModuleHandleA(nullptr)));
            }
            if ((uintptr_t)r.Rip == (uintptr_t)GetModuleHandleA(nullptr) + 0x4e52a40) {   // flush локального буфера задачи в общий поток
                uint32_t used = 0; uintptr_t data = 0; ReadProcessMemory(GetCurrentProcess(), (void*)(r.Rcx + 0x18), &used, 4, nullptr); ReadProcessMemory(GetCurrentProcess(), (void*)(r.Rcx + 8), &data, 8, nullptr);
                static std::atomic<int> nf{0}; int k = nf++;
                if (used == 0x6c0 && k < 4000) { static std::atomic<int> nd{0}; if (nd++ < 3) { unsigned char b[0x6c0] = {}; ReadProcessMemory(GetCurrentProcess(), (void*)data, b, used, nullptr);
                    char path[64]; snprintf(path, sizeof path, "T:\\chunk6c0_%d.bin", nd.load()); HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, 0, 0); DWORD wr; if (f != INVALID_HANDLE_VALUE) { WriteFile(f, b, used, &wr, 0); CloseHandle(f); } } }
                if (k < 200) XS_LOG("FLUSH tid=%lu local=%p used=%#x -> target=%p", GetCurrentThreadId(), (void*)r.Rcx, used, (void*)r.Rdx);
            }
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
static DWORD WINAPI FreeTraceArm(void*)
{
    uintptr_t b0 = (uintptr_t)GetModuleHandleA(nullptr);
    { char v[8] = {}; GetEnvironmentVariableA("XSX_FREE_TRACE", v, sizeof v); if (v[0] == '2') { uintptr_t fn = b0 + 0x187b0; DWORD o; if (VirtualProtect((void*)fn, 1, PAGE_EXECUTE_READWRITE, &o)) { g_traps[g_nTraps].addr = fn; g_traps[g_nTraps].wantRet = false; g_traps[g_nTraps].orig = *(unsigned char*)fn; g_freeFn = fn; *(unsigned char*)fn = 0xCC; VirtualProtect((void*)fn, 1, o, &o); g_nTraps++; XS_LOG("FREE-TRACE(2): трап на FreeChecked exe+0x187b0"); } return 0; } }
    for (int i = 0; i < 12000; i++) { Sleep(10); uintptr_t obj = 0, vt = 0, fn = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), (void*)(b0 + 0xabcc310), &obj, 8, nullptr) || !obj) continue;
        if (!ReadProcessMemory(GetCurrentProcess(), (void*)obj, &vt, 8, nullptr) || !vt || !ReadProcessMemory(GetCurrentProcess(), (void*)(vt + 0x60), &fn, 8, nullptr) || !fn) continue;
        for (int k = 0x40; k <= 0x78; k += 8) { uintptr_t f2 = 0; ReadProcessMemory(GetCurrentProcess(), (void*)(vt + k), &f2, 8, nullptr); XS_LOG("GLOBALHEAP vt[%#x]=exe+%#llx", k, (unsigned long long)(f2 - b0)); }
        DWORD o; if (VirtualProtect((void*)fn, 1, PAGE_EXECUTE_READWRITE, &o)) { g_traps[g_nTraps].addr = fn; g_traps[g_nTraps].wantRet = false; g_traps[g_nTraps].orig = *(unsigned char*)fn; g_freeFn = fn; *(unsigned char*)fn = 0xCC; VirtualProtect((void*)fn, 1, o, &o); g_nTraps++; XS_LOG("FREE-TRACE: трап на Free = exe+%#llx", (unsigned long long)(fn - b0)); }
        return 0; }
    return 0;
}
static void InstallTraps()
{
    InstallWriterTaskTrace();
    { char v[4]; if (GetEnvironmentVariableA("XSX_FREE_TRACE", v, sizeof v)) { AddVectoredExceptionHandler(1, &TrapHandler); CreateThread(nullptr, 0, FreeTraceArm, nullptr, 0, nullptr); } }
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
static inline void* Unwrap(void* p) { uintptr_t u = (uintptr_t)p; return (u >= g_arena_lo && u < g_arena_hi) ? ((Wrap*)p)->real : p; }
static const int kObjSlots = 192;
static void* g_vtRes[kObjSlots];
static void InitObjVts();
static HRESULT WrapOutput(HRESULT hr, void** out, void** vt, int kind);
// gfx_native.h (режим XSX_DEV_NATIVE_GFX) — объявлены здесь, определены в include внизу файла
static bool GfxNativeEnabled();
static bool GfxNativeMinimal();
static bool GfxRecordOn();
static HRESULT GfxNativeCreatePso57(Wrap* w, const void* desc, REFIID iid, void** out);
static bool GfxNativeOnSetPso(Wrap* w, ID3D12PipelineState* p);
enum class GfxDrawResult { Unrecognized, Ready, Incomplete };
static GfxDrawResult GfxNativeOnDrawBind(Wrap* w, bool indexed, UINT vertices, UINT start, UINT* outVertices, UINT* outStart);
static HRESULT GfxNativeSubmit(Wrap* w, UINT n, ID3D12CommandList* const* lists);
static void GfxNativeOnReset(Wrap* w);
static void GfxNativeRememberRtv(ID3D12Resource*, const D3D12_RENDER_TARGET_VIEW_DESC*, uint64_t guest, uint64_t native);
static void GfxNativeRememberSrv(ID3D12Resource*, const D3D12_SHADER_RESOURCE_VIEW_DESC*, uint64_t guest, uint64_t native);
static void GfxNativeInstallListHooks();
static void GfxNativeOnPresent(ID3D12Resource* nativeSource, unsigned frame);
// Xbox-расширения DXGI_FORMAT (116..120 и т.п.) не существуют на PC → VKD3D отвергает ресурс/вид. Отображаем на ближайшие PC-форматы.
// 116 R10G10B10_7E3_A2_FLOAT, 117 R10G10B10_6E4_A2_FLOAT → R10G10B10A2_UNORM (неточно: float-каналы); 118 D16_UNORM_S8_UINT → D24_UNORM_S8_UINT;
// 119 R16_UNORM_X8_TYPELESS → R24_UNORM_X8_TYPELESS; 120 X16_TYPELESS_G8_UINT → X24_TYPELESS_G8_UINT; 189/190 R10G10B10_SNORM_A2_UNORM → R10G10B10A2_UNORM.
// Типless-база для D16S8 (если игра создаёт ресурс как D16_UNORM_S8_UINT, а виды — 119/120) → R24G8_TYPELESS.
static DXGI_FORMAT XboxFormatToPc(DXGI_FORMAT f, bool resource = false)
{
    switch ((int)f) {
    case 116: case 117: case 189: case 190: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case 118: return resource ? DXGI_FORMAT_R24G8_TYPELESS : DXGI_FORMAT_D24_UNORM_S8_UINT;
    case 119: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case 120: return DXGI_FORMAT_X24_TYPELESS_G8_UINT;
    default: return f;
    }
}
static void LogXboxFormat(const char* where, int f)
{
    static std::atomic<int> n{0}; if ((f >= 116 && f <= 129) || f >= 133) { if (n++ < 24) XS_LOG("FORMAT Xbox %d -> PC %d (%s)", f, (int)XboxFormatToPc((DXGI_FORMAT)f), where); }
}
static D3D12_RESOURCE_DESC PcResourceDesc(const D3D12_RESOURCE_DESC& xbox)
{
    D3D12_RESOURCE_DESC pc = xbox;
    LogXboxFormat("resource", (int)xbox.Format);
    pc.Format = XboxFormatToPc(xbox.Format, true);   // D16S8 -> R24G8_TYPELESS (виды 118/119/120 -> D24S8 / R24X8 / X24G8)
    // Preserve the existing FFX 0xc00000 stripping and also remove observed
    // Xbox 0x300000 allocation bits, which are unknown PC resource flags.
    pc.Flags = (D3D12_RESOURCE_FLAGS)((unsigned)xbox.Flags & 0x0fffff);
    if ((int)pc.Layout > 3) pc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    // Observed game depth target: R32G8X24_TYPELESS, flags=0x80300006.
    // PC D3D12 forbids DEPTH_STENCIL|UNORDERED_ACCESS on the same resource.
    // Allocate depth-only backing consistently in sizing AND creation. This
    // does not emulate Xbox UAV access to the depth surface (needs a shadow).
    if (xbox.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&   // (раньше только при Xbox-битах 0x300000; встречается и 0x800000)
        (xbox.Format == DXGI_FORMAT_R32G8X24_TYPELESS || (int)xbox.Format == 118 || xbox.Format == DXGI_FORMAT_R24G8_TYPELESS ||
         xbox.Format == DXGI_FORMAT_R32_TYPELESS || xbox.Format == DXGI_FORMAT_R16_TYPELESS || xbox.Format == DXGI_FORMAT_D32_FLOAT ||
         xbox.Format == DXGI_FORMAT_D24_UNORM_S8_UINT || xbox.Format == DXGI_FORMAT_D16_UNORM || xbox.Format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT) &&
        ((unsigned)pc.Flags & (D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) ==
        (D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) {
        pc.Flags = (D3D12_RESOURCE_FLAGS)((unsigned)pc.Flags & ~D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        static std::atomic<unsigned> calls{0};
        if (calls++ < 8) XS_LOG("RESOURCE depth backing flags=%#x->%#x (Xbox depth UAV unsupported; shadow required)",
            (unsigned)xbox.Flags, (unsigned)pc.Flags);
    }
    return pc;
}
static bool GuestColorFootprint(const D3D12_RESOURCE_DESC& desc, D3D12_RESOURCE_ALLOCATION_INFO& out)
{
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.MipLevels != 1 ||
        desc.SampleDesc.Count != 1 || desc.SampleDesc.Quality ||
        !(desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) ||
        (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) return false;
    unsigned bytes = 0;
    const unsigned format = (unsigned)desc.Format;
    if (format >= 1 && format <= 4) bytes = 16;
    else if (format >= 9 && format <= 14) bytes = 8;
    else if ((format >= 23 && format <= 32) || (format >= 87 && format <= 93) ||
        format == 116 || format == 117 || format == 189) bytes = 4;
    else if (format >= 48 && format <= 59) bytes = 2;
    else if (format >= 60 && format <= 65) bytes = 1;
    xsx::memory::Footprint info;
    if (!xsx::memory::Color64K(desc.Width, desc.Height, desc.DepthOrArraySize,
        bytes, (unsigned)desc.Layout, desc.Alignment, info)) return false;
    out.SizeInBytes = info.size; out.Alignment = info.alignment;
    return true;
}
static HRESULT __stdcall Dev_CreateCommittedResource76(Wrap* w, const D3D12_HEAP_PROPERTIES* hp, D3D12_HEAP_FLAGS hf, const D3D12_RESOURCE_DESC* rd,
                                                       D3D12_RESOURCE_STATES st, const D3D12_CLEAR_VALUE* cv, REFIID riid, void** out)
{
    D3D12_RESOURCE_DESC d = PcResourceDesc(*rd);
    HRESULT hr = ((ID3D12Device*)w->real)->CreateCommittedResource(hp, hf, &d, st, cv, riid, out);
    if (SUCCEEDED(hr) && out && *out && IsEqualGUID(riid, __uuidof(ID3D12Resource))) { InitObjVts(); hr = WrapOutput(hr, out, g_vtRes, 8); }
    XS_LOG("DEVICE slot76 CreateCommittedResource dim=%d %llux%u fmt=%d flags=0x%x->0x%x state=0x%x -> 0x%08lX res=%p", (int)rd->Dimension, (unsigned long long)rd->Width, rd->Height,
           (int)rd->Format, (unsigned)rd->Flags, (unsigned)d.Flags, (unsigned)st, (unsigned long)hr, out ? *out : nullptr);
    return hr;
}


// ---------- трансляция CPU-handle'ов descriptor heap ----------
// Xbox-дескриптор heap = PC D3D12_DESCRIPTOR_HEAP_DESC (16 байт) + {void* ExpectedCpuBase; size_t Size} [по стеку игры, функция 0x141cf3280].
// Игра создаёт heap в своей памяти и требует GetCPUDescriptorHandleForHeapStart()==ExpectedCpuBase; VKD3D-handle'ы непрозрачны -> диапазоны {guest,size,real}.
struct HRange { uint64_t guest, size, real, gpu; };
static uint64_t GpuHandleToGuest(uint64_t h);
static HRange g_hr[512]; static std::atomic<int> g_hrN{0};
#include "guest_shadow.h"
static inline uint64_t XlatCpu(uint64_t h)
{
    int n = g_hrN.load(std::memory_order_acquire);
    for (int i = 0; i < n; i++) { const HRange& r = g_hr[i]; if (h - r.guest < r.size) return r.real + (h - r.guest); }
    return h;
}
static uint64_t GpuHandleToGuest(uint64_t h)
{
    int n = g_hrN.load(std::memory_order_acquire);
    for (int i = 0; i < n; i++) { const HRange& r = g_hr[i]; if (r.gpu && h - r.gpu < r.size) return r.guest + (h - r.gpu); }
    return 0;
}
#include "gfx_view_trace.h"
static void __stdcall Dev_CreateCBV(Wrap* w, const D3D12_CONSTANT_BUFFER_VIEW_DESC* d, D3D12_CPU_DESCRIPTOR_HANDLE h) { h.ptr = XlatCpu(h.ptr); ((ID3D12Device*)w->real)->CreateConstantBufferView(d, h); }
static void __stdcall Dev_CreateSRV(Wrap* w, ID3D12Resource* r, const D3D12_SHADER_RESOURCE_VIEW_DESC* d, D3D12_CPU_DESCRIPTOR_HANDLE h)
{ const uint64_t guest=h.ptr;h.ptr = XlatCpu(h.ptr); D3D12_SHADER_RESOURCE_VIEW_DESC c; if (d) { c = *d; LogXboxFormat("SRV", (int)d->Format); c.Format = XboxFormatToPc(d->Format); d = &c; } ((ID3D12Device*)w->real)->CreateShaderResourceView((ID3D12Resource*)Unwrap(r), d, h);
  if (GfxNativeEnabled() && d && d->ViewDimension >= D3D12_SRV_DIMENSION_TEXTURE1D) GfxNativeRememberSrv(r, d, guest, h.ptr);
  GfxViewTrace("srv-create",w,__builtin_return_address(0),guest,h.ptr,r,Unwrap(r),d,d?sizeof *d:0); }
static void __stdcall Dev_CreateUAV(Wrap* w, ID3D12Resource* r, ID3D12Resource* c, const D3D12_UNORDERED_ACCESS_VIEW_DESC* d, D3D12_CPU_DESCRIPTOR_HANDLE h)
{ h.ptr = XlatCpu(h.ptr); D3D12_UNORDERED_ACCESS_VIEW_DESC x; if (d) { x = *d; LogXboxFormat("UAV", (int)d->Format); x.Format = XboxFormatToPc(d->Format); d = &x; } ((ID3D12Device*)w->real)->CreateUnorderedAccessView((ID3D12Resource*)Unwrap(r), (ID3D12Resource*)Unwrap(c), d, h); }
static void __stdcall Dev_CreateRTV(Wrap* w, ID3D12Resource* r, const D3D12_RENDER_TARGET_VIEW_DESC* d, D3D12_CPU_DESCRIPTOR_HANDLE h)
{ const uint64_t guest=h.ptr;h.ptr = XlatCpu(h.ptr); D3D12_RENDER_TARGET_VIEW_DESC x; if (d) { x = *d; LogXboxFormat("RTV", (int)d->Format); x.Format = XboxFormatToPc(d->Format); d = &x; } ((ID3D12Device*)w->real)->CreateRenderTargetView((ID3D12Resource*)Unwrap(r), d, h); if (GfxNativeEnabled()) GfxNativeRememberRtv(r,d,guest,h.ptr);
  GfxViewTrace("rtv-create",w,__builtin_return_address(0),guest,h.ptr,r,Unwrap(r),d,d?sizeof *d:0); }
static void __stdcall Dev_CreateDSV(Wrap* w, ID3D12Resource* r, const D3D12_DEPTH_STENCIL_VIEW_DESC* d, D3D12_CPU_DESCRIPTOR_HANDLE h)
{ h.ptr = XlatCpu(h.ptr); D3D12_DEPTH_STENCIL_VIEW_DESC x; if (d) { x = *d; LogXboxFormat("DSV", (int)d->Format); x.Format = XboxFormatToPc(d->Format); d = &x; } ((ID3D12Device*)w->real)->CreateDepthStencilView((ID3D12Resource*)Unwrap(r), d, h); }
static void __stdcall Dev_CreateSampler(Wrap* w, const D3D12_SAMPLER_DESC* d, D3D12_CPU_DESCRIPTOR_HANDLE h) { h.ptr = XlatCpu(h.ptr); ((ID3D12Device*)w->real)->CreateSampler(d, h); }
static void __stdcall Dev_CopyDescriptors(Wrap* w, UINT nd, const D3D12_CPU_DESCRIPTOR_HANDLE* ds, const UINT* dsz, UINT ns, const D3D12_CPU_DESCRIPTOR_HANDLE* ss, const UINT* ssz, D3D12_DESCRIPTOR_HEAP_TYPE t)
{
    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> d(nd), s(ns); for (UINT i = 0; i < nd; i++) d[i].ptr = XlatCpu(ds[i].ptr); for (UINT i = 0; i < ns; i++) s[i].ptr = XlatCpu(ss[i].ptr);
    ((ID3D12Device*)w->real)->CopyDescriptors(nd, d.data(), dsz, ns, s.data(), ssz, t);
    if (nd && ns) {
        GfxViewTrace("copy-ranges-dst",w,__builtin_return_address(0),ds[0].ptr,d[0].ptr,nullptr,nullptr,nullptr,0,t,nd);
        GfxViewTrace("copy-ranges-src",w,__builtin_return_address(0),ss[0].ptr,s[0].ptr,nullptr,nullptr,nullptr,0,t,ns);
    }
}
static void __stdcall Dev_CopyDescriptorsSimple(Wrap* w, UINT n, D3D12_CPU_DESCRIPTOR_HANDLE d, D3D12_CPU_DESCRIPTOR_HANDLE s, D3D12_DESCRIPTOR_HEAP_TYPE t)
{ const uint64_t guestDst=d.ptr,guestSrc=s.ptr;d.ptr = XlatCpu(d.ptr); s.ptr = XlatCpu(s.ptr); ((ID3D12Device*)w->real)->CopyDescriptorsSimple(n, d, s, t);
  GfxViewTrace("copy-simple-dst",w,__builtin_return_address(0),guestDst,d.ptr,nullptr,nullptr,nullptr,0,t,n);
  GfxViewTrace("copy-simple-src",w,__builtin_return_address(0),guestSrc,s.ptr,nullptr,nullptr,nullptr,0,t,n); }
static void __stdcall List_OMSetRenderTargets(Wrap* w, UINT n, const D3D12_CPU_DESCRIPTOR_HANDLE* rtv, BOOL single, const D3D12_CPU_DESCRIPTOR_HANDLE* dsv)
{
    D3D12_CPU_DESCRIPTOR_HANDLE r[8] = {}, dd = {}; UINT m = n < 8 ? n : 8; for (UINT i = 0; i < m && rtv; i++) r[i].ptr = XlatCpu(rtv[i].ptr); if (dsv) dd.ptr = XlatCpu(dsv->ptr);
    ((ID3D12GraphicsCommandList*)w->real)->OMSetRenderTargets(n, rtv ? r : nullptr, single, dsv ? &dd : nullptr);
}
static bool EnvOn(const char* n) { char v[4]; return GetEnvironmentVariableA(n, v, sizeof v) > 0; }
#include <mutex>
#include <algorithm>
#include "profiler.h"
static bool DrawTraceOn() { static bool on = EnvOn("XSX_DRAW_TRACE"); return on; }
static bool GfxNativeMinimal() { static bool m = EnvOn("XSX_DEV_NATIVE_MINIMAL"); return m; }
static bool GfxRecordOn() { return DrawTraceOn() || (GfxNativeEnabled() && !GfxNativeMinimal()); }
struct XListState {
    uintptr_t pso = 0;
    std::map<std::pair<int, uintptr_t>, std::array<uintptr_t, 4>> slots;
    std::map<UINT,D3D12_VERTEX_BUFFER_VIEW> vertexViews;
    D3D12_INDEX_BUFFER_VIEW indexView{};
    bool vertexValid=false,indexValid=false,indexPresent=false;
};   // (слот, корневой индекс) -> аргументы
static std::map<Wrap*, XListState> g_xstate; static SRWLOCK g_xstateLock = SRWLOCK_INIT;
static void XStateRecord(Wrap* w, int slot, uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d)
{
    ExclusiveSrwLock g(g_xstateLock); auto& st = g_xstate[w];
    bool keyed = slot >= 35 && slot <= 46;   // таблицы/константы/корневые виды: ключ — корневой индекс
    st.slots[{slot, keyed ? a : 0}] = { a, b, c, d };
    // Setter arrays are frequently on the game's stack. Copy them now.
    if (slot==48) {
        D3D12_VERTEX_BUFFER_VIEW views[32]{};SIZE_T got=0;
        st.vertexValid=a<32 && b<=32-a && (!b || (c &&
            ReadProcessMemory(GetCurrentProcess(),(void*)c,views,b*sizeof views[0],&got) && got==b*sizeof views[0]));
        if (st.vertexValid) for (UINT i=0;i<b;++i) st.vertexViews[(UINT)a+i]=views[i];
    }
    if (slot==47) {
        SIZE_T got=0;st.indexPresent=a!=0;
        st.indexValid=!a || (ReadProcessMemory(GetCurrentProcess(),(void*)a,&st.indexView,sizeof st.indexView,&got) && got==sizeof st.indexView);
    }
}
static void DrawTrace(Wrap* w, const char* what, uint32_t x, uint32_t y);
static void __stdcall ListSlot47(Wrap* w, D3D12_CPU_DESCRIPTOR_HANDLE h, D3D12_CLEAR_FLAGS f, FLOAT d, UINT8 st, UINT nr, const D3D12_RECT* rc)
{
    if (GfxRecordOn() && IsXboxList(w)) XStateRecord(w, 47, h.ptr, (uintptr_t)f, 0, 0);
    auto* list = (ID3D12GraphicsCommandList*)w->real;
    if (IsXboxList(w)) {
        // Game RVA0x328b8: RDX points to {GPU VA, size, R16/R32_UINT},
        // or is null to unbind. This is IASetIndexBuffer, not a DSV handle.
        static std::atomic<unsigned> calls{0};
        if (calls++ < 12) { CALLER(c); XS_LOG("IA ABI Xbox list47 index_view=%p caller=%s", (void*)h.ptr, c); }
        if (GfxNativeEnabled()) return; // Guest VA is resolved from the copied view at Draw.
        list->IASetIndexBuffer((const D3D12_INDEX_BUFFER_VIEW*)h.ptr);
        return;
    }
    h.ptr = XlatCpu(h.ptr);
    list->ClearDepthStencilView(h, f, d, st, nr, rc);
}
static void __stdcall ListSlot48(Wrap* w, uintptr_t argument, uintptr_t third, uintptr_t fourth, const D3D12_RECT* rc)
{
    if (GfxRecordOn() && IsXboxList(w)) XStateRecord(w, 48, argument, third, fourth, 0);
    auto* list = (ID3D12GraphicsCommandList*)w->real;
    if (IsXboxList(w)) {
        // Game RVA0x32960: (startSlot=0, viewCount, views). Each 16-byte
        // view is built as {GPU VA, size, stride}; PC IASetVertexBuffers.
        static std::atomic<unsigned> calls{0};
        if (calls++ < 12) { CALLER(c); XS_LOG("IA ABI Xbox list48 start=%u count=%u views=%p caller=%s", (UINT)argument, (UINT)third, (void*)fourth, c); }
        if (GfxNativeEnabled()) return;
        list->IASetVertexBuffers((UINT)argument, (UINT)third, (const D3D12_VERTEX_BUFFER_VIEW*)fourth);
        return;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE h{XlatCpu(argument)};
    list->ClearRenderTargetView(h, (const FLOAT*)third, (UINT)fourth, rc);
}
static void __stdcall List_ClearUAVUint(Wrap* w, D3D12_GPU_DESCRIPTOR_HANDLE g, D3D12_CPU_DESCRIPTOR_HANDLE h, ID3D12Resource* r, const UINT* v, UINT nr, const D3D12_RECT* rc) { h.ptr = XlatCpu(h.ptr); ((ID3D12GraphicsCommandList*)w->real)->ClearUnorderedAccessViewUint(g, h, (ID3D12Resource*)Unwrap(r), v, nr, rc); }
static void __stdcall List_ClearUAVFloat(Wrap* w, D3D12_GPU_DESCRIPTOR_HANDLE g, D3D12_CPU_DESCRIPTOR_HANDLE h, ID3D12Resource* r, const FLOAT* v, UINT nr, const D3D12_RECT* rc) { h.ptr = XlatCpu(h.ptr); ((ID3D12GraphicsCommandList*)w->real)->ClearUnorderedAccessViewFloat(g, h, (ID3D12Resource*)Unwrap(r), v, nr, rc); }
// ---------- обёртки остальных объектов (queue=2, list=3, heap=4) ----------
static Wrap* g_devWrap = nullptr;
static const char* kKindName[] = { "?", "device", "queue", "list", "heap", "pso", "allocator", "fence", "resource" };
static void* g_vtQueue[kObjSlots], *g_vtList[kObjSlots], *g_vtHeap[kObjSlots], *g_vtPso[kObjSlots], *g_vtAlloc[kObjSlots], *g_vtFence[kObjSlots];
static void* g_originalRootSlots[kObjSlots];
static bool RootTraceOn() { static bool on = EnvOn("XSX_ROOT_TRACE"); return on; }

static std::atomic<int> g_rootTraceN[kObjSlots];
template<int N> static void RootTrace(Wrap* w, uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d)
{
    if (GfxRecordOn() && IsXboxList(w)) XStateRecord(w, N, a, b, c, d);
    if (!RootTraceOn() || !IsXboxList(w)) return;
    int n = ++g_rootTraceN[N]; if (n > 10) return;
    XS_LOG("ROOTTRACE list=%p slot %d (+0x%x) #%d a=%p b=%p c=%p d=%p", (void*)w, N, N * 8, n, (void*)a, (void*)b, (void*)c, (void*)d);
}
template<int N> static void __stdcall TraceFwdSlot(Wrap* w, uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d)
{
    RootTrace<N>(w, a, b, c, d);
    ((void (__stdcall*)(Wrap*, uintptr_t, uintptr_t, uintptr_t, uintptr_t))g_originalRootSlots[N])(w, a, b, c, d);
}
template<int N> static void __stdcall BootstrapRootSlot(Wrap* w, uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d)
{
    RootTrace<N>(w, a, b, c, d);
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
static std::atomic<int> g_objCalls[9][kObjSlots];
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
    if (kind == 8) { static bool off = [] { char v[4]; return GetEnvironmentVariableA("XSX_NO_RESWRAP", v, sizeof v) > 0; }(); if (off) return hr; }
    if (!out || !*out) return E_FAIL;
    void* real = *out;
    *out = NewWrap(vt, real, kind);
    if (*out) { if (kind == 7) ((Wrap*)*out)->pad[0] = 1; return hr; }
    ((IUnknown*)real)->Release(); // Do not leak the backend's owned reference.
    return E_OUTOFMEMORY;
}

// ---------- Обёртка ID3D12Resource (kind 8) ----------
// Xbox UMA: GetGPUVirtualAddress (vtable +0x58) у ресурса == CPU-адрес его памяти; игра пишет в неё процессором (memset, memcpy).
// VKD3D возвращает VA 0 для текстур (и VA, не являющийся CPU-адресом, для буферов). Как у VIctory (victory-gpu resource.rs `gpu_address`):
// у текстуры есть CPU-«тень» (память игры), её адрес и отдаётся как GPU VA. Синхронизации тени с PC-текстурой НЕТ (раскладка Xbox — тайлинг,
// нужен detile): пока тень лишь предотвращает падения; буферы отдают настоящий VA VKD3D.
using ResourceFreeHook = void (__stdcall*)(void*, uint64_t);
struct ResExtra {
    void* shadow; size_t size; unsigned shadowMaps = 0;
    // Only eager committed127 allocations carry ownership. Placed addresses
    // and legacy lazy shadows retain their existing ownership policy.
    void* ownedBase = nullptr;
    ResourceFreeHook freeHook = nullptr;
    uint64_t attributes = 0;
    D3D12_RESOURCE_STATES presentState = D3D12_RESOURCE_STATE_COMMON;
    bool presentStateKnown = false;
};
static std::map<void*, ResExtra> g_resShadow;   // wrapper -> тень (под g_resLock)
static SRWLOCK g_resLock = SRWLOCK_INIT;
static bool PresentSourceState(void* exposed, D3D12_RESOURCE_STATES& state)
{
    SharedSrwLock held(g_resLock);
    auto it = g_resShadow.find(exposed);
    if (it == g_resShadow.end() || !it->second.presentStateKnown) return false;
    state = it->second.presentState;
    return true;
}
static void InvalidatePresentState(void* exposed)
{
    ExclusiveSrwLock held(g_resLock);
    // A null aliasing endpoint means any resource, not an empty set.
    if (!exposed) {
        for (auto& entry : g_resShadow) entry.second.presentStateKnown = false;
        return;
    }
    auto it = g_resShadow.find(exposed);
    if (it != g_resShadow.end()) it->second.presentStateKnown = false;
}
static void ForgetResourceAddress(Wrap* resource, void* address);
static void FreeOwnedShadow(const ResExtra& extra)
{
    if (!extra.ownedBase) return;
    if (extra.freeHook) extra.freeHook(extra.shadow, extra.attributes);
    else VirtualFree(extra.ownedBase, 0, MEM_RELEASE);
}
static ULONG __stdcall Res_Release(Wrap* w)
{
    const ULONG refs = ((IUnknown*)w->real)->Release();
    if (!refs) {
        ResExtra extra{};
        {
            ExclusiveSrwLock held(g_resLock);
            auto it = g_resShadow.find(w);
            if (it != g_resShadow.end()) { extra = it->second; g_resShadow.erase(it); }
        }
        // Never call a title allocator while holding our registry lock.
        if (extra.ownedBase) {
            ForgetResourceAddress(w, extra.shadow);
            FreeOwnedShadow(extra);
        }
    }
    return refs;
}
static D3D12_GPU_VIRTUAL_ADDRESS __stdcall Res_GetGPUVirtualAddress(Wrap* w)
{
    ID3D12Resource* r = (ID3D12Resource*)w->real; D3D12_RESOURCE_DESC d = r->GetDesc();
    if (d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) return r->GetGPUVirtualAddress();
    ExclusiveSrwLock held(g_resLock);
    auto it = g_resShadow.find(w);
    if (it == g_resShadow.end()) {
        ID3D12Device* dev = nullptr; r->GetDevice(__uuidof(ID3D12Device), (void**)&dev); size_t sz = 0x10000;
        if (dev) { D3D12_RESOURCE_ALLOCATION_INFO ai = dev->GetResourceAllocationInfo(0, 1, &d); if (ai.SizeInBytes != ~0ull) sz = (size_t)ai.SizeInBytes; dev->Release(); }
        if (sz > (1ull << 30)) sz = (size_t)1 << 30;
        sz = (sz + 0xffff) & ~(size_t)0xffff;
        // Память ресурса берём из аллокатора ИГРЫ (хук __XMemAllocationHooks__, как VIctory `Memory::for_resource`): игра при освобождении ищет
        // адрес в своём дереве аллокаций — чужой VirtualAlloc-адрес даёт NULL в дереве и AV (exe+0x515acf5).
        // attrs = GRAPHICS_ALLOCATOR(128<<16) | pages<<14 | log2(align)<<24 | memtype<<29 (по VIctory resource.rs; поля pages/memtype — предположение).
        void* m = nullptr; { typedef void* (*AllocHook)(size_t, uint64_t); auto hooks = (void**)GetProcAddress(GetModuleHandleW(nullptr), "__XMemAllocationHooks__");
            if (hooks && hooks[0]) { uint64_t pages = sz >= (2u << 20) ? 2 : 1; uint64_t lg = sz >= (2u << 20) ? 21 : 16; uint64_t attrs = (128ull << 16) | (pages << 14) | (lg << 24) | (1ull << 29);
                m = ((AllocHook)hooks[0])(sz, attrs); } }
        bool viaTitle = m != nullptr; if (!m) m = VirtualAlloc(nullptr, sz, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        XS_LOG("RESOURCE %p via=%s (%llux%u fmt=%d) GetGPUVirtualAddress -> CPU-тень %p size=%#zx (UMA-эмуляция; синхронизации с PC-текстурой нет)", (void*)w, viaTitle ? "аллокатор игры" : "VirtualAlloc", (unsigned long long)d.Width, d.Height, (int)d.Format, m, sz);
        it = g_resShadow.emplace(w, ResExtra{m, sz}).first;
    }
    return (D3D12_GPU_VIRTUAL_ADDRESS)(uintptr_t)it->second.shadow;
}
static HRESULT __stdcall Res_Map(Wrap* w, UINT subresource, const D3D12_RANGE* readRange, void** data)
{
    auto* real = (ID3D12Resource*)w->real;
    const HRESULT native = real->Map(subresource, readRange, data);
    const auto dim = real->GetDesc().Dimension;
    // Buffers too: Xbox CreatePlacedResourceX buffers live at title memory and Map returns that address.
    if (native != E_INVALIDARG || subresource != 0 ||
        (dim != D3D12_RESOURCE_DIMENSION_TEXTURE2D && dim != D3D12_RESOURCE_DIMENSION_BUFFER)) return native;
    // Game+0x5121891 maps subresource0, then adds its own footprint offset.
    // DEFAULT PC textures are unmappable; Xbox UMA uses the title memory
    // already exposed by GetGPUVirtualAddress. Do not invent mip offsets or
    // mask device removal/OOM. This is CPU shadow access, not GPU coherence.
    if (data) *data = nullptr;
    // Textures: GetGPUVirtualAddress creates/returns the CPU shadow. Buffers: the GPU VA is VKD3D's,
    // so only resources placed at title memory (slot133, shadow = address) can be mapped.
    if (dim == D3D12_RESOURCE_DIMENSION_TEXTURE2D && !Res_GetGPUVirtualAddress(w)) return E_OUTOFMEMORY;
    uintptr_t address = 0;
    {
        ExclusiveSrwLock held(g_resLock);
        auto it = g_resShadow.find(w);
        if (it == g_resShadow.end() || !it->second.shadow) return native;
        address = (uintptr_t)it->second.shadow;
        if (readRange && (readRange->Begin > readRange->End ||
            (it->second.size && readRange->End > it->second.size))) return E_INVALIDARG;
        ++it->second.shadowMaps;
    }
    if (data) *data = (void*)address;
    static std::atomic<unsigned> calls{0};
    const unsigned n = ++calls;
    if (n <= 12 || !(n % 1000)) XS_LOG("RESOURCE Map subresource0 native=0x%08lX -> CPU-shadow S_OK resource=%p data=%p (no GPU upload/detile)", (unsigned long)native, (void*)w, (void*)address);
    return S_OK;
}
static void __stdcall Res_Unmap(Wrap* w, UINT subresource, const D3D12_RANGE* writtenRange)
{
    if (subresource == 0) {
        ExclusiveSrwLock held(g_resLock);
        auto it = g_resShadow.find(w);
        if (it != g_resShadow.end() && it->second.shadowMaps) {
            --it->second.shadowMaps;
            return; // No native Map succeeded for this shadow mapping.
        }
    }
    ((ID3D12Resource*)w->real)->Unmap(subresource, writtenRange);
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
#include "list_writes.h"
#include "gfx_snapshot.h"
static void __stdcall Queue_ExecuteCommandLists(Wrap* w, UINT n, ID3D12CommandList* const* lists)
{
    PROF_SCOPE(PROF_SUBMIT);
    const HRESULT hr = GfxNativeEnabled() ? GfxNativeSubmit(w, n, lists) : SubmitListsWithWrites(w, n, lists);
    if (FAILED(hr)) XS_LOG("ExecuteCommandLists deferred write failure hr=0x%08lX", (unsigned long)hr);
    if (BootstrapNoopEnabled()) ++g_bootstrap.execute;
}
// ---- PresentX -> DXGI swapchain на окне игры ----
// Plane (по VIctory queue.rs): { u64 token; u32 count; This* resources } — показывается первый ресурс первой плоскости.
// Окно игры находим сами (EnumWindows по PID), swapchain FLIP_DISCARD x2 на ПЕРВОЙ DIRECT-очереди, в которую пришёл PresentX.
// Native color presentation: copy equal surfaces, otherwise sample with a
// private PC shader. This does not translate title shaders or upload CPU shadows.
struct PresentState {
    bool tried = false, ok = false, poisoned = false, blitTried = false;
    HWND hwnd = nullptr; HMODULE dxgi = nullptr;
    IDXGISwapChain3* sc = nullptr; ID3D12CommandQueue* q = nullptr; ID3D12Device* dev = nullptr;
    ID3D12DescriptorHeap* rtvHeap = nullptr; UINT rtvInc = 0;
    ID3D12CommandAllocator* alloc[2] = {}; ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr; HANDLE ev = nullptr; UINT64 fv = 0;
    UINT w = 0, h = 0, frame = 0; HRESULT lastHr = S_OK, blitHr = S_OK;
    // Process-lifetime objects, like the existing swapchain. Per-frame source
    // references are explicitly released after our fence, never from DllMain.
    xsx::presentation::SurfaceBlit* blit = nullptr;
    IUnknown* pendingGuest = nullptr;
};
static PresentState g_pr; static SRWLOCK g_prLock = SRWLOCK_INIT;
static void ReleasePresentSource()
{
    // Drop the native reference first. The wrapper's final Release must own
    // CPU-shadow cleanup; dropping the last reference behind it would leak it.
    if (g_pr.blit) g_pr.blit->ReleaseSource();
    IUnknown* previous = g_pr.pendingGuest; g_pr.pendingGuest = nullptr;
    if (previous) previous->Release();
}
static BOOL CALLBACK FindGameWindow(HWND h, LPARAM lp)
{
    DWORD pid = 0; GetWindowThreadProcessId(h, &pid); if (pid != GetCurrentProcessId() || !IsWindowVisible(h)) return TRUE;
    RECT r; GetClientRect(h, &r); long area = (r.right - r.left) * (r.bottom - r.top); HWND* best = (HWND*)lp;
    RECT rb = {}; if (*best) GetClientRect(*best, &rb); if (area > (rb.right - rb.left) * (rb.bottom - rb.top)) *best = h; return TRUE;
}
static HRESULT InitPresent(Wrap* qw)
{
    HWND hw = nullptr; EnumWindows(FindGameWindow, (LPARAM)&hw); if (!hw) return S_FALSE;
    {   // XSX_WINDOW: fullscreen (default; borderless window covering the primary monitor, which Wine
        // presents as fullscreen) | window (decorated 1920x1080 client, centred) | keep (title's own window).
        char mode[16] = "fullscreen"; GetEnvironmentVariableA("XSX_WINDOW", mode, sizeof mode);
        const int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
        if (!_stricmp(mode, "window")) {
            const DWORD style = WS_OVERLAPPEDWINDOW | WS_VISIBLE; RECT r{0, 0, 1920, 1080};
            if (r.right > sw) { r.right = sw * 9 / 10; r.bottom = r.right * 9 / 16; }
            AdjustWindowRect(&r, style, FALSE);
            SetWindowLongPtrW(hw, GWL_STYLE, style);
            SetWindowPos(hw, HWND_TOP, (sw - (r.right - r.left)) / 2, (sh - (r.bottom - r.top)) / 2, r.right - r.left, r.bottom - r.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        } else if (_stricmp(mode, "keep")) {
            SetWindowLongPtrW(hw, GWL_STYLE, WS_POPUP | WS_VISIBLE);
            SetWindowLongPtrW(hw, GWL_EXSTYLE, GetWindowLongPtrW(hw, GWL_EXSTYLE) & ~(WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_DLGMODALFRAME));
            SetWindowPos(hw, HWND_TOP, 0, 0, sw, sh, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        }
        XS_LOG("PRESENT window mode=%s monitor=%dx%d", mode, sw, sh);
    }
    RECT cr{}; if (!GetClientRect(hw, &cr)) return HRESULT_FROM_WIN32(GetLastError());
    const UINT width = cr.right - cr.left, height = cr.bottom - cr.top;
    if (!width || !height) return S_FALSE;
    auto* queue = (ID3D12CommandQueue*)qw->real;
    if (queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) return S_FALSE;
    g_pr.tried = true; g_pr.hwnd = hw; g_pr.w = width; g_pr.h = height;
    HRESULT hr = [&]() -> HRESULT {
        HRESULT result = queue->GetDevice(__uuidof(ID3D12Device), (void**)&g_pr.dev);
        if (FAILED(result)) return result;
        g_pr.q = queue; queue->AddRef();
        g_pr.dxgi = LoadLibraryW(L"dxgi.dll");
        using Factory = HRESULT (WINAPI*)(UINT, REFIID, void**);
        auto create = xsx::presentation::Symbol<Factory>(g_pr.dxgi, "CreateDXGIFactory2");
        if (!create) return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
        xsx::presentation::Ref<IDXGIFactory2> factory;
        result = create(0, __uuidof(IDXGIFactory2), (void**)factory.Put());
        if (FAILED(result)) return result;
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = width; desc.Height = height; desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1; desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2; desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        xsx::presentation::Ref<IDXGISwapChain1> initial;
        result = factory->CreateSwapChainForHwnd(queue, hw, &desc, nullptr, nullptr, initial.Put());
        if (FAILED(result)) return result;
        result = initial->QueryInterface(__uuidof(IDXGISwapChain3), (void**)&g_pr.sc);
        if (FAILED(result)) return result;
        D3D12_DESCRIPTOR_HEAP_DESC heap{}; heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; heap.NumDescriptors = 2;
        result = g_pr.dev->CreateDescriptorHeap(&heap, __uuidof(ID3D12DescriptorHeap), (void**)&g_pr.rtvHeap);
        if (FAILED(result)) return result;
        g_pr.rtvInc = g_pr.dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        for (auto& allocator : g_pr.alloc) {
            result = g_pr.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                __uuidof(ID3D12CommandAllocator), (void**)&allocator);
            if (FAILED(result)) return result;
        }
        result = g_pr.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_pr.alloc[0], nullptr,
            __uuidof(ID3D12GraphicsCommandList), (void**)&g_pr.list);
        if (FAILED(result)) return result;
        result = g_pr.list->Close(); if (FAILED(result)) return result;
        result = g_pr.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void**)&g_pr.fence);
        if (FAILED(result)) return result;
        g_pr.ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!g_pr.ev) return HRESULT_FROM_WIN32(GetLastError());
        g_pr.blit = new (std::nothrow) xsx::presentation::SurfaceBlit;
        if (!g_pr.blit) return E_OUTOFMEMORY;
        // Compile lazily only when scaling is needed; the copy/pattern path
        // remains usable if a compiler is absent.
        return S_OK;
    }();
    g_pr.lastHr = hr;
    if (FAILED(hr)) {
        delete g_pr.blit; g_pr.blit = nullptr;
        if (g_pr.ev) { CloseHandle(g_pr.ev); g_pr.ev = nullptr; }
        auto release = [](auto*& value) { if (value) { value->Release(); value = nullptr; } };
        release(g_pr.fence); release(g_pr.list);
        for (auto& allocator : g_pr.alloc) release(allocator);
        release(g_pr.rtvHeap); release(g_pr.sc); release(g_pr.q); release(g_pr.dev);
        if (g_pr.dxgi) { FreeLibrary(g_pr.dxgi); g_pr.dxgi = nullptr; }
        XS_LOG("PRESENT initialization failed hr=0x%08lX", (unsigned long)hr);
        return hr;
    }
    g_pr.ok = true;
    XS_LOG("PRESENT swapchain=%ux%u window=%p format=28 buffers=2", width, height, (void*)hw);
    return S_OK;
}
static HRESULT PresentFrame(Wrap* qw, IUnknown* exposedSource)
{
    PROF_SCOPE(PROF_PRESENT);
    using xsx::presentation::Transition;
    ExclusiveSrwLock held(g_prLock);
    if (!g_pr.ok && !g_pr.tried) {
        const HRESULT hr = InitPresent(qw);
        if (hr != S_OK) return FAILED(hr) ? hr : S_OK;
    }
    if (!g_pr.ok || g_pr.poisoned) return g_pr.lastHr;
    // Cross-queue production requires an explicit dependency, not an assumed
    // completion. The observed title presents on this same DIRECT queue.
    if (qw->real != g_pr.q) return E_INVALIDARG;
    auto fail = [&](const char* stage, HRESULT hr) {
        g_pr.lastHr = hr; g_pr.poisoned = true;
        XS_LOG("PRESENT failure stage=%s frame=%u hr=0x%08lX", stage, g_pr.frame, (unsigned long)hr);
        return hr;
    };
    const UINT64 completed = g_pr.fence->GetCompletedValue();
    if (completed == UINT64_MAX) return fail("device-removed", DXGI_ERROR_DEVICE_REMOVED);
    if (completed < g_pr.fv) {
        PROF_SCOPE(PROF_PRESENT_WAIT);
        const HRESULT hr = g_pr.fence->SetEventOnCompletion(g_pr.fv, g_pr.ev);
        if (FAILED(hr)) return fail("fence-event", hr);
        const DWORD waited = WaitForSingleObject(g_pr.ev, 5000);
        if (waited != WAIT_OBJECT_0) return fail("fence-wait",
            HRESULT_FROM_WIN32(waited == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError()));
        const UINT64 finished = g_pr.fence->GetCompletedValue();
        if (finished == UINT64_MAX) return fail("device-removed", DXGI_ERROR_DEVICE_REMOVED);
        if (finished < g_pr.fv) return fail("fence-incomplete", E_UNEXPECTED);
    }
    ReleasePresentSource();
    const UINT index = g_pr.sc->GetCurrentBackBufferIndex();
    xsx::presentation::Ref<ID3D12Resource> target;
    HRESULT hr = g_pr.sc->GetBuffer(index, __uuidof(ID3D12Resource), (void**)target.Put());
    if (FAILED(hr)) return fail("backbuffer", hr);
    auto handle = g_pr.rtvHeap->GetCPUDescriptorHandleForHeapStart(); handle.ptr += (SIZE_T)index * g_pr.rtvInc;
    g_pr.dev->CreateRenderTargetView(target.Get(), nullptr, handle);
    hr = g_pr.alloc[index]->Reset(); if (FAILED(hr)) return fail("allocator-reset", hr);
    hr = g_pr.list->Reset(g_pr.alloc[index], nullptr); if (FAILED(hr)) return fail("list-reset", hr);
    auto* source = (ID3D12Resource*)Unwrap(exposedSource);
    if (GfxNativeEnabled()) GfxNativeOnPresent(source, g_pr.frame);
    D3D12_RESOURCE_DESC sourceDesc{};
    if (source) sourceDesc = source->GetDesc();
    D3D12_RESOURCE_STATES sourceState = D3D12_RESOURCE_STATE_COMMON;
    const bool stateKnown = source && PresentSourceState(exposedSource, sourceState);
    static const bool pattern = [] { char value[4]; return GetEnvironmentVariableA("XSX_PRESENT_PATTERN", value, sizeof value) > 0; }();
    enum Mode { Pattern, Copy, Scale } mode = Pattern;
    const char* reason = pattern ? "forced" : !source ? "missing-source" : !stateKnown ? "unknown-source-state" : "unsupported-source";
    const auto targetDesc = target->GetDesc();
    if (!pattern && stateKnown && xsx::presentation::SurfaceBlit::Supports(sourceDesc)) {
        if (sourceDesc.Width == targetDesc.Width && sourceDesc.Height == targetDesc.Height && sourceDesc.Format == targetDesc.Format) mode = Copy;
        else {
            if (!g_pr.blitTried) {
                g_pr.blitTried = true; g_pr.blitHr = g_pr.blit->Initialize(g_pr.dev);
                if (FAILED(g_pr.blitHr)) XS_LOG("PRESENT blit unavailable hr=0x%08lX", (unsigned long)g_pr.blitHr);
            }
            if (SUCCEEDED(g_pr.blitHr)) mode = Scale;
            else reason = "blit-initialize-failed";
        }
    }
    if (mode != Pattern) {
        exposedSource->AddRef(); g_pr.pendingGuest = exposedSource;
    }
    const auto targetState = mode == Copy ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_RENDER_TARGET;
    Transition(g_pr.list, target.Get(), D3D12_RESOURCE_STATE_PRESENT, targetState);
    if (mode == Copy) {
        Transition(g_pr.list, source, sourceState, D3D12_RESOURCE_STATE_COPY_SOURCE);
        g_pr.list->CopyResource(target.Get(), source);
        Transition(g_pr.list, source, D3D12_RESOURCE_STATE_COPY_SOURCE, sourceState);
    } else if (mode == Scale) {
        hr = g_pr.blit->Record(g_pr.list, source, sourceState, (UINT)targetDesc.Width, targetDesc.Height, handle);
        if (FAILED(hr)) { ReleasePresentSource(); return fail("blit-record", hr); }
    } else {
        const float phase = (float)(g_pr.frame % 240) / 240.f;
        const float color[4]{0.15f + 0.6f * phase, 0.1f + 0.3f * (1 - phase), 0.45f, 1.f};
        g_pr.list->ClearRenderTargetView(handle, color, 0, nullptr);
    }
    if ((int)(g_pr.frame % (UINT)std::max(1, GfxSnapEvery())) == 0 && GfxSnapEvery() > 0)
        g_snapPending = GfxSnapRecord(g_pr.list, target.Get(), targetState);
    Transition(g_pr.list, target.Get(), targetState, D3D12_RESOURCE_STATE_PRESENT);
    hr = g_pr.list->Close();
    if (FAILED(hr)) { ReleasePresentSource(); return fail("list-close", hr); }
    ID3D12CommandList* commands[]{g_pr.list}; g_pr.q->ExecuteCommandLists(1, commands);
    const HRESULT presented = g_pr.sc->Present(0, 0);
    hr = g_pr.q->Signal(g_pr.fence, ++g_pr.fv);
    // On failed signal retain both source refs and prohibit reuse: completion
    // is unknown, so recycling the descriptor or allocator would be unsafe.
    if (FAILED(hr)) return fail("fence-signal", hr);
    GfxSnapSave(g_pr.fv, g_pr.fence, g_pr.ev, target.Get());
    g_pr.lastHr = presented;
    if (g_pr.frame < 6 || FAILED(presented)) XS_LOG("PRESENT frame=%u mode=%s source=%llux%u format=%u state=%#x target=%llux%u reason=%s hr=0x%08lX",
        g_pr.frame, mode == Copy ? "copied-surface" : mode == Scale ? "scaled-surface" : "test-pattern",
        (unsigned long long)sourceDesc.Width, sourceDesc.Height, (unsigned)sourceDesc.Format, (unsigned)sourceState,
        (unsigned long long)targetDesc.Width, targetDesc.Height, mode == Pattern ? reason : "none", (unsigned long)presented);
    ++g_pr.frame;
    return presented;
}
static HRESULT __stdcall QueuePresent32(Wrap* w, UINT planes, const void* a, const void* b2)
{
    ProfStart(); ProfFrame();
    const unsigned count = ++g_bootstrap.present;
    IUnknown* exposed = nullptr; ID3D12Resource* src = nullptr;
    if (a && !IsBadReadPtr(a, 24)) { const uint64_t* pl = (const uint64_t*)a; uint32_t cnt = (uint32_t)pl[1]; void* res = (void*)pl[2];
        if (count <= 3) XS_LOG("PresentX plane[0]: token=%#llx count=%u resources=%p (arg2=%p)", (unsigned long long)pl[0], cnt, res, b2);
        if (count <= 3 && res && !IsBadReadPtr(res, 32)) { const uint64_t* q = (const uint64_t*)res; XS_LOG("PresentX resources rva=exe+%#llx [0..3]=%p %p %p %p arena=[%p,%p)", (unsigned long long)((uintptr_t)res - (uintptr_t)GetModuleHandleA(nullptr)), (void*)q[0], (void*)q[1], (void*)q[2], (void*)q[3], (void*)g_arena_lo, (void*)g_arena_hi);
            const uint64_t* pp = (const uint64_t*)a; XS_LOG("PresentX arg a dump: %p %p %p %p %p %p %p %p", (void*)pp[0], (void*)pp[1], (void*)pp[2], (void*)pp[3], (void*)pp[4], (void*)pp[5], (void*)pp[6], (void*)pp[7]);
            for (int k = 2; k <= 4; k++) { void* pk = (void*)pp[k]; if (pk && !IsBadReadPtr(pk, 32)) { const uint64_t* z = (const uint64_t*)pk; char who[160]; CallerStr((void*)z[0], who, sizeof who); XS_LOG("PresentX a[%d] -> %p %p %p %p  (z0=%s)", k, (void*)z[0], (void*)z[1], (void*)z[2], (void*)z[3], who); } }
            if (b2 && !IsBadReadPtr(b2, 64)) { const uint64_t* bp = (const uint64_t*)b2; XS_LOG("PresentX arg b dump: %p %p %p %p %p %p %p %p", (void*)bp[0], (void*)bp[1], (void*)bp[2], (void*)bp[3], (void*)bp[4], (void*)bp[5], (void*)bp[6], (void*)bp[7]); } }
        if (cnt && res && !IsBadReadPtr(res, 8)) { void* r0 = *(void**)res; if (r0) { exposed = (IUnknown*)r0; src = (ID3D12Resource*)Unwrap(r0); } } }
    { static std::atomic<int> nn{0}, nz{0}; if (src) { if (nn++ < 3) XS_LOG("PresentX: НЕНУЛЕВОЙ ресурс кадра %p (present #%u)", (void*)src, count); } else nz++; if (count % 200 == 0) XS_LOG("PresentX: итого без ресурса=%d с ресурсом=%d", nz.load(), nn.load()); }
    const HRESULT hr = PresentFrame(w, exposed);
    if (count <= 8 || !(count % 1000)) {
        CALLER(c);
        XS_LOG("BOOTSTRAP PresentX attempt=%u planes=%u pso=%u set=%u barriers=%u draw=%u dispatch=%u execute=%u native_present=%d noop=%u caller=%s",
            count, planes, g_bootstrap.pso.load(), g_bootstrap.set.load(), g_bootstrap.barriers.load(),
            g_bootstrap.draw.load(), g_bootstrap.dispatch.load(), g_bootstrap.execute.load(), g_pr.ok ? 1 : 0, BootstrapNoopEnabled(), c);
    }
    return hr;
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
    ((ID3D12GraphicsCommandList*)w->real)->ResolveQueryData(heap, type, start, count, (ID3D12Resource*)Unwrap(dst), offset);
}
static void __stdcall List_SetDescriptorHeaps(Wrap* w, UINT n, ID3D12DescriptorHeap* const* heaps)
{
    if (RootTraceOn() && IsXboxList(w)) { static std::atomic<int> k{0}; if (++k <= 10) { auto* h0 = n ? (Wrap*)heaps[0] : nullptr; XS_LOG("ROOTTRACE SetDescriptorHeaps n=%u heaps[0]=%p heaps[1]=%p", n, (void*)h0, n > 1 ? (void*)heaps[1] : nullptr); } }
    ID3D12DescriptorHeap* r[8] = {}; for (UINT i = 0; i < n && i < 8; i++) r[i] = (ID3D12DescriptorHeap*)Unwrap(heaps[i]);
    ((ID3D12GraphicsCommandList*)w->real)->SetDescriptorHeaps(n < 8 ? n : 8, r);
}
static void __stdcall List_SetPipelineState(Wrap* w, ID3D12PipelineState* p)
{
    if (GfxRecordOn() && IsXboxList(w)) { ExclusiveSrwLock g(g_xstateLock); g_xstate[w].pso = (uintptr_t)p; }
    if (GfxNativeEnabled() && IsXboxList(w) && GfxNativeOnSetPso(w, p)) { if (++g_bootstrap.set == 1) XS_LOG("GFXNATIVE SetPipelineState: настоящий PSO привязан"); return; }
    if (BootstrapNoopEnabled() && IsXboxList(w)) { if (BindBootstrapGraphics(w)) ++g_bootstrap.set; return; }
    ((ID3D12GraphicsCommandList*)w->real)->SetPipelineState((ID3D12PipelineState*)Unwrap(p));
}
static void __stdcall ListDraw12(Wrap* w, UINT vertices, UINT instances, UINT start, UINT first_instance)
{
    ProfListOp(w, OP_DRAW);
    if (DrawTraceOn()) DrawTrace(w, "Draw", vertices, instances);
    const GfxDrawResult gfxResult = GfxNativeEnabled() && IsXboxList(w) ? GfxNativeOnDrawBind(w, false, vertices, start, &vertices, &start) : GfxDrawResult::Unrecognized;
    if (gfxResult == GfxDrawResult::Incomplete) return;
    const bool gfxBound = gfxResult == GfxDrawResult::Ready;
    if (BootstrapNoopEnabled() && IsXboxList(w) && !gfxBound) {
        if (!BindBootstrapGraphics(w)) return;
        ++g_bootstrap.draw;
    }
    if (gfxBound) { ++g_bootstrap.draw; static std::atomic<unsigned> k{0}; if (k++ < 16) XS_LOG("GFXNATIVE DRAWARGS Draw vertices=%u instances=%u start=%u first_instance=%u", vertices, instances, start, first_instance); }
    ((ID3D12GraphicsCommandList*)w->real)->DrawInstanced(vertices, instances, start, first_instance);
}
static void __stdcall ListDrawIndexed13(Wrap* w, UINT indices, UINT instances, UINT start, INT base, UINT first_instance)
{
    ProfListOp(w, OP_DRAW);
    if (DrawTraceOn()) DrawTrace(w, "DrawIndexed", indices, instances);
    const GfxDrawResult gfxResult = GfxNativeEnabled() && IsXboxList(w) ? GfxNativeOnDrawBind(w, true, indices, start, &indices, &start) : GfxDrawResult::Unrecognized;
    if (gfxResult == GfxDrawResult::Incomplete) return;
    const bool gfxBound = gfxResult == GfxDrawResult::Ready;
    if (BootstrapNoopEnabled() && IsXboxList(w) && !gfxBound) {
        if (!BindBootstrapGraphics(w)) return;
        ++g_bootstrap.draw;
    }
    if (gfxBound) { ++g_bootstrap.draw; static std::atomic<unsigned> k{0}; if (k++ < 16) XS_LOG("GFXNATIVE DRAWARGS DrawIndexed indices=%u instances=%u start=%u base=%d first_instance=%u", indices, instances, start, base, first_instance); }
    ((ID3D12GraphicsCommandList*)w->real)->DrawIndexedInstanced(indices, instances, start, base, first_instance);
}
static void __stdcall ListDispatch14(Wrap* w, UINT x, UINT y, UINT z)
{
    ProfListOp(w, OP_DISPATCH, 0, (uint64_t)x * y * z);
    if (DrawTraceOn()) DrawTrace(w, "Dispatch", x, y);
    if (BootstrapNoopEnabled() && IsXboxList(w)) {
        if (!BindBootstrapCompute(w)) return;
        ++g_bootstrap.dispatch;
    }
    ((ID3D12GraphicsCommandList*)w->real)->Dispatch(x, y, z);
}

// ---- ресурсы в аргументах списка/очереди: обёртки kind 8 -> настоящие объекты ----
static void __stdcall ListCopyBufferRegion15(Wrap* w, ID3D12Resource* d, UINT64 doff, ID3D12Resource* s, UINT64 soff, UINT64 n)
{
    ProfListOp(w, OP_COPYBUF, n); ((ID3D12GraphicsCommandList*)w->real)->CopyBufferRegion((ID3D12Resource*)Unwrap(d), doff, (ID3D12Resource*)Unwrap(s), soff, n); }
static void __stdcall ListCopyTextureRegion16(Wrap* w, const D3D12_TEXTURE_COPY_LOCATION* d, UINT x, UINT y, UINT z, const D3D12_TEXTURE_COPY_LOCATION* s, const D3D12_BOX* box)
{
    ProfListOp(w, OP_COPYTEX); D3D12_TEXTURE_COPY_LOCATION dd = *d, ss = *s; dd.pResource = (ID3D12Resource*)Unwrap(dd.pResource); ss.pResource = (ID3D12Resource*)Unwrap(ss.pResource);
  ((ID3D12GraphicsCommandList*)w->real)->CopyTextureRegion(&dd, x, y, z, &ss, box); }
static void __stdcall ListCopyResource17(Wrap* w, ID3D12Resource* d, ID3D12Resource* s)
{
    ProfListOp(w, OP_COPYRES); ((ID3D12GraphicsCommandList*)w->real)->CopyResource((ID3D12Resource*)Unwrap(d), (ID3D12Resource*)Unwrap(s)); }
static void __stdcall ListCopyTiles18(Wrap* w, ID3D12Resource* r, const D3D12_TILED_RESOURCE_COORDINATE* c, const D3D12_TILE_REGION_SIZE* sz, ID3D12Resource* b, UINT64 off, D3D12_TILE_COPY_FLAGS f)
{
    ProfListOp(w, OP_COPYTILES); ((ID3D12GraphicsCommandList*)w->real)->CopyTiles((ID3D12Resource*)Unwrap(r), c, sz, (ID3D12Resource*)Unwrap(b), off, f); }
static void __stdcall ListResolveSubresource19(Wrap* w, ID3D12Resource* d, UINT ds, ID3D12Resource* s, UINT ss, DXGI_FORMAT f)
{ ((ID3D12GraphicsCommandList*)w->real)->ResolveSubresource((ID3D12Resource*)Unwrap(d), ds, (ID3D12Resource*)Unwrap(s), ss, XboxFormatToPc(f)); }
static void __stdcall ListDiscardResource51(Wrap* w, ID3D12Resource* r, const D3D12_DISCARD_REGION* rg)
{ ((ID3D12GraphicsCommandList*)w->real)->DiscardResource((ID3D12Resource*)Unwrap(r), rg); }
static void __stdcall ListSetPredication55(Wrap* w, ID3D12Resource* b, UINT64 off, D3D12_PREDICATION_OP op)
{ ((ID3D12GraphicsCommandList*)w->real)->SetPredication((ID3D12Resource*)Unwrap(b), off, op); }
static void __stdcall ListExecuteIndirect59(Wrap* w, ID3D12CommandSignature* sig, UINT mx, ID3D12Resource* a, UINT64 ao, ID3D12Resource* c, UINT64 co)
{
    ProfListOp(w, OP_EXECINDIRECT); ((ID3D12GraphicsCommandList*)w->real)->ExecuteIndirect((ID3D12CommandSignature*)Unwrap(sig), mx, (ID3D12Resource*)Unwrap(a), ao, (ID3D12Resource*)Unwrap(c), co); }
static void __stdcall QueueUpdateTileMappings8(Wrap* w, ID3D12Resource* r, UINT n, const D3D12_TILED_RESOURCE_COORDINATE* sc, const D3D12_TILE_REGION_SIZE* rs, ID3D12Heap* h, UINT nr, const D3D12_TILE_RANGE_FLAGS* rf, const UINT* ho, const UINT* rc, D3D12_TILE_MAPPING_FLAGS f)
{ ((ID3D12CommandQueue*)w->real)->UpdateTileMappings((ID3D12Resource*)Unwrap(r), n, sc, rs, (ID3D12Heap*)Unwrap(h), nr, rf, ho, rc, f); }
static void __stdcall QueueCopyTileMappings9(Wrap* w, ID3D12Resource* d, const D3D12_TILED_RESOURCE_COORDINATE* dc, ID3D12Resource* s, const D3D12_TILED_RESOURCE_COORDINATE* sc, const D3D12_TILE_REGION_SIZE* rs, D3D12_TILE_MAPPING_FLAGS f)
{ ((ID3D12CommandQueue*)w->real)->CopyTileMappings((ID3D12Resource*)Unwrap(d), dc, (ID3D12Resource*)Unwrap(s), sc, rs, f); }
static void __stdcall ListBarriers26(Wrap* w, UINT count, const D3D12_RESOURCE_BARRIER* barriers)
{
    ProfListOp(w, OP_BARRIER);
    if (BootstrapNoopEnabled()) ++g_bootstrap.barriers;
    std::vector<D3D12_RESOURCE_BARRIER> b(barriers, barriers + count);
    for (auto& x : b) {
        if (x.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) {
            InvalidatePresentState(x.Transition.pResource);
            x.Transition.pResource = (ID3D12Resource*)Unwrap(x.Transition.pResource);
        } else if (x.Type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING) {
            InvalidatePresentState(x.Aliasing.pResourceBefore);
            InvalidatePresentState(x.Aliasing.pResourceAfter);
            x.Aliasing.pResourceBefore = (ID3D12Resource*)Unwrap(x.Aliasing.pResourceBefore);
            x.Aliasing.pResourceAfter = (ID3D12Resource*)Unwrap(x.Aliasing.pResourceAfter);
        }
        else if (x.Type == D3D12_RESOURCE_BARRIER_TYPE_UAV) x.UAV.pResource = (ID3D12Resource*)Unwrap(x.UAV.pResource);
    }
    ((ID3D12GraphicsCommandList*)w->real)->ResourceBarrier(count, b.data());
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
// ---- Xbox-расширения списка команд (раскладка GTA VI = «новая» раскладка VIctory; слот s>=59 → база s-2) ----
// 70/75/126/142 — состояние, не имеющее PC-аналога (depth bounds, PS depth order, shading rate, неизвестное): игнорируются.
// 74 — 64-битное ожидание значения в памяти: списки у нас выполняются до конца, ждать нечего.
// 131/132 — sync top/bottom-of-pipe: порядок обеспечивает хост-список. 146 — группы барьеров: один UAV-барьер на всё (как у VIctory).
// 148 — AllocateUploadMemory(size, alignment) -> указатель на обнулённую память, принадлежащую списку, освобождается при Reset.
static void __stdcall ListIgnored(Wrap*) {}
static void __stdcall ListWait74(Wrap*, UINT64, UINT, UINT64) {}
static void __stdcall ListSync(Wrap*, UINT, UINT, UINT64, UINT64) {}
static void __stdcall ListBarrierGroups146(Wrap* w, UINT count, const void* /*groups*/)
{
    if (!count) return;
    D3D12_RESOURCE_BARRIER b = {}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; b.UAV.pResource = nullptr;
    ((ID3D12GraphicsCommandList*)w->real)->ResourceBarrier(1, &b);
}
static std::map<Wrap*, std::vector<void*>> g_upload; static SRWLOCK g_uploadLock = SRWLOCK_INIT;
static void FreeUploadFor(Wrap* w)
{
    std::vector<void*> v; { ExclusiveSrwLock g(g_uploadLock); auto it = g_upload.find(w); if (it == g_upload.end()) return; v.swap(it->second); g_upload.erase(it); }
    for (void* p : v) _aligned_free(p);
}
static void* __stdcall ListAllocateUpload148(Wrap* w, UINT64 size, UINT64 alignment)
{
    if (alignment < 16) alignment = 16;
    if (alignment & (alignment - 1)) { XS_LOG("list slot148 AllocateUpload: alignment %#llx не степень двойки -> NULL", (unsigned long long)alignment); return nullptr; }
    void* p = _aligned_malloc((size_t)(size ? size : 1), (size_t)alignment); if (!p) return nullptr;
    memset(p, 0, (size_t)size);
    { ExclusiveSrwLock g(g_uploadLock); g_upload[w].push_back(p); }
    static std::atomic<int> n{0}; if (++n <= 6) XS_LOG("list slot148 AllocateUpload size=%#llx align=%#llx -> %p", (unsigned long long)size, (unsigned long long)alignment, p);
    return p;
}
static HRESULT __stdcall List_Reset(Wrap* w, ID3D12CommandAllocator* a, ID3D12PipelineState* p)
{
    ProfListReset(w);
    FreeUploadFor(w);
    const HRESULT hr = ((ID3D12GraphicsCommandList*)w->real)->Reset((ID3D12CommandAllocator*)Unwrap(a), (ID3D12PipelineState*)Unwrap(p));
    if (SUCCEEDED(hr)) {
        DiscardListWrites(w);
        if (GfxNativeEnabled()) GfxNativeOnReset(w);
        if (GfxRecordOn()) { ExclusiveSrwLock g(g_xstateLock); g_xstate[w]=XListState{};g_xstate[w].pso=(uintptr_t)p; }
    }
    return hr;
}
// Game+0x2474cce calls list+0x458 with a device9-created allocator in RDX,
// R8=0 and R9=0, then checks HRESULT. The missing reset left PC lists closed:
// subsequent Close calls returned E_FAIL. Only the observed zero extension
// word is supported; its full Xbox meaning is not yet established.
static HRESULT __stdcall List_ResetX139(Wrap* w, ID3D12CommandAllocator* allocator,
    ID3D12PipelineState* initialState, UINT64 extension)
{
    ProfListReset(w);
    if (extension) {
        XS_LOG("list slot139 ResetX unsupported extension=%#llx", (unsigned long long)extension);
        return E_NOTIMPL;
    }
    const HRESULT hr = List_Reset(w, allocator, initialState);
    static std::atomic<unsigned> calls{0};
    const unsigned n = ++calls;
    if (n <= 8 || FAILED(hr)) XS_LOG("list slot139 ResetX allocator=%p initial=%p -> PC Reset hr=0x%08lX", (void*)allocator, (void*)initialState, (unsigned long)hr);
    return hr;
}
static uint64_t __stdcall Heap_GetCPUStart(Wrap* w) { if (w->pad[1]) return w->pad[1]; D3D12_CPU_DESCRIPTOR_HANDLE h = {}; ((void* (*)(void*, void*))(*(void***)w->real)[9])(w->real, &h); return h.ptr; }
static uint64_t __stdcall Heap_GetGPUStart(Wrap* w) { D3D12_GPU_DESCRIPTOR_HANDLE h = {}; ((void* (*)(void*, void*))(*(void***)w->real)[10])(w->real, &h); return h.ptr; }
// Слот 9 allocator'а (Xbox): наблюдался вызов (this, fn, ctx) из 0x1401884fe — регистрация колбэка; пока лог + S_OK.
static HRESULT __stdcall Alloc_Slot9(Wrap*, uintptr_t fn, uintptr_t ctx) { static std::atomic<int> n{0}; if (n++ < 4) XS_LOG("allocator slot9 (callback?) fn=%p ctx=%p -> S_OK", (void*)fn, (void*)ctx); return S_OK; }
static HRESULT __stdcall Queue_ExecuteX35(Wrap* w, UINT n, ID3D12CommandList* const* lists, uint64_t x, uint64_t y);
static void InstallObjectReturnTrace();
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
    for (int i = 0; i < 15; ++i) g_vtRes[i] = (void*)fw[i];          // ID3D12Resource: 0..14
    g_vtRes[0] = (void*)Obj_QI; g_vtRes[2] = (void*)Res_Release; g_vtRes[7] = (void*)Obj_GetDevice; g_vtRes[8] = (void*)Res_Map; g_vtRes[9] = (void*)Res_Unmap; g_vtRes[11] = (void*)Res_GetGPUVirtualAddress;
    for (int i = 0; i < 12; ++i) g_vtFence[i] = (void*)fw[i];
    g_vtFence[0] = (void*)FenceQI; g_vtFence[1] = (void*)FenceAddRef; g_vtFence[2] = (void*)FenceRelease;
    g_vtFence[7] = (void*)Obj_GetDevice; g_vtFence[8] = (void*)FenceCompleted8;
    g_vtFence[10] = (void*)FenceSignal10; g_vtFence[12] = (void*)FenceAddress12;
    void** vts[3] = { g_vtQueue, g_vtList, g_vtHeap };
    for (void** vt : vts) { vt[0] = (void*)Obj_QI; vt[7] = (void*)Obj_GetDevice; }
    g_vtQueue[10] = (void*)Queue_ExecuteCommandLists; g_vtQueue[18] = (void*)Queue_TimestampFrequency18; g_vtQueue[35] = (void*)Queue_ExecuteX35; g_vtList[28] = (void*)List_SetDescriptorHeaps; g_vtList[25] = (void*)List_SetPipelineState; g_vtList[10] = (void*)List_Reset;
    g_vtList[139] = (void*)List_ResetX139;
    g_vtList[71] = (void*)ListWrite32_71; g_vtList[72] = (void*)ListWrite64_72;
    for (int i : {70, 75, 126, 142}) g_vtList[i] = (void*)ListIgnored;
    g_vtList[74] = (void*)ListWait74; g_vtList[131] = (void*)ListSync; g_vtList[132] = (void*)ListSync;
    g_vtList[146] = (void*)ListBarrierGroups146; g_vtList[148] = (void*)ListAllocateUpload148;
    g_vtQueue[14] = (void*)QueueSignal14; g_vtQueue[15] = (void*)QueueWait15;
    g_vtQueue[16] = (void*)QueueSlot16;
    g_vtQueue[32] = (void*)QueuePresent32;
    g_vtList[12] = (void*)ListDraw12; g_vtList[13] = (void*)ListDrawIndexed13;
    g_vtList[14] = (void*)ListDispatch14; g_vtList[26] = (void*)ListBarriers26;
    g_vtList[15] = (void*)ListCopyBufferRegion15; g_vtList[16] = (void*)ListCopyTextureRegion16; g_vtList[17] = (void*)ListCopyResource17; g_vtList[18] = (void*)ListCopyTiles18;
    g_vtList[19] = (void*)ListResolveSubresource19; g_vtList[51] = (void*)ListDiscardResource51; g_vtList[55] = (void*)ListSetPredication55; g_vtList[59] = (void*)ListExecuteIndirect59;
    g_vtQueue[8] = (void*)QueueUpdateTileMappings8; g_vtQueue[9] = (void*)QueueCopyTileMappings9;
    g_vtList[52] = (void*)List_Slot52; g_vtList[53] = (void*)List_EndQuery53; g_vtList[54] = (void*)List_ResolveQuery54;
    g_vtList[34] = (void*)ListSlot34;
    g_vtList[36] = (void*)ListSlot36; g_vtList[40] = (void*)ListSlot40;
    g_vtHeap[9] = (void*)Heap_GetCPUStart; g_vtHeap[10] = (void*)Heap_GetGPUStart;
    g_vtList[46] = (void*)List_OMSetRenderTargets; g_vtList[47] = (void*)ListSlot47; g_vtList[48] = (void*)ListSlot48; g_vtList[49] = (void*)List_ClearUAVUint; g_vtList[50] = (void*)List_ClearUAVFloat;
    GfxNativeInstallListHooks();   // при XSX_DEV_NATIVE_GFX=1 переназначает list50/51/52 (RT/clear по статусу пользователя)
    if (RootTraceOn() || DrawTraceOn() || GfxRecordOn()) {
        g_originalRootSlots[29] = g_vtList[29]; g_vtList[29] = (void*)&TraceFwdSlot<29>; g_originalRootSlots[30] = g_vtList[30]; g_vtList[30] = (void*)&TraceFwdSlot<30>;
        g_originalRootSlots[31] = g_vtList[31]; g_vtList[31] = (void*)&TraceFwdSlot<31>; g_originalRootSlots[32] = g_vtList[32]; g_vtList[32] = (void*)&TraceFwdSlot<32>;
    }
    InstallBootstrapRootSlots(std::make_integer_sequence<int, 14>{}); // Xbox root33..46, PC contracts retained.
    InstallObjectReturnTrace();
}

// These are explicit contracts, not guesses from return-register bits. In
// particular, zero refcounts/void returns are not failed HRESULTs.
template<int Kind> static void TraceObjectBase(void** vt)
{
    using namespace xsx::returns;
    Install<Kind, 0>(vt, &Obj_QI, "QueryInterface");
    InstallMember<Kind, 1, Wrap>(vt, &IUnknown::AddRef, "AddRef", Contract::ReferenceCount);
    InstallMember<Kind, 2, Wrap>(vt, &IUnknown::Release, "Release", Contract::ReferenceCount);
    InstallMember<Kind, 3, Wrap>(vt, &ID3D12Object::GetPrivateData, "GetPrivateData");
    InstallMember<Kind, 4, Wrap>(vt, &ID3D12Object::SetPrivateData, "SetPrivateData");
    InstallMember<Kind, 5, Wrap>(vt, &ID3D12Object::SetPrivateDataInterface, "SetPrivateDataInterface");
    InstallMember<Kind, 6, Wrap>(vt, &ID3D12Object::SetName, "SetName");
    if constexpr (Kind != 1) Install<Kind, 7>(vt, &Obj_GetDevice, "GetDevice");
}
static void InstallObjectReturnTrace()
{
    if (!ReturnTraceMode()) return;
    using namespace xsx::returns;
    TraceObjectBase<2>(g_vtQueue); TraceObjectBase<3>(g_vtList); TraceObjectBase<4>(g_vtHeap);
    TraceObjectBase<5>(g_vtPso); TraceObjectBase<6>(g_vtAlloc); TraceObjectBase<7>(g_vtFence); TraceObjectBase<8>(g_vtRes);
    Install<2, 14>(g_vtQueue, &QueueSignal14, "Signal");
    Install<2, 15>(g_vtQueue, &QueueWait15, "Wait");
    Install<2, 16>(g_vtQueue, &QueueSlot16, "XboxSignalOrPCFrequency");
    InstallMember<2, 17, Wrap>(g_vtQueue, &ID3D12CommandQueue::GetClockCalibration, "GetClockCalibration");
    Install<2, 18>(g_vtQueue, &Queue_TimestampFrequency18, "GetTimestampFrequency");
    Install<2, 32>(g_vtQueue, &QueuePresent32, "PresentX");
    Install<2, 35>(g_vtQueue, &Queue_ExecuteX35, "ExecuteCommandListsX");
    InstallMember<3, 9, Wrap>(g_vtList, &ID3D12GraphicsCommandList::Close, "Close");
    Install<3, 10>(g_vtList, &List_Reset, "Reset");
    Install<3, 139>(g_vtList, &List_ResetX139, "ResetX");
    Install<4, 9>(g_vtHeap, &Heap_GetCPUStart, "CPUStart", Contract::NonNull);
    // A non-shader-visible heap legitimately has no GPU handle.
    Install<4, 10>(g_vtHeap, &Heap_GetGPUStart, "GPUStart", Contract::Value);
    InstallMember<5, 8, Wrap>(g_vtPso, &ID3D12PipelineState::GetCachedBlob, "GetCachedBlob");
    InstallMember<6, 8, Wrap>(g_vtAlloc, &ID3D12CommandAllocator::Reset, "Reset");
    Install<6, 9>(g_vtAlloc, &Alloc_Slot9, "XboxCallback");
    InstallMember<7, 9, Wrap>(g_vtFence, &ID3D12Fence::SetEventOnCompletion, "SetEventOnCompletion");
    Install<7, 10>(g_vtFence, &FenceSignal10, "Signal");
    Install<7, 12>(g_vtFence, &FenceAddress12, "CompletionAddress", Contract::NonNull);
    InstallMember<8, 8, Wrap>(g_vtRes, &ID3D12Resource::Map, "Map");
    Install<8, 11>(g_vtRes, &Res_GetGPUVirtualAddress, "GPUVirtualAddress", Contract::NonNull);
    InstallMember<8, 12, Wrap>(g_vtRes, &ID3D12Resource::WriteToSubresource, "WriteToSubresource");
    InstallMember<8, 13, Wrap>(g_vtRes, &ID3D12Resource::ReadFromSubresource, "ReadFromSubresource");
    InstallMember<8, 14, Wrap>(g_vtRes, &ID3D12Resource::GetHeapProperties, "GetHeapProperties");
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
            int i = g_hrN.load(); uint64_t gpu = 0; if (d->Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) { D3D12_GPU_DESCRIPTOR_HANDLE gh = {}; ((void* (*)(void*, void*))(*(void***)*out)[10])(*out, &gh); gpu = gh.ptr; }
            g_hr[i] = { guestBase, guestSize, rs.ptr, gpu }; g_hrN.store(i + 1, std::memory_order_release);
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
// Реестр «адрес памяти игры → ресурс» (аналог shadow::find у VIctory): нужен для расшифровки дескрипторов-по-адресу (слоты 70/71) и трансляции GPU-адресов.
struct AddrRes { uint64_t size; void* res; uint32_t dim, fmt; };
static std::map<uint64_t, AddrRes> g_addrRes; static SRWLOCK g_addrResLock = SRWLOCK_INIT;
static void ForgetResourceAddress(Wrap* resource, void* address)
{
    ExclusiveSrwLock held(g_addrResLock);
    auto it = g_addrRes.find((uint64_t)address);
    if (it != g_addrRes.end() && it->second.res == resource) g_addrRes.erase(it);
}
static const AddrRes* FindAddrRes(uint64_t a, uint64_t* base = nullptr)
{
    auto it = g_addrRes.upper_bound(a); if (it == g_addrRes.begin()) return nullptr; --it;
    if (a >= it->first && a < it->first + (it->second.size ? it->second.size : 1)) { if (base) *base = it->first; return &it->second; }
    return nullptr;
}
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
    if (SUCCEEDED(hr) && out && *out && IsEqualGUID(riid, __uuidof(ID3D12Resource))) {   // как VIctory (Backing::Address): GPU VA ресурса на адресе == адрес памяти игры
        InitObjVts(); hr = WrapOutput(hr, out, g_vtRes, 8);
        if (SUCCEEDED(hr)) { { ExclusiveSrwLock held(g_resLock); g_resShadow[*out] = ResExtra{addr, 0}; }
            D3D12_RESOURCE_ALLOCATION_INFO ai = real->GetResourceAllocationInfo(0, 1, &dd); ExclusiveSrwLock h2(g_addrResLock); g_addrRes[(uint64_t)addr] = AddrRes{ ai.SizeInBytes != ~0ull ? ai.SizeInBytes : dd.Width, *out, (uint32_t)dd.Dimension, (uint32_t)dd.Format }; }
    }
    XS_LOG("DEVICE slot133 CreatePlacedResourceX addr=%p dim=%d width=%#llx state=%#x -> %s hr=0x%08lX res=%p", addr, (int)desc->Dimension, (unsigned long long)desc->Width, state, path, (unsigned long)hr, out ? *out : nullptr);
    return hr;
}
struct FrameHostClock {
    uint64_t NowNs() const {
        return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    void SleepUntilNs(uint64_t deadline) const {
        const auto at = std::chrono::steady_clock::time_point(
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::nanoseconds(deadline)));
        std::this_thread::sleep_until(at);
    }
};
static xsx::frame::Timeline g_frameClock;
static FrameHostClock g_frameHostClock;
static void FrameClockSnapshot()
{
    xsx::frame::State state;
    // Fault observation must not wait for a lock held by a faulting thread.
    if (!g_frameClock.TrySnapshot(state)) { XS_LOG("FRAME CLOCK snapshot busy"); return; }
    XS_LOG("FRAME CLOCK interval_us=%u period=%u origin_tokens=%llu reserved_ticks=%llu,%llu,%llu (CPU clock, no scanout statistics)",
        state.interval_us, state.period, (unsigned long long)state.frames,
        (unsigned long long)state.reserved[0], (unsigned long long)state.reserved[1], (unsigned long long)state.reserved[2]);
}
// Local+0x247abd8 and the public GDK FramePacing sample: output, us, depth,
// flags. Preserve this signature separately from ScheduleFrameEventX.
static HRESULT __stdcall Dev_SetFrameInterval78(Wrap*, void* output, UINT intervalUs, UINT period, UINT flags)
{
    if (output || flags) return E_NOTIMPL; // Only the observed default-output form.
    g_frameClock.SetInterval(g_frameHostClock.NowNs(), intervalUs, period);
    static std::atomic<unsigned> calls{0};
    const unsigned call = ++calls;
    if (call <= 8 || !(call % 256)) XS_LOG("DEVICE slot78 SetFrameIntervalX interval_us=%u period=%u flags=%#x -> S_OK", intervalUs, period, flags);
    return S_OK;
}
static HRESULT __stdcall Dev_ScheduleFrameEvent79(Wrap*, UINT kind, UINT offsetUs, const void* list, UINT flags)
{
    if (kind > 2) return E_INVALIDARG;
    // Observed game events use zero offset and no GPU command list. Do not
    // claim GPU-triggered scheduling or offsets we do not implement.
    if (offsetUs || list || flags) return E_NOTIMPL;
    static std::atomic<unsigned> calls{0};
    if (calls++ < 8) XS_LOG("DEVICE slot79 ScheduleFrameEventX kind=%u offset_us=0 -> S_OK (CPU clock)", kind);
    return S_OK;
}

// Game+0x193e1b4: heap, flags=ALLOW_DISPLAY, desc1, layout, clear,
// protected session, castable count/formats, IID_ID3D12Resource, out.
// The eleventh argument is pre-call rsp+0x50. Match that ABI, not a guessed
// three-argument allocation method. No title offsets or budget padding.
static HRESULT __stdcall Dev_CreateCommittedResource127(Wrap* w,
    const D3D12_HEAP_PROPERTIES* heap, D3D12_HEAP_FLAGS flags,
    const D3D12_RESOURCE_DESC1* desc, D3D12_BARRIER_LAYOUT layout,
    const D3D12_CLEAR_VALUE* clear, ID3D12ProtectedResourceSession* protectedSession,
    UINT castableCount, const DXGI_FORMAT* castableFormats, REFIID iid, void** out)
{
    if (out) *out = nullptr;
    D3D12_RESOURCE_ALLOCATION_INFO info{};
    D3D12_RESOURCE_ALLOCATION_INFO hostInfo{};
    bool guestLayout = false;
    ResExtra extra{};
    const HRESULT hr = [&]() -> HRESULT {
        if (!heap || !desc) return E_INVALIDARG;
        if (!IsEqualGUID(iid, __uuidof(ID3D12Resource))) return E_NOINTERFACE;
        // Only the observed unprotected, non-castable form is implemented.
        if (protectedSession || castableCount || castableFormats) return E_NOTIMPL;
        if (desc->Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
            desc->Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) return E_NOTIMPL;
        if (desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER &&
            (layout != D3D12_BARRIER_LAYOUT_UNDEFINED || !desc->Width)) return E_INVALIDARG;
        D3D12_RESOURCE_DESC base{};
        std::memcpy(&base, desc, sizeof base);
        base = PcResourceDesc(base);
        D3D12_RESOURCE_DESC1 pc = *desc;
        pc.Format = base.Format; pc.Flags = base.Flags; pc.Layout = base.Layout;
        // DESC1's mip region starts in DESC's tail padding: do not memcpy a
        // whole DESC over it. Preserve sampler-feedback fields for the backend.
        D3D12_CLEAR_VALUE converted{};
        if (clear) { converted = *clear; converted.Format = XboxFormatToPc(clear->Format); clear = &converted; }
        ID3D12Device10* native = nullptr;
        HRESULT result = ((IUnknown*)w->real)->QueryInterface(__uuidof(ID3D12Device10), (void**)&native);
        if (FAILED(result)) return result;
        ID3D12Resource* resource = nullptr;
        result = native->CreateCommittedResource3(heap, flags, &pc, layout, clear,
            nullptr, 0, nullptr, iid, out ? (void**)&resource : nullptr);
        if (SUCCEEDED(result) && resource) info = native->GetResourceAllocationInfo2(0, 1, &pc, nullptr);
        native->Release();
        if (FAILED(result) || !out) return result; // Validation-only S_FALSE: no allocation.
        if (!resource) return E_FAIL;
        hostInfo = info;
        if (desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
            // Standard PC buffer semantics, as in the existing committed76
            // path: native Map and native GPU VA. There is no unused CPU
            // shadow/title allocation and no claim of console UMA coherence.
            // Buffer InitialLayout must be UNDEFINED; let the backend validate
            // it, heap properties, clear value, flags and all DESC1 fields.
            InitObjVts();
            void* output = resource;
            const HRESULT wrapped = WrapOutput(result, &output, g_vtRes, 8);
            if (SUCCEEDED(wrapped)) *out = output;
            return wrapped;
        }
        // CPU memory belongs to the console layout, not Vulkan's private
        // image metadata. Retain native sizing only for unimplemented layouts.
        D3D12_RESOURCE_DESC guest{}; std::memcpy(&guest, desc, sizeof guest);
        guestLayout = GuestColorFootprint(guest, info);
        if (!info.SizeInBytes || info.SizeInBytes == ~0ull || !info.Alignment ||
            (info.Alignment & (info.Alignment - 1)) || info.Alignment > (1ull << 28) ||
            info.SizeInBytes > SIZE_MAX - info.Alignment) {
            resource->Release(); return E_INVALIDARG;
        }
        extra.size = (size_t)info.SizeInBytes;
        // Only these initial legacy states are known. Recording a title
        // transition invalidates this knowledge; it does not prove execution.
        switch (layout) {
        case D3D12_BARRIER_LAYOUT_COMMON:
            extra.presentState = D3D12_RESOURCE_STATE_COMMON; extra.presentStateKnown = true; break;
        case D3D12_BARRIER_LAYOUT_RENDER_TARGET:
            extra.presentState = D3D12_RESOURCE_STATE_RENDER_TARGET; extra.presentStateKnown = true; break;
        case D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS:
            extra.presentState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS; extra.presentStateKnown = true; break;
        default: break;
        }
        auto hooks = (void**)GetProcAddress(GetModuleHandleW(nullptr), "__XMemAllocationHooks__");
        if (hooks) {
            if (!hooks[0] || !hooks[1]) { resource->Release(); return E_NOTIMPL; }
            unsigned lg = 0;
            for (uint64_t alignment = info.Alignment; alignment > 1; alignment >>= 1) ++lg;
            // Page class is independent of alignment. In particular, a 4MiB
            // resource may require 64KiB alignment, not 2MiB alignment.
            const uint64_t pages = info.SizeInBytes >= (2ull << 20) ? 2 : 1;
            extra.attributes = (128ull << 16) | (pages << 14) | ((uint64_t)lg << 24) |
                ((heap->Type == D3D12_HEAP_TYPE_DEFAULT ? 1ull : 3ull) << 29);
            extra.freeHook = (ResourceFreeHook)hooks[1];
            extra.shadow = ((void* (__stdcall*)(size_t, uint64_t))hooks[0])(extra.size, extra.attributes);
            extra.ownedBase = extra.shadow;
            // A title allocator failure must not escape its budget through a
            // fallback VirtualAlloc. The same hook and attributes free it.
        } else {
            extra.ownedBase = VirtualAlloc(nullptr, extra.size + (size_t)info.Alignment - 1,
                MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            if (extra.ownedBase) extra.shadow = (void*)(((uintptr_t)extra.ownedBase + info.Alignment - 1) & ~(info.Alignment - 1));
        }
        if (!extra.shadow) { resource->Release(); return E_OUTOFMEMORY; }
        if ((uintptr_t)extra.shadow & (info.Alignment - 1)) {
            XS_LOG("RESOURCE127 allocator alignment failure address=%p required=%#llx attributes=%#llx",
                extra.shadow, (unsigned long long)info.Alignment, (unsigned long long)extra.attributes);
            FreeOwnedShadow(extra); resource->Release(); return E_INVALIDARG;
        }
        std::memset(extra.shadow, 0, extra.size);
        InitObjVts();
        auto* wrapped = NewWrap(g_vtRes, resource, 8);
        if (!wrapped) { FreeOwnedShadow(extra); resource->Release(); return E_OUTOFMEMORY; }
        try {
            {
                ExclusiveSrwLock held(g_resLock);
                g_resShadow[wrapped] = extra;
            }
            {
                ExclusiveSrwLock held(g_addrResLock);
                g_addrRes[(uint64_t)extra.shadow] = AddrRes{info.SizeInBytes, wrapped, (uint32_t)base.Dimension, (uint32_t)base.Format};
            }
        } catch (const std::bad_alloc&) {
            { ExclusiveSrwLock held(g_resLock); g_resShadow.erase(wrapped); }
            FreeOwnedShadow(extra); resource->Release(); return E_OUTOFMEMORY;
        }
        *out = wrapped;
        return S_OK;
    }();
    CALLER(caller);
    const bool buffer = desc && desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER;
    XS_LOG("DEVICE slot127 CreateCommittedResource3 %llux%u fmt=%d heap_flags=%#x layout=%#x desc_layout=%#x guest_bytes=%#llx host_bytes=%#llx align=%#llx sizing=%s -> hr=0x%08lX res=%p native=%p CPU=%p owner=%s caller=%s",
        desc ? (unsigned long long)desc->Width : 0, desc ? desc->Height : 0, desc ? (int)desc->Format : 0,
        (unsigned)flags, (unsigned)layout, desc ? (unsigned)desc->Layout : 0,
        (unsigned long long)(buffer ? 0 : info.SizeInBytes), (unsigned long long)hostInfo.SizeInBytes,
        (unsigned long long)(buffer ? hostInfo.Alignment : info.Alignment),
        buffer ? "native-buffer" : guestLayout ? "GFX10-color64K" : "native-fallback",
        (unsigned long)hr, out ? *out : nullptr, out && *out ? Unwrap(*out) : nullptr, SUCCEEDED(hr) ? extra.shadow : nullptr,
        buffer ? "native-buffer" : extra.freeHook ? "title" : "VirtualAlloc", caller);
    if (buffer) XS_LOG("BUFFER127 heap_type=%u cpu_pages=%u pool=%u flags=%#x native_GPU_VA=%#llx hr=0x%08lX (native Map/VA; no guest UMA bridge)",
        heap ? (unsigned)heap->Type : 0, heap ? (unsigned)heap->CPUPageProperty : 0,
        heap ? (unsigned)heap->MemoryPoolPreference : 0, (unsigned)desc->Flags,
        (unsigned long long)(SUCCEEDED(hr) && out && *out ? ((ID3D12Resource*)Unwrap(*out))->GetGPUVirtualAddress() : 0),
        (unsigned long)hr);
    return hr;
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
// ---- Инвентарь нативных PSO (XSX_PSO_INVENTORY=1): ищет в первых 256 байтах описания пары (указатель, размер), на которые указывают
// блоки метаданных шейдера (заголовок {2, size-12}, 128-битный хеш программы перед 12 нулевыми байтами), и пишет хеши в лог. Описание НЕ расшифровано.
static thread_local std::vector<std::string> g_lastPsoBlobs;
static std::map<void*, std::vector<std::string>> g_psoBlobs;
static std::string InventoryPso(int slot, const void* desc)
{
    std::string hs;
    static bool on = EnvOn("XSX_PSO_INVENTORY") || EnvOn("XSX_DRAW_TRACE"); if (!on || !desc) return hs;
    static std::atomic<unsigned> seq{0}; const unsigned n = seq++;
    uint64_t w[32] = {}; if (!ReadPsoMemory(desc, w, sizeof w)) { XS_LOG("PSOINV slot%d #%u: описание нечитаемо", slot, n); return hs; }
    char line[1200]; int o = snprintf(line, sizeof line, "PSOINV slot%d #%u", slot, n);
    for (int i = 0; i < 32; i++) {
        const uint64_t ptr = w[i]; if (ptr < 0x10000 || (ptr >> 47)) continue;
        for (int d = -1; d <= 1; d += 2) {
            const int j = i + d; if (j < 0 || j >= 32) continue; const uint64_t sz = w[j];
            if (sz < 28 || sz > 4096) continue;
            uint8_t blob[4096]; if (!ReadPsoMemory((void*)ptr, blob, (size_t)sz)) continue;
            uint32_t h0 = *(uint32_t*)blob, h1 = *(uint32_t*)(blob + 4); static const uint8_t zero12[12] = {};
            if (h0 != 2 || h1 != sz - 12 || memcmp(blob + sz - 12, zero12, 12)) continue;
            o += snprintf(line + o, sizeof line - o, " [q%d sz=%#llx hash=", i, (unsigned long long)sz);
            char hx[40]; for (int k = 0; k < 16; k++) snprintf(hx + k * 2, 3, "%02x", blob[sz - 28 + k]);
            o += snprintf(line + o, sizeof line - o, "%s]", hx); hs += hx; hs += ' '; break;
        }
    }
    g_lastPsoBlobs.clear();
    for (int i = 0; i < 32; i++) {
        const uint64_t ptr = w[i]; if (ptr < 0x10000 || (ptr >> 47)) continue;
        for (int d = -1; d <= 1; d += 2) {
            const int j = i + d; if (j < 0 || j >= 32) continue; const uint64_t sz = w[j]; if (sz < 16 || sz > 4096) continue;
            uint8_t blob[4096]; if (!ReadPsoMemory((void*)ptr, blob, (size_t)sz)) continue;
            const uint32_t magic = *(uint32_t*)blob;
            (void)magic; { char tag[32]; snprintf(tag, sizeof tag, "q%d:", i); g_lastPsoBlobs.push_back(std::string(tag) + std::string((char*)blob, (size_t)sz)); break; }
        }
    }
    { static bool verbose = EnvOn("XSX_PSO_INVENTORY"); if (verbose) XS_LOG("%s", line); }
    return hs;
}
static std::map<void*, std::string> g_psoHashes; static SRWLOCK g_psoHashLock = SRWLOCK_INIT;
static std::set<std::string> g_usedPso;
// Game RVA0x2477578: (this, desc, IID_ID3D12PipelineState, out).
// The following code tests HRESULT and packs *out >> 1 into the PSO cache.
// Returning S_OK without writing *out cached exe+0x6993a4, then faulted on
// AddRef at RVA0x247a09e. No native description may reach a PC PSO API until
// its shader/fixed-state layout has been decoded and translated.
static HRESULT __stdcall Dev_NativePso57(Wrap* w, const void* desc, REFIID iid, void** out)
{
    PROF_SCOPE(PROF_PSO57);
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!desc) return E_INVALIDARG;
    if (!IsEqualGUID(iid, __uuidof(ID3D12PipelineState))) return E_NOINTERFACE;
    CapturePso57(desc, iid, __builtin_return_address(0)); std::string hashes = InventoryPso(57, desc);
    if (GfxNativeEnabled()) {
        const HRESULT hgn = GfxNativeCreatePso57(w, desc, iid, out);
        if (SUCCEEDED(hgn) && *out) { if (!hashes.empty()) { ExclusiveSrwLock g(g_psoHashLock); g_psoHashes[*out] = hashes; g_psoBlobs[*out] = g_lastPsoBlobs; } return hgn; }
    }
    if (BootstrapNoopEnabled()) { HRESULT hr = BootstrapGraphicsOutput(w, iid, out); if (SUCCEEDED(hr) && *out && !hashes.empty()) { ExclusiveSrwLock g(g_psoHashLock); g_psoHashes[*out] = hashes; g_psoBlobs[*out] = g_lastPsoBlobs; } return hr; }
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
    LogNativePso("BVH-builder-style", 62, desc); InventoryPso(62, desc);
    ID3D12PipelineState* st = GetStubComputePso((ID3D12Device*)w->real);
    if (st) { HRESULT hr = st->QueryInterface(riid, ppv); InitObjVts(); hr = WrapOutput(hr, ppv, g_vtPso, 5);
        XS_LOG("DEVICE slot62 -> ЗАГЛУШКА: пустой compute-PSO %p (BVH не функционален) hr=0x%08lX", (void*)st, (unsigned long)hr); return hr; }
    if (ppv) *ppv = nullptr; return E_NOTIMPL;
}
static HRESULT __stdcall Dev_NativePso67(Wrap* w, const void* desc, REFIID iid, void** ppv)
{
    LogNativePso("FFX-style", 67, desc); InventoryPso(67, desc);
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
    LogNativePso("game-style", 86, desc); InventoryPso(86, desc);
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
    if (xbox && n == 1 && ret->SizeInBytes != ~0ull) GuestColorFootprint(descs[0], *ret);
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
    if (xbox && n == 1 && ret->SizeInBytes != ~0ull && GuestColorFootprint(descs[0], *ret) && info1) {
        auto* perResource = (D3D12_RESOURCE_ALLOCATION_INFO1*)info1;
        perResource[0].Offset = 0; perResource[0].SizeInBytes = ret->SizeInBytes;
        perResource[0].Alignment = ret->Alignment;
    }
    static std::atomic<int> k{0}; if (k++ < 6 || ret->SizeInBytes > 0xFFFFFFFFull) XS_LOG("DEVICE slot108 GetResourceAllocationInfo1(%s) n=%u dim=%d %llux%u fmt=%d -> size=%#llx align=%#llx", xbox ? "Xbox" : "PC", n, n ? (int)descs[0].Dimension : -1, n ? (unsigned long long)descs[0].Width : 0ull, n ? descs[0].Height : 0u, n ? (int)descs[0].Format : -1, (unsigned long long)ret->SizeInBytes, (unsigned long long)ret->Alignment);
    return ret;
}

// Queue слот 35 (+0x118) — ExecuteCommandListsX(count, lists**, X, 0) [callsite 0x142482689: edx=1, r8=&массив списков, r9=64-бит значение (вероятно fence/flags)]: разворачиваем списки и вызываем PC ExecuteCommandLists.
static HRESULT __stdcall Queue_ExecuteX35(Wrap* w, UINT n, ID3D12CommandList* const* lists, uint64_t x, uint64_t y)
{
    PROF_SCOPE(PROF_SUBMIT);
    { static bool on = EnvOn("XSX_SHADOW"); if (on) ShadowSyncAll(); }
    const HRESULT hr = GfxNativeEnabled() ? GfxNativeSubmit(w, n, lists) : SubmitListsWithWrites(w, n, lists);
    if (BootstrapNoopEnabled()) ++g_bootstrap.execute;
    static std::atomic<int> k{0}; if (k++ < 6) XS_LOG("queue slot35 ExecuteCommandListsX n=%u lists[0]=%p x=%#llx y=%#llx", n, n ? (void*)lists[0] : nullptr, (unsigned long long)x, (unsigned long long)y);
    return hr;
}
// Device слот 8 (PC CreateCommandQueue): тоже оборачиваем.
static HRESULT __stdcall Dev_CreateCommandQueue8(Wrap* w, const D3D12_COMMAND_QUEUE_DESC* d, REFIID riid, void** out)
{
    HRESULT hr = ((ID3D12Device*)w->real)->CreateCommandQueue(d, riid, out); InitObjVts(); hr = WrapOutput(hr, out, g_vtQueue, 2);
    XS_LOG("DEVICE slot8 CreateCommandQueue(PC) type=%d -> hr=0x%08lX q=%p", d ? (int)d->Type : -1, (unsigned long)hr, out ? *out : nullptr); return hr;
}

// Local+0x2482448 polls origin0 with timeout0/flag2. The flip2 wait uses
// INFINITE/flags0 and must keep its existing token. No title data is touched
// unless the event actually creates an origin token.
static HRESULT __stdcall Dev_WaitFrameEvent80(Wrap*, UINT kind, UINT timeoutMs, const void* list, UINT flags, uint64_t* out)
{
    if (list) return E_NOTIMPL;
    if (out && IsBadWritePtr(out, sizeof *out)) return E_POINTER;
    const auto result = g_frameClock.Wait(g_frameHostClock, kind, timeoutMs, flags);
    if (result.token_created && out) *out = result.token;
    static std::atomic<unsigned> calls{0};
    const unsigned call = ++calls;
    if (call <= 12 || !(call % 256)) XS_LOG("DEVICE slot80 WaitFrameEventX kind=%u timeout_ms=%#x flags=%#x -> hr=0x%08lX token_action=%s new_token=%llu tick=%llu wait_us=%llu",
        kind, timeoutMs, flags, (unsigned long)(uint32_t)result.status,
        result.token_created ? "created" : "preserved", (unsigned long long)result.token,
        (unsigned long long)result.tick, (unsigned long long)(result.wait_ns / 1000));
    return (HRESULT)result.status;
}
// GetFrameStatisticsX(token, types, in/out count, records). Game+0x2482710
// walks 56-byte records and reads a timestamp at +0x18, not a page size.
// GDK FramePacing.cpp accepts S_FALSE/unavailable. Until complete frame
// records exist, return no records; do not expose a partially initialized one.
static HRESULT __stdcall Dev_QueueInfo81(Wrap*, UINT64 token, uint32_t types, uint32_t* count, void* /*out*/)
{
    if (!count) return E_POINTER;
    *count = 0;
    static std::atomic<unsigned> calls{0};
    if (calls++ < 4) XS_LOG("DEVICE slot81 GetFrameStatisticsX token=%llu types=%#x -> S_FALSE count=0 (unavailable)", (unsigned long long)token, types);
    return S_FALSE;
}

// Device слоты 70/71 (+0x230/+0x238) — «placed» SRV/UAV (по VIctory: берут сырой ресурс и слова описания вида): (this, A, B, C).
// B — описание ресурса: B[5] = адрес памяти ресурса в памяти игры, B[3] = размер/число элементов, B[0] = {format?|dim?}; A — слова описания вида (A[2] = число элементов/размер).
// C — адрес дескриптора В ПАМЯТИ ИГРЫ (шаг 0x40). Модель VIctory: на хосте дескриптор НЕ эмулируется побитно — слот игры затирается программной записью
// {тег 'vVIE', индекс в глобальной таблице, поколение}, а при отрисовке запись разворачивается в настоящий PC-вид. Здесь: реестр + запись в C + попытка разрешить ресурс по адресу.
struct ViewRec { uint64_t A[8], B[8]; uint32_t kind, gen; uint64_t base; const void* res; };
static std::vector<ViewRec> g_views; static SRWLOCK g_viewLock = SRWLOCK_INIT; static std::atomic<uint32_t> g_viewResolved{0}, g_viewTotal{0};
static constexpr uint32_t kViewTag = 0x45495676;   // "vVIE"
// Буферный placed-вид -> нативный вид на теневой копии памяти игры, записанный в нативный heap по позиции дескриптора (XSX_SHADOW=1).
static bool NativeHeapSlot(uint64_t guest, uint64_t* real) { int n = g_hrN.load(std::memory_order_acquire); for (int i = 0; i < n; i++) { const HRange& r = g_hr[i]; if (guest - r.guest < r.size) { *real = r.real + (guest - r.guest); return true; } } return false; }
static void MirrorPlacedBuffer(ID3D12Device* dev, uint32_t kind, const ViewRec& r, uintptr_t C)
{
    static std::atomic<int> logn{0}; const bool log = logn++ < 16;
    const uint32_t fmt = (uint32_t)(r.B[0] & 0xffffffff); uint64_t real = 0;
    if (!NativeHeapSlot(C, &real)) { if (log) XS_LOG("SHADOWVIEW: handle %p вне зарегистрированных heap — пропуск", (void*)C); return; }
    const uint64_t address = r.B[5];
    if (kind == 0) {   // SRV: слова 4-5 FirstElement, 6 NumElements, 7 Stride, 8 Flags
        const uint32_t stride = (uint32_t)(r.B[3] >> 32), num = (uint32_t)(r.B[3] & 0xffffffff), flags = (uint32_t)(r.B[4] & 0xffffffff), mapping = (uint32_t)(r.B[1] & 0xffffffff);
        const uint64_t first = r.B[2]; uint32_t elem = stride ? stride : ((flags & 1) ? 4 : ConsoleElementBytes(fmt));
        if (!elem) { if (log) XS_LOG("SHADOWVIEW: SRV fmt=%u: размер элемента неизвестен", fmt); return; }
        const uint64_t start = address + first * elem; uint64_t off = 0;
        ID3D12Resource* sh = ShadowFor(dev, start, (uint64_t)num * elem, &off, elem); if (!sh) return;
        if (off % elem) { if (log) XS_LOG("SHADOWVIEW: смещение %#llx не кратно элементу %u", (unsigned long long)off, elem); return; }
        D3D12_SHADER_RESOURCE_VIEW_DESC d = {}; d.ViewDimension = D3D12_SRV_DIMENSION_BUFFER; d.Shader4ComponentMapping = mapping ? mapping : D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        d.Format = stride ? DXGI_FORMAT_UNKNOWN : ((flags & 1) ? DXGI_FORMAT_R32_TYPELESS : XboxFormatToPc((DXGI_FORMAT)fmt));
        d.Buffer.FirstElement = off / elem; d.Buffer.NumElements = num; d.Buffer.StructureByteStride = stride; d.Buffer.Flags = (flags & 1) ? D3D12_BUFFER_SRV_FLAG_RAW : D3D12_BUFFER_SRV_FLAG_NONE;
        dev->CreateShaderResourceView(sh, &d, D3D12_CPU_DESCRIPTOR_HANDLE{ (SIZE_T)real });
        if (log) XS_LOG("SHADOWVIEW: SRV fmt=%u->%d stride=%u num=%u addr=%#llx -> shadow %p +%#llx (слот native %p)", fmt, (int)d.Format, stride, num, (unsigned long long)start, (void*)sh, (unsigned long long)off, (void*)real);
    } else if (log) XS_LOG("SHADOWVIEW: UAV буфер пока не зеркалируется (нужен DEFAULT-буфер и обратная запись)");
}
static HRESULT PlacedViewImpl(uint32_t kind, uintptr_t A, uintptr_t B, uintptr_t C, ID3D12Device* dev = nullptr)
{
    ViewRec r = {}; r.kind = kind;
    if (A && !IsBadReadPtr((void*)A, 64)) memcpy(r.A, (void*)A, 64);
    if (B && !IsBadReadPtr((void*)B, 64)) memcpy(r.B, (void*)B, 64);
    uint64_t base = 0; { SharedSrwLock g(g_addrResLock); const AddrRes* ar = FindAddrRes(r.B[5], &base); if (ar) { r.res = ar->res; r.base = base; } }
    uint32_t idx, gen = 1; { ExclusiveSrwLock g(g_viewLock); idx = (uint32_t)g_views.size(); r.gen = gen; g_views.push_back(r); }
    static bool noWrite = [] { char v[4]; return GetEnvironmentVariableA("XSX_NO_VIEW_WRITE", v, sizeof v) > 0; }();
    if (!noWrite && C && !IsBadWritePtr((void*)C, 12)) { uint32_t* d = (uint32_t*)C; d[0] = kViewTag; d[1] = idx; d[2] = gen; }
    GfxViewTrace("placed-view",dev,__builtin_return_address(0),C,XlatCpu(C),(void*)r.res,nullptr,r.A,sizeof r.A,kind,idx,r.B,sizeof r.B);
    ++g_viewTotal; if (r.res) ++g_viewResolved;
    { static bool on = EnvOn("XSX_SHADOW"); if (on && dev && C && r.A[0] % 0x100000000ull == 1) MirrorPlacedBuffer(dev, kind, r, C); }
    static std::atomic<int> n{0}; int k = n++;
    if (k < 12) XS_LOG("PLACEDVIEW#%u слова ресурса: dim=%llu align=%#llx w=%#llx h=%u dep|mips=%#x fmt=%u samples=%u layout=%#x flags=%#x; вид: fmt=%u dim=%u addr=%#llx", k,
        (unsigned long long)(r.A[0] & 0xffffffff), (unsigned long long)r.A[1], (unsigned long long)r.A[2], (unsigned)(r.A[3] & 0xffffffff), (unsigned)(r.A[3] >> 32), (unsigned)(r.A[4] & 0xffffffff), (unsigned)(r.A[4] >> 32), (unsigned)(r.A[5] & 0xffffffff), (unsigned)(r.A[6] & 0xffffffff),
        (unsigned)(r.B[0] & 0xffffffff), (unsigned)(r.B[0] >> 32), (unsigned long long)r.B[5]);
    if (k < 8 || (k % 5000) == 0) XS_LOG("PLACEDVIEW#%u kind=%u idx=%u dest=%p B[0]=%#llx B[3]=%#llx addr=%#llx -> %s (разрешено %u из %u)", k, kind, idx, (void*)C, (unsigned long long)r.B[0], (unsigned long long)r.B[3], (unsigned long long)r.B[5],
        r.res ? "ресурс найден (смещение внутри)" : "ресурс по адресу НЕ найден", g_viewResolved.load(), g_viewTotal.load());
    return S_OK;
}
static HRESULT __stdcall Dev_PlacedView70(Wrap* w, uintptr_t A, uintptr_t B, uintptr_t C) { static bool off = EnvOn("XSX_NO_PLACEDVIEW"); return off ? S_OK : PlacedViewImpl(0, A, B, C, (ID3D12Device*)w->real); }
static HRESULT __stdcall Dev_PlacedView71(Wrap* w, uintptr_t A, uintptr_t B, uintptr_t C) { static bool off = EnvOn("XSX_NO_PLACEDVIEW"); return off ? S_OK : PlacedViewImpl(1, A, B, C, (ID3D12Device*)w->real); }

// ---- Диагностика: что именно игра привязывает перед Draw/Dispatch (XSX_DRAW_TRACE=1) ----
static void DescribeRecord(uintptr_t addr, char* out, size_t cap)
{
    out[0] = 0; uint32_t rec[3] = {};
    if (!addr) { snprintf(out, cap, "<handle вне известных heap>"); return; }
    if (!ReadProcessMemory(GetCurrentProcess(), (void*)addr, rec, sizeof rec, nullptr)) { snprintf(out, cap, "<нечитаемо %p>", (void*)addr); return; }
    if (rec[0] != kViewTag) { snprintf(out, cap, "raw %08x %08x %08x", rec[0], rec[1], rec[2]); return; }
    ViewRec v; { SharedSrwLock g(g_viewLock); if (rec[1] >= g_views.size()) { snprintf(out, cap, "vVIE idx=%u вне реестра", rec[1]); return; } v = g_views[rec[1]]; }
    snprintf(out, cap, "vVIE idx=%u kind=%u B0=%#llx B3=%#llx addr=%#llx res=%p", rec[1], v.kind, (unsigned long long)v.B[0], (unsigned long long)v.B[3], (unsigned long long)v.B[5], v.res);
}
static void DrawTrace(Wrap* w, const char* what, uint32_t x, uint32_t y)
{
    static std::atomic<unsigned> n{0}; unsigned k = n++;
    if (IsXboxList(w)) {   // новый использованный PSO -> его хеши шейдеров
        uintptr_t pso = 0; { SharedSrwLock g(g_xstateLock); auto it = g_xstate.find(w); if (it != g_xstate.end()) pso = it->second.pso; }
        std::string hs; { SharedSrwLock g(g_psoHashLock); auto it = g_psoHashes.find((void*)pso); if (it != g_psoHashes.end()) hs = it->second; }
        if (!hs.empty()) { bool fresh; { ExclusiveSrwLock g(g_psoHashLock); fresh = g_usedPso.insert(hs).second; } if (fresh) { XS_LOG("USEDPSO %s #%zu (%s)", hs.c_str(), g_usedPso.size(), what);
            std::vector<std::string> bl; { SharedSrwLock g(g_psoHashLock); auto it = g_psoBlobs.find((void*)pso); if (it != g_psoBlobs.end()) bl = it->second; }
            for (size_t b = 0; b < bl.size(); b++) { std::string hx; char t[4]; for (unsigned char c : bl[b].substr(bl[b].find(':') + 1)) { snprintf(t, sizeof t, "%02x", c); hx += t; } XS_LOG("USEDPSO-BLOB #%zu [%s] %zu Б: %s", g_usedPso.size(), bl[b].substr(0, bl[b].find(':')).c_str(), bl[b].size() - bl[b].find(':') - 1, hx.c_str()); } } }
    }
    if (!IsXboxList(w) || !(k < 40 || (k % 3000) == 0)) return;
    XListState st; { SharedSrwLock g(g_xstateLock); auto it = g_xstate.find(w); if (it == g_xstate.end()) return; st = it->second; }
    XS_LOG("DRAWSTATE #%u %s(%u,%u) list=%p pso=%p записей=%zu", k, what, x, y, (void*)w, (void*)st.pso, st.slots.size());
    for (auto& e : st.slots) {
        int slot = e.first.first; auto& v = e.second; char d[300] = {};
        if (slot == 35 || slot == 36) {
            for (int i = 0; i < 3; i++) { char t[120]; uint64_t gst = GpuHandleToGuest(v[1]); DescribeRecord(gst ? gst + 32 * i : 0, t, sizeof t); size_t l = strlen(d); snprintf(d + l, sizeof d - l, "%s[%d] %s", i ? " | " : "", i, t); }
            XS_LOG("  slot %d %s table root=%llu handle=%p: %s", slot, slot == 35 ? "CS" : "GFX", (unsigned long long)v[0], (void*)v[1], d);
        } else if (slot >= 37 && slot <= 40) {
            uint32_t dw[4] = {}; if (slot >= 39) ReadProcessMemory(GetCurrentProcess(), (void*)v[2], dw, sizeof dw, nullptr);
            XS_LOG("  slot %d constants root=%llu b=%llx dwords=%08x %08x %08x %08x", slot, (unsigned long long)v[0], (unsigned long long)v[1], dw[0], dw[1], dw[2], dw[3]);
        } else if (slot >= 41 && slot <= 46) {
            uint64_t base = 0; const AddrRes* ar; { SharedSrwLock g(g_addrResLock); ar = FindAddrRes(v[1], &base); }
            uint32_t mem[24] = {}; SIZE_T got = 0; ReadProcessMemory(GetCurrentProcess(), (void*)v[1], mem, sizeof mem, &got);
            XS_LOG("  slot %d root-view root=%llu addr=%p c=%llx d=%llx -> %s; первые %zu Б: %08x %08x %08x %08x | %08x %08x %08x %08x | %08x %08x %08x %08x | %08x %08x %08x %08x | %08x %08x %08x %08x | %08x %08x %08x %08x", slot, (unsigned long long)v[0], (void*)v[1], (unsigned long long)v[2], (unsigned long long)v[3], ar ? "в ресурсе, найден" : "ресурс не найден", (size_t)got,
                mem[0], mem[1], mem[2], mem[3], mem[4], mem[5], mem[6], mem[7], mem[8], mem[9], mem[10], mem[11], mem[12], mem[13], mem[14], mem[15], mem[16], mem[17], mem[18], mem[19], mem[20], mem[21], mem[22], mem[23]);
        } else XS_LOG("  slot %d args %llx %llx %llx %llx", slot, (unsigned long long)v[0], (unsigned long long)v[1], (unsigned long long)v[2], (unsigned long long)v[3]);
    }
}
static void InstallDeviceReturnTrace()
{
    if (!ReturnTraceMode()) return;
    using namespace xsx::returns;
    TraceObjectBase<1>(g_devVt); // Slot7 is GetNodeCount, so the base helper skips it.
#define TRACE_DEVICE_PC(slot, method) InstallMember<1, slot, Wrap>(g_devVt, &ID3D12Device::method, #method)
    TRACE_DEVICE_PC(10, CreateGraphicsPipelineState);
    TRACE_DEVICE_PC(11, CreateComputePipelineState);
    TRACE_DEVICE_PC(13, CheckFeatureSupport);
    TRACE_DEVICE_PC(16, CreateRootSignature);
    TRACE_DEVICE_PC(27, CreateCommittedResource);
    TRACE_DEVICE_PC(28, CreateHeap);
    TRACE_DEVICE_PC(29, CreatePlacedResource);
    TRACE_DEVICE_PC(30, CreateReservedResource);
    TRACE_DEVICE_PC(31, CreateSharedHandle);
    TRACE_DEVICE_PC(32, OpenSharedHandle);
    TRACE_DEVICE_PC(33, OpenSharedHandleByName);
    TRACE_DEVICE_PC(34, MakeResident);
    TRACE_DEVICE_PC(35, Evict);
    TRACE_DEVICE_PC(37, GetDeviceRemovedReason);
    TRACE_DEVICE_PC(39, CreateQueryHeap);
    TRACE_DEVICE_PC(40, SetStablePowerState);
#undef TRACE_DEVICE_PC
#define TRACE_DEVICE(slot, method) Install<1, slot>(g_devVt, &method, #method)
    TRACE_DEVICE(0, Dev_QI);
    TRACE_DEVICE(8, Dev_CreateCommandQueue8);
    TRACE_DEVICE(9, Dev_CreateCommandAllocator9);
    TRACE_DEVICE(12, Dev_CreateCommandList12);
    TRACE_DEVICE(14, Dev_CreateDescriptorHeap14);
    TRACE_DEVICE(36, Dev_CreateFence36);
    TRACE_DEVICE(41, Dev_CreateCommandSignature41);
    TRACE_DEVICE(48, Dev_CreateCommandQueue48);
    TRACE_DEVICE(55, Dev_PsoUpdates55);
    TRACE_DEVICE(57, Dev_NativePso57);
    TRACE_DEVICE(61, Dev_CreateSamplerBlob61);
    TRACE_DEVICE(62, Dev_NativePso62);
    TRACE_DEVICE(67, Dev_NativePso67);
    TRACE_DEVICE(70, Dev_PlacedView70);
    TRACE_DEVICE(71, Dev_PlacedView71);
    TRACE_DEVICE(73, Dev_CreateCommandList73);
    TRACE_DEVICE(74, Dev_HeapFromRange74);
    TRACE_DEVICE(76, Dev_CreateCommittedResource76);
    TRACE_DEVICE(78, Dev_SetFrameInterval78);
    TRACE_DEVICE(79, Dev_ScheduleFrameEvent79);
    TRACE_DEVICE(80, Dev_WaitFrameEvent80);
    TRACE_DEVICE(81, Dev_QueueInfo81);
    TRACE_DEVICE(86, Dev_NativePso86);
    TRACE_DEVICE(127, Dev_CreateCommittedResource127);
    TRACE_DEVICE(133, Dev_CreatePlacedResourceX133);
#undef TRACE_DEVICE
    XS_LOG("RET tracing mode=%d enabled: typed HRESULT/output contracts, value/refcount returns only in mode2; unknown and void methods keep existing diagnostics", ReturnTraceMode());
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
    // The legacy argument logger casts to a six-argument signature. Do not
    // let it change the ABI underneath the typed return observer.
    if (getenv("XSX_DEV_TRACE") && !ReturnTraceMode()) { g_trace = true; FillTrace(std::make_integer_sequence<int, 44>{}); }
    g_devVt[0] = (void*)Dev_QI; g_devVt[43] = (void*)Dev_GetAdapterLuid; g_devVt[76] = (void*)Dev_CreateCommittedResource76; g_devVt[48] = (void*)Dev_CreateCommandQueue48; g_devVt[9] = (void*)Dev_CreateCommandAllocator9; g_devVt[14] = (void*)Dev_CreateDescriptorHeap14;
    g_devVt[55] = (void*)Dev_PsoUpdates55; g_devVt[57] = (void*)Dev_NativePso57;
    g_devVt[36] = (void*)Dev_CreateFence36;
    g_devVt[41] = (void*)Dev_CreateCommandSignature41; g_devVt[8] = (void*)Dev_CreateCommandQueue8; g_devVt[80] = (void*)Dev_WaitFrameEvent80; g_devVt[81] = (void*)Dev_QueueInfo81; g_devVt[12] = (void*)Dev_CreateCommandList12; g_devVt[25] = (void*)Dev_GetResourceAllocationInfo25; g_devVt[108] = (void*)Dev_GetResourceAllocationInfo108; g_devVt[61] = (void*)Dev_CreateSamplerBlob61; g_devVt[62] = (void*)Dev_NativePso62; g_devVt[74] = (void*)Dev_HeapFromRange74; g_devVt[86] = (void*)Dev_NativePso86; g_devVt[67] = (void*)Dev_NativePso67; g_devVt[70] = (void*)Dev_PlacedView70; g_devVt[71] = (void*)Dev_PlacedView71; g_devVt[127] = (void*)Dev_CreateCommittedResource127; g_devVt[133] = (void*)Dev_CreatePlacedResourceX133; g_devVt[78] = (void*)Dev_SetFrameInterval78; g_devVt[79] = (void*)Dev_ScheduleFrameEvent79;
    g_devVt[17] = (void*)Dev_CreateCBV; g_devVt[18] = (void*)Dev_CreateSRV; g_devVt[19] = (void*)Dev_CreateUAV; g_devVt[20] = (void*)Dev_CreateRTV; g_devVt[21] = (void*)Dev_CreateDSV;
    g_devVt[22] = (void*)Dev_CreateSampler; g_devVt[23] = (void*)Dev_CopyDescriptors; g_devVt[24] = (void*)Dev_CopyDescriptorsSimple; g_devVt[73] = (void*)Dev_CreateCommandList73;
    InstallDeviceReturnTrace();
}

#include "gfx_native.h"

// ---- D3D12XboxCreateDevice(adapter, params, riid, ppDevice) ----
// [Гипотеза] layout D3D12XBOX_CREATE_DEVICE_PARAMETERS начинается с UINT Version (SDK version).
// Остальные поля (ring sizes, debug flags, DXR flags) на PC не имеют аналога и игнорируются.
// ВНИМАНИЕ: возвращается обычный PC device. Xbox vtable (напр. слот 76 CreateCommittedResource)
// и native PSO/resource flags этим слоем НЕ адаптируются — это отдельный ABI-адаптер.
// XSX_NO_WATCHDOG=1 (debugging only): set the game's own watchdog-skip flag (exe+0x71b6340 bit0, tested at
// exe+0x2215ab8 before the heartbeat loop that ends in the deliberate *(0)=0 at exe+0x2215c35).
static void DisableGameWatchdog()
{
    static bool done = false; if (done || !EnvOn("XSX_NO_WATCHDOG")) return; done = true;
    auto* flag = (volatile uint8_t*)((uintptr_t)GetModuleHandleW(nullptr) + 0x71b6340);
    DWORD old = 0; if (VirtualProtect((void*)flag, 1, PAGE_READWRITE, &old)) { *flag |= 1; VirtualProtect((void*)flag, 1, old, &old); }
    XS_LOG("WATCHDOG disabled via game flag exe+0x71b6340 = %#x", *flag);
}
extern "C" HRESULT WINAPI xsXboxCreateDevice(
    IUnknown* adapter, const void* params, REFIID riid, void** ppDevice)
{
    DisableGameWatchdog();
    ConfigureReturnTrace();
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
