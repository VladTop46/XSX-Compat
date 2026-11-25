#pragma once
// profiler.h — XSX_PROF=1: low-overhead frame/section profiler + per-thread CPU sampler.
// Every XSX_PROF_MS (default 2000) ms one PROF block is logged:
//   PROF frames=… fps=… frame_ms avg/max; sections: calls, total ms, max ms (our hot paths only);
//   PROF thread: top threads by CPU% in the window with start address (module+rva) and TID.
// Sections are wall time on the calling thread (includes waits). Nothing is written otherwise.
#include <tlhelp32.h>
enum ProfSection { PROF_PRESENT, PROF_PRESENT_WAIT, PROF_SUBMIT, PROF_DRAWBIND, PROF_DRAW_NOOP, PROF_PSO57, PROF_QUERYAPI, PROF_SUB_REFRESH, PROF_SUB_FENCE, PROF_SUB_EXEC, PROF_SUB_SIGNAL, PROF_SUB_LOCK, PROF_COUNT };
static const char* const kProfNames[PROF_COUNT] = { "present", "present_wait", "submit", "drawbind", "draw_noop", "pso57", "queryapi", "sub_refresh", "sub_fence", "sub_exec", "sub_signal", "sub_lock" };
struct ProfSlot { std::atomic<uint64_t> calls{0}, ticks{0}, maxTicks{0}; };
static ProfSlot g_prof[PROF_COUNT];
static std::atomic<uint64_t> g_profFrames{0}, g_profFrameMaxTicks{0}, g_profLastFrame{0};
static bool ProfOn() { static bool on = EnvOn("XSX_PROF"); return on; }
static uint64_t ProfNow() { LARGE_INTEGER c; QueryPerformanceCounter(&c); return (uint64_t)c.QuadPart; }
static double ProfMs(uint64_t t) { static double f = [] { LARGE_INTEGER q; QueryPerformanceFrequency(&q); return 1000.0 / (double)q.QuadPart; }(); return (double)t * f; }
struct ProfScope {
    ProfSection s; uint64_t t0;
    explicit ProfScope(ProfSection sec) : s(sec), t0(ProfOn() ? ProfNow() : 0) {}
    ~ProfScope() {
        if (!t0) return;
        const uint64_t d = ProfNow() - t0; auto& p = g_prof[s]; p.calls++; p.ticks += d;
        uint64_t m = p.maxTicks.load(); while (d > m && !p.maxTicks.compare_exchange_weak(m, d)) {}
    }
};
#define PROF_SCOPE(sec) ProfScope prof_scope_##__LINE__(sec)
static void ProfFrame()
{
    if (!ProfOn()) return;
    const uint64_t now = ProfNow(), last = g_profLastFrame.exchange(now);
    g_profFrames++;
    if (last) { const uint64_t d = now - last; uint64_t m = g_profFrameMaxTicks.load(); while (d > m && !g_profFrameMaxTicks.compare_exchange_weak(m, d)) {} }
}
static void ProfThreadName(DWORD tid, char* out, size_t n)
{
    snprintf(out, n, "?");
    HANDLE h = OpenThread(THREAD_QUERY_INFORMATION, FALSE, tid); if (!h) return;
    typedef LONG (WINAPI* NtQIT)(HANDLE, int, void*, ULONG, ULONG*);
    static NtQIT q = (NtQIT)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationThread");
    void* start = nullptr; if (q) q(h, 9 /*ThreadQuerySetWin32StartAddress*/, &start, sizeof start, nullptr);
    CloseHandle(h);
    HMODULE mod = nullptr; char name[MAX_PATH] = "?";
    if (start && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)start, &mod)) {
        GetModuleFileNameA(mod, name, sizeof name); const char* b = strrchr(name, '\\');
        snprintf(out, n, "%s+%#llx", b ? b + 1 : name, (unsigned long long)((uintptr_t)start - (uintptr_t)mod));
    } else snprintf(out, n, "%p", start);
}
static DWORD WINAPI ProfThread(void*)
{
    DWORD every = 2000; { char v[16]; if (GetEnvironmentVariableA("XSX_PROF_MS", v, sizeof v)) every = (DWORD)atoi(v); if (every < 200) every = 200; }
    std::map<DWORD, uint64_t> prevCpu; uint64_t prevWall = ProfNow();
    const DWORD self = GetCurrentProcessId();
    for (;;) {
        Sleep(every);
        const uint64_t wall = ProfNow(); const double wallMs = ProfMs(wall - prevWall); prevWall = wall;
        const uint64_t frames = g_profFrames.exchange(0), fmax = g_profFrameMaxTicks.exchange(0);
        char line[1600]; int p = snprintf(line, sizeof line, "PROF window=%.0fms frames=%llu fps=%.1f frame_ms_avg=%.1f frame_ms_max=%.1f |",
            wallMs, (unsigned long long)frames, frames * 1000.0 / wallMs, frames ? wallMs / frames : 0.0, ProfMs(fmax));
        for (int i = 0; i < PROF_COUNT; i++) {
            const uint64_t c = g_prof[i].calls.exchange(0), t = g_prof[i].ticks.exchange(0), m = g_prof[i].maxTicks.exchange(0);
            if (c) p += snprintf(line + p, sizeof line - p, " %s=%llux/%.1fms(max %.1f)", kProfNames[i], (unsigned long long)c, ProfMs(t), ProfMs(m));
        }
        XS_LOG("%s", line);
        // Per-thread CPU (kernel+user) in this window.
        struct T { DWORD tid; uint64_t cpu; };
        std::vector<T> top; std::map<DWORD, uint64_t> cur;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap != INVALID_HANDLE_VALUE) {
            THREADENTRY32 te = { sizeof te };
            for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
                if (te.th32OwnerProcessID != self) continue;
                HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID); if (!h) continue;
                FILETIME c, e, k, u; if (GetThreadTimes(h, &c, &e, &k, &u)) {
                    const uint64_t t = ((uint64_t)k.dwHighDateTime << 32 | k.dwLowDateTime) + ((uint64_t)u.dwHighDateTime << 32 | u.dwLowDateTime);
                    cur[te.th32ThreadID] = t; auto it = prevCpu.find(te.th32ThreadID);
                    if (it != prevCpu.end() && t > it->second) top.push_back({te.th32ThreadID, t - it->second});
                }
                CloseHandle(h);
            }
            CloseHandle(snap);
        }
        prevCpu.swap(cur);
        std::sort(top.begin(), top.end(), [](const T& a, const T& b) { return a.cpu > b.cpu; });
        double total = 0; for (auto& t : top) total += t.cpu / 10000.0;
        p = snprintf(line, sizeof line, "PROF threads=%zu cpu_total=%.0f%% top:", top.size(), total * 100.0 / wallMs);
        for (size_t i = 0; i < top.size() && i < 8; i++) {
            char name[160]; ProfThreadName(top[i].tid, name, sizeof name);
            p += snprintf(line + p, sizeof line - p, " [%lu %s %.0f%%]", (unsigned long)top[i].tid, name, top[i].cpu / 10000.0 * 100.0 / wallMs);
        }
        XS_LOG("%s", line);
    }
    return 0;
}
static void ProfStart()
{
    static std::once_flag once;
    std::call_once(once, [] { if (ProfOn()) { CloseHandle(CreateThread(nullptr, 0, ProfThread, nullptr, 0, nullptr)); XS_LOG("PROF enabled"); } });
}
// ---- per-command-list op counters (XSX_PROF=1): explain slow submissions ----
enum ProfOp { OP_DRAW, OP_DISPATCH, OP_COPYBUF, OP_COPYTEX, OP_COPYRES, OP_COPYTILES, OP_EXECINDIRECT, OP_BARRIER, OP_WRITE, OP_COUNT };
static const char* const kOpNames[OP_COUNT] = { "draw", "dispatch", "copybuf", "copytex", "copyres", "copytiles", "execindirect", "barrier", "write" };
struct ProfListOps { uint32_t n[OP_COUNT] = {}; uint64_t copyBytes = 0; uint64_t dispatchGroups = 0; };
static std::map<void*, ProfListOps> g_profOps; static SRWLOCK g_profOpsLock = SRWLOCK_INIT;
static void ProfListOp(void* list, ProfOp op, uint64_t bytes = 0, uint64_t groups = 0)
{
    if (!ProfOn()) return;
    AcquireSRWLockExclusive(&g_profOpsLock); auto& o = g_profOps[list]; o.n[op]++; o.copyBytes += bytes; o.dispatchGroups += groups; ReleaseSRWLockExclusive(&g_profOpsLock);
}
static void ProfListReset(void* list) { if (!ProfOn()) return; AcquireSRWLockExclusive(&g_profOpsLock); g_profOps.erase(list); ReleaseSRWLockExclusive(&g_profOpsLock); }
static void ProfSlowSubmit(void* const* lists, unsigned count, double execMs, double waitMs, size_t writes)
{
    if (!ProfOn()) return;
    static std::atomic<unsigned> logged{0}; if (logged++ >= 40) return;
    ProfListOps sum; AcquireSRWLockShared(&g_profOpsLock);
    for (unsigned i = 0; i < count; i++) { auto it = g_profOps.find(lists[i]); if (it == g_profOps.end()) continue;
        for (int k = 0; k < OP_COUNT; k++) sum.n[k] += it->second.n[k]; sum.copyBytes += it->second.copyBytes; sum.dispatchGroups += it->second.dispatchGroups; }
    ReleaseSRWLockShared(&g_profOpsLock);
    char line[600]; int p = snprintf(line, sizeof line, "PROF slow-submit lists=%u exec=%.1fms gpu_wait=%.1fms writes=%zu ops:", count, execMs, waitMs, writes);
    for (int k = 0; k < OP_COUNT; k++) if (sum.n[k]) p += snprintf(line + p, sizeof line - p, " %s=%u", kOpNames[k], sum.n[k]);
    snprintf(line + p, sizeof line - p, " copy_bytes=%llu dispatch_groups=%llu", (unsigned long long)sum.copyBytes, (unsigned long long)sum.dispatchGroups);
    XS_LOG("%s", line);
}
